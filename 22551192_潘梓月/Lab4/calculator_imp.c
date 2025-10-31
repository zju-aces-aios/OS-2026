/*==============================================================================
  Copyright (c) 2012-2020 Qualcomm Technologies, Inc.
  All rights reserved. Qualcomm Proprietary and Confidential.
==============================================================================*/
#ifndef _DEBUG
#define _DEBUG
#endif

// ======================= 实验用宏开关 =======================
// 设置为 0: 使用朴素 C 语言 baseline 实现
// 设置为 1: 使用 HVX 优化实现
#define USE_HVX_OPTIMIZATION 1
// ==========================================================

#define THREAD_COUNT 6
#define VTCM_ENABLED 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "HAP_farf.h"
#include "calculator.h"

#include "HAP_perf.h"
#include "HAP_power.h"
#include "HAP_compute_res.h"

#include "AEEStdErr.h"
#include "hexagon_types.h"

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

#ifdef __hexagon__
#include "hexagon_protos.h"
#endif

// ==================== 朴素 Baseline 实现 ====================
static inline void matmul_ijk_naive(float *GGML_RESTRICT input_matrix1,
                 float *GGML_RESTRICT input_matrix2,
                 float *GGML_RESTRICT output,
                 uint32_t m,
                 uint32_t k,
                 uint32_t n) {
	for (uint32_t i = 0; i < m; i++) {
		for (uint32_t j = 0; j < n; j++) {
			float sum = 0.0f;
			for (uint32_t l = 0; l < k; l++) {
				sum += input_matrix1[i * k + l] * input_matrix2[l * n + j];
			}
			output[i * n + j] = sum;
		}
	}
}

static inline void matmul_ikj_transposed_b_naive(float *GGML_RESTRICT input_matrix1,
                                     float *GGML_RESTRICT input_matrix2,
                                     float *GGML_RESTRICT output,
                                     uint32_t m,
                                     uint32_t k,
                                     uint32_t n) {
	for (uint32_t i = 0; i < m; i++) {
		for (uint32_t j = 0; j < n; j++) {
			float sum = 0.0f;
			for (uint32_t l = 0; l < k; l++) {
				sum += input_matrix1[i * k + l] * input_matrix2[j * k + l];
			}
			output[i * n + j] = sum;
		}
	}
}
// ==========================================================

// ==================== HVX 优化实现 ====================
static inline int32_t float_to_bits(float input)
{
    union {
        float f;
        int32_t i;
    } fp32 = {.f = input};
    return fp32.i;
}

// HVX 外积实现 (A * B)
static inline void matmul_ijk_hvx(float *GGML_RESTRICT input_matrix1,
                 float *GGML_RESTRICT input_matrix2,
                 float *GGML_RESTRICT output,
                 uint32_t m,
                 uint32_t k,
                 uint32_t n) {
#ifdef __hexagon__
    memset(output, 0, m * n * sizeof(float));
    const int hvx_width = sizeof(HVX_Vector) / sizeof(float);
    const int vec_bytes = sizeof(HVX_Vector);
    int n_vec = n - (n % hvx_width);

    for (uint32_t i = 0; i < m; i++) {
        for (uint32_t l = 0; l < k; l++) {
            HVX_Vector vA_splat = Q6_V_vsplat_R(float_to_bits(input_matrix1[i * k + l]));
            for (int j = 0; j < n_vec; j += hvx_width) {
                // --- 内存对齐修正 ---
                // 不再使用危险的指针强转，而是用 memcpy 安全地加载数据
                HVX_Vector vecB, vecC;
                memcpy(&vecB, &input_matrix2[l * n + j], vec_bytes);
                memcpy(&vecC, &output[i * n + j], vec_bytes);
                
                HVX_Vector vprod = Q6_Vsf_vmpy_VsfVsf(vA_splat, vecB);
                vecC = Q6_Vsf_vadd_VsfVsf(vecC, vprod);
                
                // 将结果安全地写回
                memcpy(&output[i * n + j], &vecC, vec_bytes);
            }
            // 尾部循环，处理剩余元素
            for (uint32_t j = n_vec; j < n; j++) {
                output[i * n + j] += input_matrix1[i * k + l] * input_matrix2[l * n + j];
            }
        }
    }
#endif
}

// HVX 内积实现 (A * B^T)
static inline void matmul_ikj_transposed_b_hvx(float *GGML_RESTRICT input_matrix1,
                                     float *GGML_RESTRICT input_matrix2,
                                     float *GGML_RESTRICT output,
                                     uint32_t m,
                                     uint32_t k,
                                     uint32_t n) {
#ifdef __hexagon__
    const int hvx_width = sizeof(HVX_Vector) / sizeof(float);
    const int vec_bytes = sizeof(HVX_Vector);
    int k_vec = k - (k % hvx_width);

    for (uint32_t i = 0; i < m; i++) {
        for (uint32_t j = 0; j < n; j++) {
            float final_sum = 0.0f;

            if (k_vec > 0) {
                HVX_Vector v_sum = Q6_V_vzero();
                for (int l = 0; l < k_vec; l += hvx_width) {
                    // --- 内存对齐修正 ---
                    // 不再使用危险的指针强转，而是用 memcpy 安全地加载数据
                    HVX_Vector vecA, vecB;
                    memcpy(&vecA, &input_matrix1[i * k + l], vec_bytes);
                    memcpy(&vecB, &input_matrix2[j * k + l], vec_bytes);

                    v_sum = Q6_Vsf_vadd_VsfVsf(v_sum, Q6_Vsf_vmpy_VsfVsf(vecA, vecB));
                }
                
                // 向量归约
                for (int r = hvx_width / 2; r > 0; r /= 2) {
                    v_sum = Q6_Vsf_vadd_VsfVsf(v_sum, Q6_V_vror_VR(v_sum, r * sizeof(float)));
                }
                memcpy(&final_sum, &v_sum, sizeof(float));
            }

            // 尾部循环继续在之前结果上累加
            for (uint32_t l = k_vec; l < k; l++) {
                final_sum += input_matrix1[i * k + l] * input_matrix2[j * k + l];
            }
            
            output[i * n + j] = final_sum;
        }
    }
#endif
}
// ==========================================================

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
    
#if USE_HVX_OPTIMIZATION == 1
    FILE* log_file = fopen("/data/local/tmp/dsp_runtime.log", "a");
    if (log_file != NULL) {
        fprintf(log_file, "Confirmed: HVX-accelerated code is running! M=%lu, K=%lu, N=%lu, transY=%d\n", m, k, n, transY);
        fclose(log_file);
    }
#endif

	if (m == 0 || k == 0 || n == 0) return 0; 
	if (input_matrix1Len < m * k) return AEE_EBADPARM;
	if (input_matrix2Len < k * n) return AEE_EBADPARM;
	if (outputLen < m * n) return AEE_EBADPARM;

#if USE_HVX_OPTIMIZATION == 1
	if (transY) {
		matmul_ikj_transposed_b_hvx((float*)input_matrix1, (float*)input_matrix2, output, m, k, n);
	} else {
		matmul_ijk_hvx((float*)input_matrix1, (float*)input_matrix2, output, m, k, n);
	}
#else
    if (transY) {
        matmul_ikj_transposed_b_naive((float*)input_matrix1, (float*)input_matrix2, output, m, k, n);
    } else {
        matmul_ijk_naive((float*)input_matrix1, (float*)input_matrix2, output, m, k, n);
    }
#endif

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
            printf("Verification failed at index %d: ref=%.6f, out=%.6f\n", idx, a, b);
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

    for (int i = 0; i < input1_len; ++i) matrix1[i] = (float)(i % 10) * 0.1f;
    for (int i = 0; i < input2_len; ++i) matrix2[i] = (float)(i % 10) * 0.1f;
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
