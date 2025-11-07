/*==============================================================================
  Copyright (c) 2012-2020 Qualcomm Technologies, Inc.
  All rights reserved. Qualcomm Proprietary and Confidential.
==============================================================================*/
#ifndef _DEBUG
#define _DEBUG
#endif

#define THREAD_COUNT 6

#define VTCM_ENABLED 1

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

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
    // restrict not standard in C++
#    if defined(__GNUC__)
#        define GGML_RESTRICT __restrict__
#    elif defined(__clang__)
#        define GGML_RESTRICT __restrict
#    elif defined(_MSC_VER)
#        define GGML_RESTRICT __restrict
#    else
#        define GGML_RESTRICT
#    endif
#else
#    if defined (_MSC_VER) && (__STDC_VERSION__ < 201112L)
#        define GGML_RESTRICT __restrict
#    else
#        define GGML_RESTRICT restrict
#    endif
#endif

/* --- HVX configuration --- */
/* VLEN: number of floats per HVX vector */
#define VLEN 32
#define VLEN_BYTES (VLEN * sizeof(float))
#define IS_ALIGNED_128(ptr) ((((uintptr_t)(ptr)) & (VLEN_BYTES - 1)) == 0)

/* allow enabling HVX implementation at compile time:
   compile with -DUSE_HVX to use HVX implementations */
#ifdef USE_HVX
#  define HVX_ENABLED 1
#else
#  define HVX_ENABLED 0
#endif

int calculator_open(const char*uri, remote_handle64* handle) {
   void *tptr = NULL;
   tptr = (void *)malloc(1);
   *handle = (remote_handle64)tptr;
   assert(*handle);
   return 0;
}

/**
 * @param handle, the value returned by open
 * @retval, 0 for success, should always succeed
 */
int calculator_close(remote_handle64 handle) {
   if (handle)
      free((void*)handle);
   return 0;
}

/* helper: get bit-pattern of float */
static inline int32_t float_to_bits(float f) {
    union { float f; int32_t i; } u;
    u.f = f;
    return u.i;
}

/* -------------------------
   Baseline scalar matmul
   ------------------------- */
static inline void matmul_ijk(float *GGML_RESTRICT A,
                              float *GGML_RESTRICT B,
                              float *GGML_RESTRICT C,
                              uint32_t M, uint32_t K, uint32_t N) {
    for (uint32_t i = 0; i < M; ++i) {
        for (uint32_t j = 0; j < N; ++j) {
            float s = 0.0f;
            const float *a_row = A + (size_t)i * K;
            const float *b_col = B + j;
            for (uint32_t l = 0; l < K; ++l) {
                s += a_row[l] * b_col[(size_t)l * N];
            }
            C[(size_t)i * N + j] = s;
        }
    }
}

/* -------------------------
   Baseline matmul where B is stored transposed (B^T layout: N x K)
   Equivalent to inner-product style using B^T.
   ------------------------- */
static inline void matmul_ikj_transposed_b(float *GGML_RESTRICT A,
                                           float *GGML_RESTRICT B_T,
                                           float *GGML_RESTRICT C,
                                           uint32_t M, uint32_t K, uint32_t N) {
    for (uint32_t i = 0; i < M; ++i) {
        const float *a_row = A + (size_t)i * K;
        for (uint32_t j = 0; j < N; ++j) {
            const float *b_t_row = B_T + (size_t)j * K; /* B_T row holds original B column */
            float s = 0.0f;
            for (uint32_t l = 0; l < K; ++l) {
                s += a_row[l] * b_t_row[l];
            }
            C[(size_t)i * N + j] = s;
        }
    }
}

#if HVX_ENABLED

/* -------------------------
   HVX helpers (original implementations)
   ------------------------- */

/* hvx vector type assumed from hexagon headers */
typedef HVX_Vector hvx_vec_t;

/* Create a vector filled with all-zero pattern */
static inline hvx_vec_t hvx_vzero(void) {
    return Q6_V_vzero();
}

/* Broadcast scalar float into an HVX vector (we pass bits) */
static inline hvx_vec_t hvx_vsplat_floatbits(float scalar) {
    int32_t bits = float_to_bits(scalar);
    return Q6_V_vsplat_R(bits);
}

/* Load 32 floats from possibly unaligned source into hvx vector.
   We guarantee safe load by copying into an aligned local buffer when needed. */
static inline hvx_vec_t hvx_load_f32_aligned(const float *src) {
    if (IS_ALIGNED_128(src)) {
        return *((const hvx_vec_t *)src);
    } else {
        __attribute__((aligned(VLEN_BYTES))) float tmp[VLEN];
        memcpy(tmp, src, VLEN_BYTES);
        return *((const hvx_vec_t *)tmp);
    }
}

/* Store hvx vector to possibly unaligned destination */
static inline void hvx_store_f32_aligned(float *dst, hvx_vec_t v) {
    if (IS_ALIGNED_128(dst)) {
        *((hvx_vec_t *)dst) = v;
    } else {
        __attribute__((aligned(VLEN_BYTES))) float tmp[VLEN];
        *((hvx_vec_t *)tmp) = v;
        memcpy(dst, tmp, VLEN_BYTES);
    }
}

/* Reduce a QF32-style vector (in our usage the accumulator returned by vmpy/vadd)
   to a single scalar float. Implementation uses rotate + add steps.
   This function is intentionally written in original style. */
static inline float hvx_reduce_sum_qf32(hvx_vec_t v_qf32) {
    /* We'll perform log2(VLEN) rotate-add steps.
       Each rotate's amount is given in bytes. */
    // step sizes (in elements) for VLEN=32: 16,8,4,2,1
    hvx_vec_t tmp;
    tmp = Q6_V_vror_VR(v_qf32, (16 * sizeof(float)));
    v_qf32 = Q6_Vqf32_vadd_Vqf32Vqf32(v_qf32, tmp);

    tmp = Q6_V_vror_VR(v_qf32, (8 * sizeof(float)));
    v_qf32 = Q6_Vqf32_vadd_Vqf32Vqf32(v_qf32, tmp);

    tmp = Q6_V_vror_VR(v_qf32, (4 * sizeof(float)));
    v_qf32 = Q6_Vqf32_vadd_Vqf32Vqf32(v_qf32, tmp);

    tmp = Q6_V_vror_VR(v_qf32, (2 * sizeof(float)));
    v_qf32 = Q6_Vqf32_vadd_Vqf32Vqf32(v_qf32, tmp);

    tmp = Q6_V_vror_VR(v_qf32, (1 * sizeof(float)));
    v_qf32 = Q6_Vqf32_vadd_Vqf32Vqf32(v_qf32, tmp);

    /* now every lane holds the total sum (in QF32-format); convert to SF */
    hvx_vec_t v_sf = Q6_Vsf_equals_Vqf32(v_qf32);

    /* extract lane 0 */
    __attribute__((aligned(VLEN_BYTES))) float tmp_out[VLEN];
    *((hvx_vec_t *)tmp_out) = v_sf;
    return tmp_out[0];
}

/* -------------------------
   HVX outer-product (ijk) implementation:
   For each A[i][l] (scalar), broadcast to vector and multiply with B row segment B[l][j..j+31],
   accumulate to C[i][j..j+31].
   ------------------------- */
static inline void matmul_ijk_hvx_outer(float *GGML_RESTRICT A,
                                       float *GGML_RESTRICT B,
                                       float *GGML_RESTRICT C,
                                       uint32_t M, uint32_t K, uint32_t N) {
    /* Process rows of A one by one */
    for (uint32_t i = 0; i < M; ++i) {
        /* For each 32-wide vector block along N */
        uint32_t j = 0;
        uint32_t n_blocks = N / VLEN;
        for (uint32_t b = 0; b < n_blocks; ++b) {
            j = b * VLEN;
            /* initialize accumulator as zero (QF32 domain) */
            hvx_vec_t acc = hvx_vzero();
            /* accumulate over k dimension */
            for (uint32_t l = 0; l < K; ++l) {
                float a_scalar = A[(size_t)i * K + l];
                hvx_vec_t a_bcast = hvx_vsplat_floatbits(a_scalar);
                /* load B[l][j..j+31] -- B row is at B + l*N */
                const float *pB = B + (size_t)l * N + j;
                hvx_vec_t b_vec = hvx_load_f32_aligned(pB);
                /* multiply (QF32 result) and add */
                hvx_vec_t mul = Q6_Vqf32_vmpy_VsfVsf(a_bcast, b_vec);
                acc = Q6_Vqf32_vadd_Vqf32Vqf32(acc, mul);
            }
            /* acc is QF32; convert to SF vector and store to C[i][j..] */
            hvx_vec_t out_sf = Q6_Vsf_equals_Vqf32(acc);
            hvx_store_f32_aligned(C + (size_t)i * N + j, out_sf);
        }
        /* tail remainder in N */
        uint32_t j_tail = n_blocks * VLEN;
        for (uint32_t jj = j_tail; jj < N; ++jj) {
            float s = 0.0f;
            const float *a_row = A + (size_t)i * K;
            const float *b_col = B + jj;
            for (uint32_t l = 0; l < K; ++l) {
                s += a_row[l] * b_col[(size_t)l * N];
            }
            C[(size_t)i * N + jj] = s;
        }
    }
}

/* -------------------------
   HVX inner-product style (B transposed) implementation:
   For each C[i][j], compute dot(A[i, l:l+31], B_T[j, l:l+31]) vectorized:
   - load A row segment and B_T row segment (both length 32)
   - vector multiply, accumulate into QF32 accumulator
   - after k blocks reduce accumulator to scalar when finishing k-blocks
   ------------------------- */
static inline void matmul_ikj_hvx_inner_transposed_b(float *GGML_RESTRICT A,
                                                    float *GGML_RESTRICT B_T,
                                                    float *GGML_RESTRICT C,
                                                    uint32_t M, uint32_t K, uint32_t N) {
    for (uint32_t i = 0; i < M; ++i) {
        for (uint32_t j = 0; j < N; ++j) {
            /* QF32 accumulator vector */
            hvx_vec_t acc = hvx_vzero();
            uint32_t l = 0;
            uint32_t k_blocks = K / VLEN;
            for (uint32_t bl = 0; bl < k_blocks; ++bl) {
                l = bl * VLEN;
                const float *pA = A + (size_t)i * K + l;
                const float *pB_T = B_T + (size_t)j * K + l;
                hvx_vec_t a_v = hvx_load_f32_aligned(pA);
                hvx_vec_t b_v = hvx_load_f32_aligned(pB_T);
                hvx_vec_t mul = Q6_Vqf32_vmpy_VsfVsf(a_v, b_v);
                acc = Q6_Vqf32_vadd_Vqf32Vqf32(acc, mul);
            }
            /* reduce vector accumulator to scalar */
            float vec_sum = hvx_reduce_sum_qf32(acc);
            /* tail over K remainder */
            float tail = 0.0f;
            for (uint32_t tt = k_blocks * VLEN; tt < K; ++tt) {
                tail += A[(size_t)i * K + tt] * B_T[(size_t)j * K + tt];
            }
            C[(size_t)i * N + j] = vec_sum + tail;
        }
    }
}

#endif /* HVX_ENABLED */

/* -------------------------
   Top-level GEMM entry
   - uses HVX implementations if compiled with -DUSE_HVX
   - else uses scalar baselines
   ------------------------- */
int calculator_gemm(remote_handle64 h,
                    const float* input_matrix1,
                    int input_matrix1Len,
                    const float* input_matrix2,
                    int input_matrix2Len,
                    float* output,
                    int outputLen,
                    uint32_t m,
                    uint32_t k,
                    uint32_t n,
                    boolean transX,
                    boolean transY) {

    if (m == 0 || k == 0 || n == 0) {
        return 0;
    }
    if (input_matrix1Len < (int)((size_t)m * k)) {
        return AEE_EBADPARM;
    }
    if (input_matrix2Len < (int)((size_t)k * n)) {
        return AEE_EBADPARM;
    }
    if (outputLen < (int)((size_t)m * n)) {
        return AEE_EBADPARM;
    }

#if HVX_ENABLED
    /* HVX path */
    if (transY) {
        /* caller promises input_matrix2 is B^T layout (N x K) */
        matmul_ikj_hvx_inner_transposed_b((float*)input_matrix1,
                                         (float*)input_matrix2,
                                         output,
                                         m, k, n);
    } else {
        matmul_ijk_hvx_outer((float*)input_matrix1,
                             (float*)input_matrix2,
                             output,
                             m, k, n);
    }
#else
    /* scalar path */
    if (transY) {
        matmul_ikj_transposed_b((float*)input_matrix1,
                                (float*)input_matrix2,
                                output,
                                m, k, n);
    } else {
        matmul_ijk((float*)input_matrix1,
                   (float*)input_matrix2,
                   output, m, k, n);
    }
#endif

    return 0;
}

#define SIMULATOR_TEST
#ifdef SIMULATOR_TEST

/* naive reference for verification (keeps original logic) */
static int verify_naive(float *A, float *B, float *C, uint32_t m, uint32_t k, uint32_t n, int transY) {
    int ok = 1;
    size_t out_len = (size_t)m * n;
    float *ref = (float*)malloc(out_len * sizeof(float));
    if (!ref) return 0;
    for (uint32_t i = 0; i < m; ++i) {
        for (uint32_t j = 0; j < n; ++j) {
            float s = 0.0f;
            if (transY) {
                for (uint32_t l = 0; l < k; ++l)
                    s += A[(size_t)i * k + l] * B[(size_t)j * k + l];
            } else {
                for (uint32_t l = 0; l < k; ++l)
                    s += A[(size_t)i * k + l] * B[(size_t)l * n + j];
            }
            ref[(size_t)i * n + j] = s;
        }
    }

    for (size_t idx = 0; idx < out_len; ++idx) {
        float a = ref[idx];
        float b = C[idx];
        float diff = a - b;
        if (diff < 0) diff = -diff;
        if (diff > 1e-3f) {
            ok = 0;
            break;
        }
    }
    free(ref);
    return ok;
}

/* A slightly more robust single-run test with warmup and averaging */
static int run_single_test(uint32_t m, uint32_t k, uint32_t n, int transY) {
    size_t input1_len = (size_t)m * k;
    size_t input2_len = (size_t)k * n;
    size_t output_len = (size_t)m * n;
    const size_t align_size = 128;

    float *matrix1 = NULL;
    float *matrix2 = NULL;
    float *output_matrix = NULL;

    if (posix_memalign((void**)&matrix1, align_size, input1_len * sizeof(float)) != 0) matrix1 = NULL;
    if (posix_memalign((void**)&matrix2, align_size, input2_len * sizeof(float)) != 0) matrix2 = NULL;
    if (posix_memalign((void**)&output_matrix, align_size, output_len * sizeof(float)) != 0) output_matrix = NULL;

    if (!matrix1 || !matrix2 || !output_matrix) {
        printf("ERROR: memory allocation failed\n");
        if (matrix1) free(matrix1);
        if (matrix2) free(matrix2);
        if (output_matrix) free(output_matrix);
        return -1;
    }

    for (size_t i = 0; i < input1_len; ++i) matrix1[i] = 1.0f;
    for (size_t i = 0; i < input2_len; ++i) matrix2[i] = 2.0f;
    memset(output_matrix, 0, output_len * sizeof(float));

    printf("\nCalling calculator_gemm (transY=%d) M=%u K=%u N=%u ...\n", transY, m, k, n);

    /* warmup run */
    calculator_gemm(0, matrix1, (int)input1_len, matrix2, (int)input2_len,
                    output_matrix, (int)output_len, m, k, n, FALSE, transY ? TRUE : FALSE);

    /* measure multiple iterations */
    const int iters = 5;
    unsigned long total_ms = 0;
    for (int it = 0; it < iters; ++it) {
        unsigned int t0 = HAP_perf_get_time_us();
        int res = calculator_gemm(0, matrix1, (int)input1_len, matrix2, (int)input2_len,
                                  output_matrix, (int)output_len, m, k, n, FALSE, transY ? TRUE : FALSE);
        unsigned int t1 = HAP_perf_get_time_us();
        if (res != 0) {
            printf("GEMM returned error %d\n", res);
            free(matrix1); free(matrix2); free(output_matrix);
            return res;
        }
        total_ms += (t1 - t0) / 1000;
    }
    unsigned int avg_ms = (unsigned int)(total_ms / iters);
    printf("GEMM average time over %d runs: %u ms\n", iters, avg_ms);

    int ok = verify_naive(matrix1, matrix2, output_matrix, m, k, n, transY);
    printf("Verification: %s (transY=%d)\n", ok ? "PASSED" : "FAILED", transY);

    free(matrix1);
    free(matrix2);
    free(output_matrix);
    return ok ? 0 : -1;
}

int main(int argc, char* argv[]) {
    printf("\n=====================================\n");
    if (argc != 4) {
        printf("ERROR: Invalid arguments.\n");
        printf("Usage: %s <M> <K> <N>\n", argv[0]);
        return -1;
    }

    uint32_t m = (uint32_t)atoi(argv[1]);
    uint32_t k = (uint32_t)atoi(argv[2]);
    uint32_t n = (uint32_t)atoi(argv[3]);
    printf("Starting GEMM test in simulator: M=%u, K=%u, N=%u\n", m, k, n);

    int r0 = run_single_test(m, k, n, 0);
    int r1 = run_single_test(m, k, n, 1);

    printf("=====================================\n");
    return (r0 == 0 && r1 == 0) ? 0 : -1;
}

#endif /* SIMULATOR_TEST */
