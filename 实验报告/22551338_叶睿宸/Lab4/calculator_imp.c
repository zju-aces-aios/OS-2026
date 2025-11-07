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

// 拿到 float 的二进制表示
static inline int32_t float_to_bits(float input)
{
    union {
        float f;
        int32_t i;
    } fp32 = {.f = input};
    return fp32.i;
}

static inline float hvx_reduce_sum(HVX_Vector acc) {
    // 归约过程：多次右移并加
    for (int shift = 16; shift >= 1; shift /= 2) {
        HVX_Vector tmp = Q6_V_vror_VR(acc, shift * sizeof(float));
        acc = Q6_Vqf32_vadd_Vqf32Vqf32(acc, tmp);
    }
    HVX_Vector acc_sf = Q6_Vsf_equals_Vqf32(acc);
    // 取第一个元素作为最终sum
    float result;
    memcpy(&result, &acc_sf, sizeof(float));

    return result;
}

static inline void matmul_ijk(float *restrict input_matrix1,
                 float *restrict input_matrix2,
                 float *restrict output,
                 uint32_t m,
                 uint32_t k,
                 uint32_t n) {
    // a = m x k
    // b = k x n
    // c = m x n
    // 外积计算矩阵乘法
    int section = (n - 1) / 32 + 1;
    HVX_Vector acc[section];
    int tmp_n = n;
    for (int i = 0; i < m; i++) {
        for (int s = 0; s < section; s++) {
            acc[s] = Q6_V_vzero();
        }

        for (int j = 0; j < k; j++) {
            tmp_n = n;
            HVX_Vector a_vec = Q6_V_vsplat_R(float_to_bits(input_matrix1[i * k + j]));
            for (int s = 0; s < section; s++) {
                HVX_Vector b_vec = Q6_V_vzero();
                if (tmp_n > 32) {
                    memcpy(&b_vec, &input_matrix2[j * n + s * 32], 32 * sizeof(float));
                    tmp_n -= 32;
                } else {
                    memcpy(&b_vec, &input_matrix2[j * n + s * 32], tmp_n * sizeof(float));
                }
                HVX_Vector mul = Q6_Vqf32_vmpy_VsfVsf(a_vec, b_vec);
                acc[s] = Q6_Vqf32_vadd_Vqf32Vqf32(acc[s], mul);
            }
        }

        tmp_n = n;
        for (int s = 0; s < section; s++) {
            HVX_Vector res = Q6_Vsf_equals_Vqf32(acc[s]);
            if (tmp_n > 32) {
                memcpy(&output[i * n + s * 32], &res, 32 * sizeof(float));
                tmp_n -= 32;
            } else {
                memcpy(&output[i * n + s * 32], &res, tmp_n * sizeof(float));
            }
        }
    }

	return;
}

static inline void matmul_ikj_transposed_b(float *restrict input_matrix1,
                                     float *restrict input_matrix2,
                                     float *restrict output,
                                     uint32_t m,
                                     uint32_t k,
                                     uint32_t n) {
    // a = m x k
    // b = n x k (transposed)
    // c = m x n
    // a[i * k: i * k + k] * b[j * k: j * k + k] -> c[i * n + j];
    for (int i = 0; i < m; i++) {
		for (int j = 0; j < n; j++) {
            HVX_Vector acc = Q6_V_vzero();
            int l = 0;
            for (; l + 31 < k; l += 32) {
                HVX_Vector a_vec, b_vec;
                memcpy(&a_vec, &input_matrix1[i * k + l], 128);
                memcpy(&b_vec, &input_matrix2[j * k + l], 128);
                HVX_Vector mul = Q6_Vqf32_vmpy_VsfVsf(a_vec, b_vec);
                acc = Q6_Vqf32_vadd_Vqf32Vqf32(acc, mul);
            }
            // 处理剩余不足32个的部分
            int rem = k - l;
            if (rem > 0) {
                HVX_Vector a_vec = Q6_V_vzero();
                HVX_Vector b_vec = Q6_V_vzero();
                memcpy(&a_vec, &input_matrix1[i * k + l], rem * sizeof(float));
                memcpy(&b_vec, &input_matrix2[j * k + l], rem * sizeof(float));
                HVX_Vector mul = Q6_Vqf32_vmpy_VsfVsf(a_vec, b_vec);
                acc = Q6_Vqf32_vadd_Vqf32Vqf32(acc, mul);
            }
            // 归约acc为标量sum
            float sum = hvx_reduce_sum(acc);
            output[i * n + j] = sum;
		}
	}

	return;
}

// static inline void matmul_ijk(float *restrict input_matrix1,
//                  float *restrict input_matrix2,
//                  float *restrict output,
//                  uint32_t m,
//                  uint32_t k,
//                  uint32_t n) {
// 	for (int i = 0;i < m; i++) {
// 		for (int j = 0; j < n; j++) {
// 			float sum = 0.0f;
// 			for (int l = 0; l < k; l++) {
// 				sum += input_matrix1[i * k + l] * input_matrix2[l * n + j];
// 			}
// 			output[i * n + j] = sum;
// 		}
// 	}
// 	return;
// }

// static inline void matmul_ikj_transposed_b(float *restrict input_matrix1,
//                                      float *restrict input_matrix2,
//                                      float *restrict output,
//                                      uint32_t m,
//                                      uint32_t k,
//                                      uint32_t n) {
// 	for (int i = 0; i < m; i++) {
// 		for (int j = 0; j < n; j++) {
// 			float sum = 0.0f;
// 			for (int l = 0; l < k; l++) {
// 				sum += input_matrix1[i * k + l] * input_matrix2[j * k + l];
// 			}
// 			output[i * n + j] = sum;
// 		}
// 	}
// 	return;
// }

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