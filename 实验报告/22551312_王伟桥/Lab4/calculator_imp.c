/*==============================================================================
  Copyright ...
==============================================================================*/
#ifndef _DEBUG
#define _DEBUG
#endif

#define THREAD_COUNT 6
#define VTCM_ENABLED 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "HAP_farf.h"
#include "calculator.h"

#include "HAP_perf.h"
#include "HAP_farf.h"
#include "HAP_power.h"
#include "HAP_compute_res.h"

#include "AEEStdErr.h"
#include "hexagon_types.h"
#include "hexagon_protos.h"

#ifdef __cplusplus
#  if defined(__GNUC__)
#    define GGML_RESTRICT __restrict__
#  elif defined(__clang__)
#    define GGML_RESTRICT __restrict
#  elif defined(_MSC_VER)
#    define GGML_RESTRICT __restrict
#  else
#    define GGML_RESTRICT
#  endif
#else
#  if defined (_MSC_VER) && (__STDC_VERSION__ < 201112L)
#    define GGML_RESTRICT __restrict
#  else
#    define GGML_RESTRICT restrict
#  endif
#endif

/* ===================== 开关：是否启用 HVX 实现 ===================== */
#ifndef USE_HVX
#define USE_HVX 1   /* 1: 在 Hexagon 上使用 HVX；0: 始终使用朴素实现 */
#endif

/* ===================== RPC Open/Close ===================== */
int calculator_open(const char*uri, remote_handle64* handle) {
    void *tptr = (void *)malloc(1);
    *handle = (remote_handle64)tptr;
    assert(*handle);
    return 0;
}

int calculator_close(remote_handle64 handle) {
    if (handle) free((void*)handle);
    return 0;
}

/* ===================== 朴素三重循环（baseline） ===================== */
/* C[m,n] = A[m,k] * B[k,n]  （B 按 k×n 行主序）*/
static inline void
matmul_ijk(float * GGML_RESTRICT A,
           float * GGML_RESTRICT B,
           float * GGML_RESTRICT C,
           uint32_t m, uint32_t k, uint32_t n) {
    for (uint32_t i = 0; i < m; ++i) {
        for (uint32_t j = 0; j < n; ++j) {
            float sum = 0.f;
            for (uint32_t l = 0; l < k; ++l) {
                sum += A[i * k + l] * B[l * n + j];
            }
            C[i * n + j] = sum;
        }
    }
}

/* C[m,n] = A[m,k] * B^T[n,k]  （第二个矩阵以 n×k 行主序提供/访问）*/
static inline void
matmul_ikj_transposed_b(float * GGML_RESTRICT A,
                        float * GGML_RESTRICT BT, /* 逻辑上是 B^T，大小 n×k */
                        float * GGML_RESTRICT C,
                        uint32_t m, uint32_t k, uint32_t n) {
    for (uint32_t i = 0; i < m; ++i) {
        for (uint32_t j = 0; j < n; ++j) {
            float sum = 0.f;
            for (uint32_t l = 0; l < k; ++l) {
                sum += A[i * k + l] * BT[j * k + l];
            }
            C[i * n + j] = sum;
        }
    }
}

/* ===================== HVX 工具函数 ===================== */
static inline int32_t float_to_bits(float x) {
    union { float f; int32_t i; } u = { .f = x };
    return u.i;
}

#if USE_HVX && defined(__HEXAGON_ARCH__)
/* HVX 载入/存储（对齐可选；不强制 128B）*/
#define HVX_BYTES 128
#define HVX_F32_PER_VEC 32

static inline HVX_Vector vloadu_f32(const float *p) {
    HVX_Vector v;
    /* 使用 memcpy 规避对齐限制，便于通用处理 */
    memcpy(&v, p, HVX_BYTES);
    return v;
}

static inline void vstoreu_f32(float *p, HVX_Vector v) {
    memcpy(p, &v, HVX_BYTES);
}

/* 将 qf32 向量做水平求和为一个标量（树形归约 + 旋转）*/
static inline float hvx_reduce_add_qf32(HVX_Vector vq) {
    /* 每步把后半段旋转到前半并相加：16、8、4、2、1 个 float 的粒度 */
    vq = Q6_Vqf32_vadd_Vqf32Vqf32(vq, Q6_V_vror_VR(vq, 16 * sizeof(float)));
    vq = Q6_Vqf32_vadd_Vqf32Vqf32(vq, Q6_V_vror_VR(vq,  8 * sizeof(float)));
    vq = Q6_Vqf32_vadd_Vqf32Vqf32(vq, Q6_V_vror_VR(vq,  4 * sizeof(float)));
    vq = Q6_Vqf32_vadd_Vqf32Vqf32(vq, Q6_V_vror_VR(vq,  2 * sizeof(float)));
    vq = Q6_Vqf32_vadd_Vqf32Vqf32(vq, Q6_V_vror_VR(vq,  1 * sizeof(float)));
    /* 转回 sf 并取第 0 个元素 */
    HVX_Vector vsf = Q6_Vsf_equals_Vqf32(vq);
    float tmp[HVX_F32_PER_VEC];
    vstoreu_f32(tmp, vsf);
    return tmp[0];
}

/* ===================== HVX 外积法：C=A*B （transY==0） ===================== */
static inline void
gemm_hvx_outer(const float * GGML_RESTRICT A,
               const float * GGML_RESTRICT B,   /* k×n */
               float       * GGML_RESTRICT C,   /* m×n */
               uint32_t m, uint32_t k, uint32_t n) {
    const uint32_t n_vec = (n / HVX_F32_PER_VEC) * HVX_F32_PER_VEC;

    for (uint32_t i = 0; i < m; ++i) {
        /* 1) 处理整向量块（列方向一口气 32 个） */
        for (uint32_t j = 0; j < n_vec; j += HVX_F32_PER_VEC) {
            HVX_Vector acc_q = Q6_V_vzero(); /* qf32 累加器 */

            for (uint32_t l = 0; l < k; ++l) {
                /* 广播 A[i,l] */
                HVX_Vector a_brd = Q6_V_vsplat_R(float_to_bits(A[i * k + l]));
                /* 取 B[l, j..j+31] */
                const float *bptr = &B[l * n + j];
                HVX_Vector b_vec  = vloadu_f32(bptr);
                /* 向量乘并累加到 qf32 */
                HVX_Vector prod_q = Q6_Vqf32_vmpy_VsfVsf(a_brd, b_vec);
                acc_q = Q6_Vqf32_vadd_Vqf32Vqf32(acc_q, prod_q);
            }
            /* 写回 C[i, j..j+31] */
            HVX_Vector acc_sf = Q6_Vsf_equals_Vqf32(acc_q);
            vstoreu_f32(&C[i * n + j], acc_sf);
        }

        /* 2) 尾部（不足 32 列）走标量 */
        for (uint32_t j = n_vec; j < n; ++j) {
            float sum = 0.f;
            for (uint32_t l = 0; l < k; ++l) {
                sum += A[i * k + l] * B[l * n + j];
            }
            C[i * n + j] = sum;
        }
    }
}

/* ===================== HVX 内积法：C=A*B^T （transY==1） ===================== */
static inline void
gemm_hvx_dot(const float * GGML_RESTRICT A,    /* m×k */
             const float * GGML_RESTRICT BT,   /* n×k （B^T 的行主序）*/
             float       * GGML_RESTRICT C,    /* m×n */
             uint32_t m, uint32_t k, uint32_t n) {

    const uint32_t k_vec = (k / HVX_F32_PER_VEC) * HVX_F32_PER_VEC;

    for (uint32_t i = 0; i < m; ++i) {
        const float *ai = &A[i * k];
        for (uint32_t j = 0; j < n; ++j) {
            const float *btj = &BT[j * k];

            /* 分块做向量点积累加到 qf32 向量寄存器，再水平归约为 1 个标量 */
            HVX_Vector acc_q = Q6_V_vzero();
            uint32_t l = 0;
            for (; l < k_vec; l += HVX_F32_PER_VEC) {
                HVX_Vector va = vloadu_f32(ai  + l);
                HVX_Vector vb = vloadu_f32(btj + l);
                HVX_Vector prod_q = Q6_Vqf32_vmpy_VsfVsf(va, vb);
                acc_q = Q6_Vqf32_vadd_Vqf32Vqf32(acc_q, prod_q);
            }

            float sum = hvx_reduce_add_qf32(acc_q);

            /* 尾部不足 32 的部分用标量补齐 */
            for (; l < k; ++l) {
                sum += ai[l] * btj[l];
            }
            C[i * n + j] = sum;
        }
    }
}
#endif /* USE_HVX && __HEXAGON_ARCH__ */

/* ===================== 顶层 GEMM 分发 ===================== */
int calculator_gemm(remote_handle64 h,
                    const float* input_matrix1, int input_matrix1Len,
                    const float* input_matrix2, int input_matrix2Len,
                    float* output,         int outputLen,
                    uint32_t m, uint32_t k, uint32_t n,
                    boolean transX, boolean transY) {

    (void)h; (void)transX; /* 未使用 */

    if (m == 0 || k == 0 || n == 0) return 0;
    if (input_matrix1Len < (int)(m * k)) return AEE_EBADPARM;
    if (input_matrix2Len < (int)(k * n)) return AEE_EBADPARM;
    if (outputLen        < (int)(m * n)) return AEE_EBADPARM;

#if USE_HVX && defined(__HEXAGON_ARCH__)
    /* 在 Hexagon 上：HVX 外积 / HVX 内积分支 */
    if (transY) {
        gemm_hvx_dot( (const float*)input_matrix1,
                      (const float*)input_matrix2,
                      output, m, k, n);
    } else {
        gemm_hvx_outer( (const float*)input_matrix1,
                        (const float*)input_matrix2,
                        output, m, k, n);
    }
#else
    /* 非 Hexagon 或关闭 HVX：朴素三重循环 */
    if (transY) {
        matmul_ikj_transposed_b((float*)input_matrix1,
                                (float*)input_matrix2,
                                output, m, k, n);
    } else {
        matmul_ijk((float*)input_matrix1,
                   (float*)input_matrix2,
                   output, m, k, n);
    }
#endif
    return 0;
}

/* ===================== 模拟器自测（可保留，设备端不会用到） ===================== */
#define SIMULATOR_TEST
#ifdef SIMULATOR_TEST
static int verify_naive(float *A, float *B, float *C,
                        uint32_t m, uint32_t k, uint32_t n, int transY) {
    int ok = 1;
    int output_len = (int)(m * n);
    float *ref = (float*)malloc(output_len * sizeof(float));
    if (!ref) return 0;

    if (transY) matmul_ikj_transposed_b(A,B,ref,m,k,n);
    else        matmul_ijk(A,B,ref,m,k,n);

    for (int idx = 0; idx < output_len; ++idx) {
        float diff = ref[idx] - C[idx];
        if (diff < 0) diff = -diff;
        if (diff > 1e-3f) { ok = 0; break; }
    }
    free(ref);
    return ok;
}

static int run_single_test(uint32_t m, uint32_t k, uint32_t n, int transY) {
    int input1_len = (int)(m * k);
    int input2_len = (int)(k * n);
    int output_len = (int)(m * n);
    const int align_size = 128;

    float* A = (float*)memalign(align_size, input1_len * sizeof(float));
    float* B = (float*)memalign(align_size, input2_len * sizeof(float));
    float* C = (float*)memalign(align_size, output_len * sizeof(float));
    if (!A || !B || !C) { printf("alloc fail\n"); free(A); free(B); free(C); return -1; }

    for (int i = 0; i < input1_len; ++i) A[i] = 1.0f;
    for (int i = 0; i < input2_len; ++i) B[i] = 2.0f;
    memset(C, 0, output_len * sizeof(float));

    printf("\nCalling calculator_gemm (transY=%d)...\n", transY);
    unsigned int t0 = HAP_perf_get_time_us();
    int ret = calculator_gemm(0, A, input1_len, B, input2_len, C, output_len,
                              m, k, n, FALSE, transY ? TRUE : FALSE);
    unsigned int t1 = HAP_perf_get_time_us();
    unsigned int ms = (t1 - t0) / 1000;

    if (ret == 0) {
        printf("GEMM executed successfully. Time: %u ms\n", ms);
        int ok = verify_naive(A, B, C, m, k, n, transY);
        printf("Verification: %s (transY=%d)\n", ok ? "PASSED" : "FAILED", transY);
    } else {
        printf("GEMM FAILED with error %d\n", ret);
    }
    free(A); free(B); free(C);
    return ret;
}

int main(int argc, char* argv[]) {
    printf("=====================================\n");
    if (argc != 4) {
        printf("Usage: %s <M> <K> <N>\n", argv[0]);
        return -1;
    }
    uint32_t m = atoi(argv[1]), k = atoi(argv[2]), n = atoi(argv[3]);
    printf("Starting GEMM test in simulator: M=%lu, K=%lu, N=%lu\n", m, k, n);

    int r0 = run_single_test(m, k, n, 0); /* A*B  ：HVX外积 / 朴素A*B */
    int r1 = run_single_test(m, k, n, 1); /* A*B^T：HVX内积 / 朴素A*B^T */

    printf("=====================================\n");
    return (r0 == 0 && r1 == 0) ? 0 : -1;
}
#endif /* SIMULATOR_TEST */
