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

//在这里添加一些常量的定义
#define VLEN 32
#define VLEN_BYTES 128
#define IS_ALIGNED(ptr) (((uintptr_t)(ptr) & (VLEN_BYTES - 1)) == 0)


int calculator_open(const char*uri, remote_handle64* handle) {
   void *tptr = NULL;
  /* can be any value or ignored, rpc layer doesn't care
   * also ok
   * *handle = 0;
   * *handle = 0xdeadc0de;
   */
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

static inline int32_t float_to_bits(float input)
{
    union {
        float f;
        int32_t i;
    } fp32 = {.f = input};
    return fp32.i;
}
 /*HVX外积实现*/
static inline void matmul_ijk(float *restrict input_matrix1,
                 float *restrict input_matrix2,
                 float *restrict output,
                 uint32_t m,
                 uint32_t k,
                 uint32_t n) {
    for (int i = 0; i < m; i++) {
        int j = 0;
        const int n_vec_loops = n / VLEN;
        for (j = 0; j < n_vec_loops * VLEN; j += VLEN) {
            //使用Q6_V_vzero初始化累加器（QF32格式，用于累加 C[i][j:j+31] 的结果
            HVX_Vector vC_acc_qf32 = Q6_V_vzero();
            for (int l = 0; l < k; l++) {
                // 加载 A[i][l]，并利用Q6_V_vsplat_R将其广播到 32 个通道
                float a_scalar = input_matrix1[i * k + l];
                HVX_Vector vA_scalar_sf = Q6_V_vsplat_R(float_to_bits(a_scalar));
                float *pB = input_matrix2 + l * n + j;
                HVX_Vector vB_loaded_sf;
                if (IS_ALIGNED(pB)) {
                    vB_loaded_sf = *((HVX_Vector *)pB);
                } else {
                    __attribute__((aligned(VLEN_BYTES))) float b_buf[VLEN];
                    memcpy(b_buf, pB, VLEN_BYTES);
                    vB_loaded_sf = *((HVX_Vector *)b_buf);
                }
                //使用Q6_Vqf32_vmpy_VsfVsf进行向量逐元素相乘
                HVX_Vector vMul_qf32 = Q6_Vqf32_vmpy_VsfVsf(vA_scalar_sf, vB_loaded_sf);
                //使用Q6_Vqf32_vadd_Vqf32Vqf32进行向量逐元素相加
                vC_acc_qf32 = Q6_Vqf32_vadd_Vqf32Vqf32(vC_acc_qf32, vMul_qf32);
            }
            //使用Q6_Vsf_equals_Vqf32将高精度累加结果转回标准 float 格式
            HVX_Vector vC_final_sf = Q6_Vsf_equals_Vqf32(vC_acc_qf32);
            float *pC = output + i * n + j;
            if (IS_ALIGNED(pC)) {
                *((HVX_Vector *)pC) = vC_final_sf;
            } else {
                __attribute__((aligned(VLEN_BYTES))) float c_buf[VLEN];
                *((HVX_Vector *)c_buf) = vC_final_sf;
                memcpy(pC, c_buf, VLEN_BYTES);
            }
        }
        // 7. 利用标量实现，边界处理N维度上剩余的n%32个元素
        for (; j < n; j++) {
            float sum = 0.0f;
            for (int l = 0; l < k; l++) {
                sum += input_matrix1[i * k + l] * input_matrix2[l * n + j];
            }
            output[i * n + j] = sum;
        }
    }
    return;
}
/*辅助函数：HVX向量规约，将一个QF32的所有32个元素相加，并返回float标量*/
/*核心思想是使用5次旋转和5次求和得到整个向量之和*/
static inline float hsum_qf32_reduction(HVX_Vector v_qf32) {
    HVX_Vector vRot; 
    // 使用Q6_V_vror_VR旋转16个float并使用Q6_Vqf32_vadd_Vqf32Vqf32执行向量加法
    vRot = Q6_V_vror_VR(v_qf32, 16 * sizeof(float));
    v_qf32 = Q6_Vqf32_vadd_Vqf32Vqf32(v_qf32, vRot);
    vRot = Q6_V_vror_VR(v_qf32, 8 * sizeof(float));
    v_qf32 = Q6_Vqf32_vadd_Vqf32Vqf32(v_qf32, vRot);
    vRot = Q6_V_vror_VR(v_qf32, 4 * sizeof(float));
    v_qf32 = Q6_Vqf32_vadd_Vqf32Vqf32(v_qf32, vRot);
    vRot = Q6_V_vror_VR(v_qf32, 2 * sizeof(float));
    v_qf32 = Q6_Vqf32_vadd_Vqf32Vqf32(v_qf32, vRot);
    vRot = Q6_V_vror_VR(v_qf32, 1 * sizeof(float));
    v_qf32 = Q6_Vqf32_vadd_Vqf32Vqf32(v_qf32, vRot);
    // 此时，v_qf32 的所有元素都等于总和（QF32 格式）
    HVX_Vector vSum_sf = Q6_Vsf_equals_Vqf32(v_qf32);
    __attribute__((aligned(VLEN_BYTES))) float temp_arr[VLEN];
    *((HVX_Vector *)temp_arr) = vSum_sf;
    return temp_arr[0];
}
/*HVX内积实现*/
static inline void matmul_ikj_transposed_b(float *restrict input_matrix1,
                                     float *restrict input_matrix2,
                                     float *restrict output,
                                     uint32_t m,
                                     uint32_t k,
                                     uint32_t n) {
    for (int i = 0; i < m; i++) {
        for (int j = 0; j < n; j++) {
            //使用Q6_V_vzero初始化累加器
            HVX_Vector vAcc_qf32 = Q6_V_vzero();
            int l = 0;
            const int k_vec_loops = k / VLEN;
            for (l = 0; l < k_vec_loops * VLEN; l += VLEN) {
                float *pA = input_matrix1 + i * k + l;
                HVX_Vector vA_sf;
                if (IS_ALIGNED(pA)) {
                    vA_sf = *((HVX_Vector *)pA);
                } else {
                    __attribute__((aligned(VLEN_BYTES))) float bufA[VLEN];
                    memcpy(bufA, pA, VLEN_BYTES);
                    vA_sf = *((HVX_Vector *)bufA);
                }
                float *pB_T = input_matrix2 + j * k + l;
                HVX_Vector vB_T_sf;
                if (IS_ALIGNED(pB_T)) {
                    vB_T_sf = *((HVX_Vector *)pB_T);
                } else {
                    __attribute__((aligned(VLEN_BYTES))) float bufB[VLEN];
                    memcpy(bufB, pB_T, VLEN_BYTES);
                    vB_T_sf = *((HVX_Vector *)bufB);
                }
                //使用Q6_Vqf32_vmpy_VsfVsf执行向量乘法
                HVX_Vector vMul_qf32 = Q6_Vqf32_vmpy_VsfVsf(vA_sf, vB_T_sf);
                //使用Q6_Vqf32_vadd_Vqf32Vqf32执行向量加法
                vAcc_qf32 = Q6_Vqf32_vadd_Vqf32Vqf32(vAcc_qf32, vMul_qf32);
            }
            // 使用上面的辅助函数进行规约，将vAcc中的32个元素相加
            float vec_sum = hsum_qf32_reduction(vAcc_qf32);
            // 7. 边界处理（尾部循环），使用标量乘法处理尾部的k%32个元素
            float tail_sum = 0.0f;
            for (; l < k; l++) {
                tail_sum += input_matrix1[i * k + l] * input_matrix2[j * k + l];
            }
            output[i * n + j] = vec_sum + tail_sum;
        }
    }
    return;
}

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
        if (input_matrix1Len < m * k) {
                return AEE_EBADPARM;
        }
        if (input_matrix2Len < k * n) {
                return AEE_EBADPARM;
        }

        if (outputLen < m * n) {
                return AEE_EBADPARM;
        }

        if (transY) {
                matmul_ikj_transposed_b((float*)input_matrix1, 
                                                                (float*)input_matrix2, 
                                                                output, m, k, n);
        } else {
                matmul_ijk((float*)input_matrix1,
                                                (float*)input_matrix2,
                                                output, m, k, n);
        }

        return 0;
}

#define SIMULATOR_TEST
#ifdef SIMULATOR_TEST

static int verify_naive(float *A, float *B, float *C, uint32_t m, uint32_t k, uint32_t n, int transY) {
    int ok = 1;
    int output_len = m * n;
    float *ref = (float*)malloc(output_len * sizeof(float));
    if (!ref) return 0;
    for (uint32_t i = 0; i < m; ++i) {
        for (uint32_t j = 0; j < n; ++j) {
            float s = 0.0f;
            if (transY) {
                for (uint32_t l = 0; l < k; ++l) {
                    s += A[i * k + l] * B[j * k + l];
                }
            } else {
                for (uint32_t l = 0; l < k; ++l) {
                    s += A[i * k + l] * B[l * n + j];
                }
            }
            ref[i * n + j] = s;
        }
    }

    for (int idx = 0; idx < output_len; ++idx) {
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

static int run_single_test(uint32_t m, uint32_t k, uint32_t n, int transY) {
    int input1_len = m * k;
    int input2_len = k * n;
    int output_len = m * n;
    const int align_size = 128;

    float* matrix1 = (float*)memalign(align_size, input1_len * sizeof(float));
    float* matrix2 = (float*)memalign(align_size, input2_len * sizeof(float));
    float* output_matrix = (float*)memalign(align_size, output_len * sizeof(float));

    if (!matrix1 || !matrix2 || !output_matrix) {
        printf("ERROR: Memory allocation failed!\n");
        if (matrix1) free(matrix1);
        if (matrix2) free(matrix2);
        if (output_matrix) free(output_matrix);
        return -1;
    }

    for (int i = 0; i < input1_len; ++i) matrix1[i] = 1.0f;
    for (int i = 0; i < input2_len; ++i) matrix2[i] = 2.0f;
    memset(output_matrix, 0, output_len * sizeof(float));

    printf("\nCalling calculator_gemm (transY=%d)...\n", transY);
    unsigned int start_time = HAP_perf_get_time_us();
    int result = calculator_gemm(0,
                                 matrix1, input1_len,
                                 matrix2, input2_len,
                                 output_matrix, output_len,
                                 m, k, n,
                                 FALSE, transY ? TRUE : FALSE);
    unsigned int end_time = HAP_perf_get_time_us();
    unsigned int elapsed_time_ms = (end_time - start_time) / 1000;

    if (result == 0) {
        printf("GEMM executed successfully. Time: %u ms\n", elapsed_time_ms);
        int ok = verify_naive(matrix1, matrix2, output_matrix, m, k, n, transY);
        if (ok) {
            printf("Verification: PASSED (transY=%d)\n", transY);
        } else {
            printf("Verification: FAILED (transY=%d)\n", transY);
        }
    } else {
        printf("GEMM FAILED with error %d\n", result);
    }

    free(matrix1);
    free(matrix2);
    free(output_matrix);
    return result;
}

int main(int argc, char* argv[]) {
    printf("\n\n\n\n\n=====================================\n");
    if (argc != 4) {
        printf("ERROR: Invalid arguments.\n");
        printf("Usage: %s <M> <K> <N>\n", argv[0]);
        return -1;
    }

    uint32_t m = atoi(argv[1]);
    uint32_t k = atoi(argv[2]);
    uint32_t n = atoi(argv[3]);
    printf("Starting GEMM test in simulator: M=%lu, K=%lu, N=%lu\n", m, k, n);

    int r0 = run_single_test(m, k, n, 0);
    int r1 = run_single_test(m, k, n, 1);

    printf("=====================================\n\n\n\n\n");
    return (r0 == 0 && r1 == 0) ? 0 : -1;
}

#endif // SIMULATOR_TEST