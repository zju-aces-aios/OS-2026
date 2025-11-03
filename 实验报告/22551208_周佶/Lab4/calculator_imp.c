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
#include <stdint.h>
#include <string.h>
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

#define VLEN            32
#define VLEN_BYTES      128
#define IS_ALIGNED(ptr) ((((uintptr_t)(ptr)) & (VLEN_BYTES - 1)) == 0)

static inline int32_t float_to_bits(float input)
{
    union {
        float f;
        int32_t i;
    } fp32 = {.f = input};
    return fp32.i;
}

static inline float hvx_reduce_sum(HVX_Vector acc) {
    for (int shift = 16; shift >= 1; shift >>= 1) {
        HVX_Vector tmp = Q6_V_vror_VR(acc, shift * sizeof(float));
        acc = Q6_Vqf32_vadd_Vqf32Vqf32(acc, tmp);
    }
    HVX_Vector acc_sf = Q6_Vsf_equals_Vqf32(acc);
    float result;
    memcpy(&result, &acc_sf, sizeof(float));
    return result;
}

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

static inline void matmul_ikj_transposed_b(float *restrict input_matrix1,
                                     float *restrict input_matrix2,
                                     float *restrict output,
                                     uint32_t m,
                                     uint32_t k,
                                     uint32_t n) {
    for (uint32_t i = 0; i < m; ++i) {
        const float *rowA = input_matrix1 + i * k;
        float *rowC = output + i * n;
        for (uint32_t j = 0; j < n; ++j) {
            const float *rowBT = input_matrix2 + j * k;
            HVX_Vector acc = Q6_V_vzero();
            uint32_t l = 0;
            for (; l + VLEN <= k; l += VLEN) {
                HVX_Vector a_vec, b_vec;
                memcpy(&a_vec, rowA + l, VLEN_BYTES);
                memcpy(&b_vec, rowBT + l, VLEN_BYTES);
                HVX_Vector mul = Q6_Vqf32_vmpy_VsfVsf(a_vec, b_vec);
                acc = Q6_Vqf32_vadd_Vqf32Vqf32(acc, mul);
            }
            if (l < k) {
                HVX_Vector a_tail = Q6_V_vzero();
                HVX_Vector b_tail = Q6_V_vzero();
                uint32_t remaining = k - l;
                memcpy(&a_tail, rowA + l, remaining * sizeof(float));
                memcpy(&b_tail, rowBT + l, remaining * sizeof(float));
                HVX_Vector mul = Q6_Vqf32_vmpy_VsfVsf(a_tail, b_tail);
                acc = Q6_Vqf32_vadd_Vqf32Vqf32(acc, mul);
            }
            rowC[j] = hvx_reduce_sum(acc);
        }
    }
}

static inline void matmul_ijk(float *restrict input_matrix1,
                 float *restrict input_matrix2,
                 float *restrict output,
                 uint32_t m,
                 uint32_t k,
                 uint32_t n) {
    uint32_t sections = (n + VLEN - 1) / VLEN;
    for (uint32_t i = 0; i < m; ++i) {
        float *rowC = output + i * n;
        for (uint32_t j = 0; j < n; ++j) {
            rowC[j] = 0.0f;
        }

        HVX_Vector acc[sections];
        for (uint32_t s = 0; s < sections; ++s) {
            acc[s] = Q6_V_vzero();
        }

        const float *rowA = input_matrix1 + i * k;
        for (uint32_t l = 0; l < k; ++l) {
            HVX_Vector a_vec = Q6_V_vsplat_R(float_to_bits(rowA[l]));
            const float *rowB = input_matrix2 + l * n;
            uint32_t remaining = n;
            for (uint32_t s = 0; s < sections && remaining > 0; ++s) {
                HVX_Vector b_vec = Q6_V_vzero();
                uint32_t block = remaining >= VLEN ? VLEN : remaining;
                memcpy(&b_vec, rowB + s * VLEN, block * sizeof(float));
                HVX_Vector mul = Q6_Vqf32_vmpy_VsfVsf(a_vec, b_vec);
                acc[s] = Q6_Vqf32_vadd_Vqf32Vqf32(acc[s], mul);
                remaining -= block;
            }
        }

        uint32_t remaining = n;
        for (uint32_t s = 0; s < sections && remaining > 0; ++s) {
            HVX_Vector res = Q6_Vsf_equals_Vqf32(acc[s]);
            uint32_t block = remaining >= VLEN ? VLEN : remaining;
            memcpy(rowC + s * VLEN, &res, block * sizeof(float));
            remaining -= block;
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
