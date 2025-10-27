/*==============================================================================
  Copyright (c) 2012-2020 Qualcomm Technologies, Inc.
  All rights reserved. Qualcomm Proprietary and Confidential.
==============================================================================*/
#ifndef _DEBUG
#define _DEBUG
#endif

#define THREAD_COUNT 6

#define VTCM_ENABLED 1
#define HVX_WIDTH sizeof(HVX_Vector)
#define HVX_LENGTH (HVX_WIDTH / sizeof(float))
#define VZERO Q6_V_vsplat_R(0)
#include <stdint.h>

static inline int32_t float_to_bits(float input);

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "HAP_farf.h"
#include "calculator.h"
#include <string.h>


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

static inline void matmul_ijk(const float *restrict input_matrix1,
                 const float *restrict input_matrix2,
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

static inline float head(HVX_Vector* v) {
    return ((float*)v)[0];
}

// **模式2: ijk循环顺序（内积形式）**  
// 优势: 更好的缓存局部性，适合转置矩阵
// 过程: A[i][k:k+31] · B[j][k:k+31] → C[i][j] (点积)
static inline void matmul_ijk_inner_prod_transposed_b(const float *restrict input_matrix1,
                                     const float *restrict input_matrix2,
                                     float *restrict output,
                                     uint32_t m,
                                     uint32_t k,
                                     uint32_t n) {
	for (int i = 0; i < m; i++) {
		for (int j = 0; j < n; j++) {
            HVX_Vector v_sum = VZERO;
            HVX_Vector v_a;
            HVX_Vector v_b;
            const float* a_row = &input_matrix1[i * k];
            const float* b_col = &input_matrix2[j * k];
            int l = 0;
            int simd_limit = (k / HVX_LENGTH) * HVX_LENGTH;
            // 每次处理 HVX_LENGTH 个元素
            for(; l < simd_limit; l += HVX_LENGTH) {
                memcpy(&v_a, &a_row[l], HVX_WIDTH);
                memcpy(&v_b, &b_col[l], HVX_WIDTH);
                HVX_Vector v_prod = Q6_Vqf32_vmpy_VsfVsf(v_a, v_b);
                v_sum = Q6_Vqf32_vadd_Vqf32Vqf32(v_sum, v_prod);
            }
            // 归约过程示意 (以8元素为例):
            // 步骤1: [a₁, a₂, a₃, a₄, a₅, a₆, a₇, a₈] + 右移4位 → [a₁+a₅, a₂+a₆, a₃+a₇, a₄+a₈, *, *, *, *]
            // 步骤2: 上述结果 + 右移2位 → [a₁+a₅+a₃+a₇, a₂+a₆+a₄+a₈, *, *, *, *, *, *]  
            // 步骤3: 上述结果 + 右移1位 → [总和, *, *, *, *, *, *, *]
            // 32个元素的simd向量进行规约
            // 示例: HVX_Vector vector = Q6_V_vror_VR(vector_original, 16 * sizeof(float))
            v_sum = Q6_Vqf32_vadd_Vqf32Vqf32(v_sum, Q6_V_vror_VR(v_sum, 16 * sizeof(float)));
            v_sum = Q6_Vqf32_vadd_Vqf32Vqf32(v_sum, Q6_V_vror_VR(v_sum, 8 * sizeof(float)));
            v_sum = Q6_Vqf32_vadd_Vqf32Vqf32(v_sum, Q6_V_vror_VR(v_sum, 4 * sizeof(float)));
            v_sum = Q6_Vqf32_vadd_Vqf32Vqf32(v_sum, Q6_V_vror_VR(v_sum, 2 * sizeof(float)));
            v_sum = Q6_Vqf32_vadd_Vqf32Vqf32(v_sum, Q6_V_vror_VR(v_sum, 1 * sizeof(float)));
            // 提取最终结果
            HVX_Vector _sum = Q6_Vsf_equals_Vqf32(v_sum);
            float sum = head(&_sum);
            // 处理剩余元素
            for (; l < k; l++) {
                sum += a_row[l] * b_col[l];
            }
            output[i * n + j] = sum;
		}
	}
	return;
}

// **模式1: ikj循环顺序（外积形式）**
// 优势: 可以充分利用向量广播
// 过程: A[i][k] × B[k][j:j+31] → C[i][j:j+31]
//     ↑ 广播1个值    ↑ 加载32个值    ↑ 更新32个值
static inline void matmul_ikj_outer_prod(const float *restrict input_matrix1,
                                     const float *restrict input_matrix2,
                                     float *restrict output,
                                     uint32_t m,
                                     uint32_t k,
                                     uint32_t n) {
    for (int i = 0; i < m; i++) {
        const float *a_row = &input_matrix1[i * k];
        int vec_limit = (n / HVX_LENGTH) * HVX_LENGTH;

        /* 向量主干：对每个输出向量块建立寄存器累加器，完成k维累加后一次性写回 */
        for (int j = 0; j < vec_limit; j += HVX_LENGTH) {
            HVX_Vector acc_qf32 = VZERO; /* QF32累加器 */
            for (int l = 0; l < k; l++) {
                uint32_t a_bits = (uint32_t)float_to_bits(a_row[l]);
                HVX_Vector v_a = Q6_V_vsplat_R(a_bits); /* 广播 A[i,l] */
                HVX_Vector v_b;                         /* 加载 B[l, j:j+31] */
                memcpy(&v_b, &input_matrix2[l * n + j], HVX_WIDTH);
                HVX_Vector v_prod_qf32 = Q6_Vqf32_vmpy_VsfVsf(v_a, v_b);
                acc_qf32 = Q6_Vqf32_vadd_Vqf32Vqf32(acc_qf32, v_prod_qf32);
            }
            /* 最终将QF32累加结果转为SF并一次性写回C的向量段 */
            HVX_Vector acc_sf = Q6_Vsf_equals_Vqf32(acc_qf32);
            memcpy(&output[i * n + j], &acc_sf, HVX_WIDTH);
        }

        /* 尾部标量处理：处理剩余无法整向量的列 */
        for (int j = vec_limit; j < (int)n; j++) {
            float sum = 0.0f;
            for (int l = 0; l < (int)k; l++) {
                sum += a_row[l] * input_matrix2[l * n + j];
            }
            output[i * n + j] = sum;
        }
    }
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

	/* Ensure output is zero-initialized for accumulation semantics */
	memset(output, 0, (size_t)m * n * sizeof(float));

	if (transY) {
        matmul_ijk_inner_prod_transposed_b((float*)input_matrix1, 
								(float*)input_matrix2, 
								output, m, k, n);
	} else {
		matmul_ikj_outer_prod((float*)input_matrix1,
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
