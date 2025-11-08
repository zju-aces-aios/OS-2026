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

static inline void matmul_ijk(float *restrict input_matrix1,
                 float *restrict input_matrix2,
                 float *restrict output,
                 uint32_t m,
                 uint32_t k,
                 uint32_t n) {
	for (int i = 0;i < m; i++) {
		for (int j = 0; j < n; j++) {
			float sum = 0.0f;
			for (int l = 0; l < k; l++) {
				sum += input_matrix1[i * k + l] * input_matrix2[l * n + j];
			}
			output[i * n + j] = sum;
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
	for (int i = 0; i < m; i++) {
		for (int j = 0; j < n; j++) {
			float sum = 0.0f;
			for (int l = 0; l < k; l++) {
				sum += input_matrix1[i * k + l] * input_matrix2[j * k + l];
			}
			output[i * n + j] = sum;
		}
	}
	return;
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
//   ==========================HVX GEMM 实现==========================
static inline void matmul_ijk_HVX(float *restrict input_matrix1,
                 float *restrict input_matrix2,
                 float *restrict output,
                 uint32_t m,
                 uint32_t k,
                 uint32_t n) {

    const int VECTOR_BYTES = 128; /* 32 floats */
    const int LANE_F32 = 32;

     /* 使用HVX提供的qf32乘法与sf加法。使用memcpy进行安全的内存转移。 */
     for (uint32_t i = 0; i < m; ++i) {
        float *c_row = &output[i * n];
        /* 初始化output矩阵 */
        for (uint32_t jj = 0; jj < n; ++jj) c_row[jj] = 0.0f;

        for (uint32_t kk = 0; kk < k; ++kk) {
            float a_scalar = input_matrix1[i * k + kk];
            int32_t a_bits = float_to_bits(a_scalar);
            HVX_Vector vA = Q6_V_vsplat_R(a_bits);

            const float *b_row = &input_matrix2[kk * n];
            uint32_t vec_count = n / LANE_F32;
            uint32_t j = 0;

            for (uint32_t v = 0; v < vec_count; ++v) {
                const float *b_ptr = b_row + v * LANE_F32;
                float *c_ptr = c_row + v * LANE_F32;

                HVX_Vector vB;
                memcpy(&vB, b_ptr, VECTOR_BYTES);

                /* vector multiply -> qf32 */
                HVX_Vector vProd_qf = Q6_Vqf32_vmpy_VsfVsf(vA, vB);
                /* convert qf32 -> sf */
                HVX_Vector vProd_sf = Q6_Vsf_equals_Vqf32(vProd_qf);

                /* load C vector (SF), add product (SF) and store back */
                HVX_Vector vC_sf;
                memcpy(&vC_sf, c_ptr, VECTOR_BYTES);
                HVX_Vector vNewC_sf = Q6_Vsf_vadd_VsfVsf(vC_sf, vProd_sf);
                memcpy(c_ptr, &vNewC_sf, VECTOR_BYTES);

                j += LANE_F32;
            }

            /* 尾部元素处理 */
            for (; j < n; ++j) {
                c_row[j] += a_scalar * b_row[j];
            }
        }
    }

    return;
}

static inline void matmul_ikj_transposed_b_HVX(float *restrict input_matrix1,
                                     float *restrict input_matrix2,
                                     float *restrict output,
                                     uint32_t m,
                                     uint32_t k,
                                     uint32_t n) {
    const int VECTOR_BYTES = 128; /* 32 floats */
    const int LANE_F32 = 32;

    /* 计算向量内积，先使用向量乘法，再通过移位与加法归约 */
    for (uint32_t i = 0; i < m; ++i) {
        for (uint32_t j = 0; j < n; ++j) {
            const float *a_row = &input_matrix1[i * k];
            const float *b_row = &input_matrix2[j * k]; 

            /* vector accumulator (SF) */
            HVX_Vector vAcc_sf = Q6_V_vzero();

            uint32_t vec_count = k / LANE_F32;
            uint32_t t = 0;
            for (uint32_t v = 0; v < vec_count; ++v) {
                HVX_Vector vA, vB;
                memcpy(&vA, a_row + t, VECTOR_BYTES);
                memcpy(&vB, b_row + t, VECTOR_BYTES);

                /* element-wise multiply -> qf32, convert to sf */
                HVX_Vector vProd_qf = Q6_Vqf32_vmpy_VsfVsf(vA, vB);
                HVX_Vector vProd_sf = Q6_Vsf_equals_Vqf32(vProd_qf);

                /* accumulate in SF domain */
                vAcc_sf = Q6_Vsf_vadd_VsfVsf(vAcc_sf, vProd_sf);

                t += LANE_F32;
            }

            /* 规约到单一标量 */
            if (vec_count > 0) {
                HVX_Vector v = vAcc_sf;
                v = Q6_Vsf_vadd_VsfVsf(v, Q6_V_vror_VR(v, 64));
                v = Q6_Vsf_vadd_VsfVsf(v, Q6_V_vror_VR(v, 32));
                v = Q6_Vsf_vadd_VsfVsf(v, Q6_V_vror_VR(v, 16));
                v = Q6_Vsf_vadd_VsfVsf(v, Q6_V_vror_VR(v, 8));
                v = Q6_Vsf_vadd_VsfVsf(v, Q6_V_vror_VR(v, 4));

                /* 标量提取 */
                float sum = 0.0f;
                memcpy(&sum, &v, sizeof(float));

                /* 处理尾部元素 (k % LANE_F32) */
                for (uint32_t r = vec_count * LANE_F32; r < k; ++r) {
                    sum += a_row[r] * b_row[r];
                }

                output[i * n + j] = sum;
            } else {
                /* 若凑不齐一个完整向量 */
                float sum = 0.0f;
                for (uint32_t r = 0; r < k; ++r) sum += a_row[r] * b_row[r];
                output[i * n + j] = sum;
            }
        }
    }
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
		// matmul_ikj_transposed_b((float*)input_matrix1, 
		// 						(float*)input_matrix2, 
		// 						output, m, k, n);
        matmul_ikj_transposed_b_HVX((float*)input_matrix1, 
								(float*)input_matrix2, 
								output, m, k, n);
	} else {
		// matmul_ijk((float*)input_matrix1,
		// 				(float*)input_matrix2,
		// 				output, m, k, n);
        matmul_ijk_HVX((float*)input_matrix1,
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