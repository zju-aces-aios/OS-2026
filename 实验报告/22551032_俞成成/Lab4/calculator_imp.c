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

// 拿到 float 的二进制表示
static inline int32_t float_to_bits(float input)
{
    union {
        float f;
        int32_t i;
    } fp32 = {.f = input};
    return fp32.i;
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

static inline void matmul_ijk(float *restrict input_matrix1,
                 float *restrict input_matrix2,
                 float *restrict output,
                 uint32_t m,
                 uint32_t k,
                 uint32_t n) {
    // HVX 一次处理 32 个 float
    const uint32_t VEC_F32 = 32; 
    uint32_t n_vec_blocks = n / VEC_F32;
    uint32_t n_tail = n % VEC_F32;

    // 1) 清零 output（确保覆盖写入语义）
    memset(output, 0, (size_t)m * n * sizeof(float));

    // 临时 aligned 向量缓冲（用于非对齐 memcpy）
    HVX_Vector tmp_bvec;   // 128B buffer
    HVX_Vector tmp_outvec; // 128B buffer

    for (uint32_t i = 0; i < m; ++i) {
        float *C_row = output + (size_t)i * n;      // 指向 C[i][0]
        float *A_row = input_matrix1 + (size_t)i * k; // 指向 A[i][0]

        // 向量主路径：每次处理 32 个列元素
        for (uint32_t nb = 0; nb < n_vec_blocks; ++nb) {
            HVX_Vector acc = Q6_V_vzero();

            // 对所有 k 做外积累加：C_row[nb*32 ..] += A_row[p] * B[p][nb*32..]
            for (uint32_t p = 0; p < k; ++p) {
                float a_ip = A_row[p];
                // 广播标量到向量
                HVX_Vector vsplat = Q6_V_vsplat_R(float_to_bits(a_ip));

                // B 行向量地址
                float *b_ptr = input_matrix2 + (size_t)p * n + (size_t)nb * VEC_F32;

                // 如果 b_ptr 已经 128 字节对齐，则直接加载；否则 memcpy 到 tmp_bvec 再使用
                if (((uintptr_t)b_ptr & 127) == 0) {
                    const HVX_Vector *bvec_ptr = (const HVX_Vector *)b_ptr;
                    HVX_Vector bvec = *bvec_ptr;
                    HVX_Vector prod = Q6_Vqf32_vmpy_VsfVsf(vsplat, bvec);
                    acc = Q6_Vqf32_vadd_Vqf32Vqf32(acc, prod);
                } else {
                    // 非对齐安全加载（一次性 memcpy 到对齐缓冲）
                    memcpy(&tmp_bvec, b_ptr, VEC_F32 * sizeof(float));
                    HVX_Vector prod = Q6_Vqf32_vmpy_VsfVsf(vsplat, tmp_bvec);
                    acc = Q6_Vqf32_vadd_Vqf32Vqf32(acc, prod);
                }
            }

            // 累加结果转换回标准 float 向量并写回 output（覆盖写）
            HVX_Vector out_vec = Q6_Vsf_equals_Vqf32(acc);
            float *c_ptr = C_row + (size_t)nb * VEC_F32;

            if (((uintptr_t)c_ptr & 127) == 0) {
                // 对齐写回可以直接 memcpy（或直接赋值 HVX 向量寄存器到内存）
                memcpy(c_ptr, &out_vec, VEC_F32 * sizeof(float));
            } else {
                // 非对齐写回：先拷贝到对齐缓冲再 memcpy 到目标地址
                memcpy(&tmp_outvec, &out_vec, VEC_F32 * sizeof(float));
                memcpy(c_ptr, &tmp_outvec, VEC_F32 * sizeof(float));
            }
        }

        // 标量尾部处理（覆盖写）
        if (n_tail) {
            uint32_t jbase = n_vec_blocks * VEC_F32;
            for (uint32_t j = 0; j < n_tail; ++j) {
                float sum = 0.0f;
                for (uint32_t p = 0; p < k; ++p) {
                    sum += A_row[p] * input_matrix2[p * n + jbase + j];
                }
                C_row[jbase + j] = sum;
            }
        }
    } // end for i
}

static inline void matmul_ikj_transposed_b(float *restrict input_matrix1,
                                           float *restrict input_matrix2,
                                           float *restrict output,
                                           uint32_t m,
                                           uint32_t k,
                                           uint32_t n) {
    // Allocate transposed B (n x k)
    size_t bt_size = (size_t)n * k * sizeof(float);
    float *bt = malloc(bt_size);
    if (bt == NULL) {
        // Error handling: fallback to scalar or abort, but for simplicity, assume success
        return;
    }

    // Transpose B: original B is k x n row-major -> bt is n x k row-major
    for (uint32_t i = 0; i < k; ++i) {
        for (uint32_t j = 0; j < n; ++j) {
            bt[(size_t)j * k + i] = input_matrix2[(size_t)i * n + j];
        }
    }

    for (uint32_t i = 0; i < m; ++i) {
        float *row_a = input_matrix1 + (size_t)i * k;
        for (uint32_t j = 0; j < n; ++j) {
            float *row_bt = bt + (size_t)j * k;  // row of B^T[j][0..k-1]
            float sum = 0.0f;
            uint32_t kk;
            for (kk = 0; kk + 31 < k; kk += 32) {
                // Load 32 floats from A[i][kk:kk+31] using memcpy for alignment
                HVX_Vector va;
                memcpy(&va, row_a + kk, sizeof(HVX_Vector));
                // Load 32 floats from B^T[j][kk:kk+31] using memcpy for alignment
                HVX_Vector vb;
                memcpy(&vb, row_bt + kk, sizeof(HVX_Vector));
                // Element-wise multiply (SF -> QF32)
                HVX_Vector vmul = Q6_Vqf32_vmpy_VsfVsf(va, vb);
                // Reduce vmul to scalar partial sum
                HVX_Vector s = vmul;
                int shift_bytes = 64;  // 16 floats * 4 bytes = 64 bytes (half vector)
                for (int step = 0; step < 5; ++step) {  // log2(32) = 5 steps
                    HVX_Vector r = Q6_V_vror_VR(s, shift_bytes);
                    s = Q6_Vqf32_vadd_Vqf32Vqf32(s, r);
                    shift_bytes >>= 1;  // 64 -> 32 -> 16 -> 8 -> 4
                }
                // Convert QF32 to SF and extract first lane (holds the sum)
                HVX_Vector sfv = Q6_Vsf_equals_Vqf32(s);
                union {
                    HVX_Vector v;
                    float f[32];
                } u;
                u.v = sfv;
                float partial = u.f[0];
                sum += partial;
            }
            // Tail: scalar loop for remaining elements
            for (; kk < k; ++kk) {
                sum += row_a[kk] * row_bt[kk];
            }
            output[(size_t)i * n + j] = sum;
        }
    }

    free(bt);
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

    int r0 = run_single_test(m, k, n, 1);
    int r1 = run_single_test(m, k, n, 0);

    printf("=====================================\n\n\n\n\n");
    return (r0 == 0 && r1 == 0) ? 0 : -1;
}

#endif // SIMULATOR_TEST