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

int calculator_open(const char* uri, remote_handle64* handle) {
    void* tptr = NULL;
    /* can be any value or ignored, rpc layer doesn't care
     * also ok
     * *handle = 0;
     * *handle = 0xdeadc0de;
     */
    tptr = (void*)malloc(1);
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

static inline void matmul_ijk(float* restrict input_matrix1,
    float* restrict input_matrix2,
    float* restrict output,
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

static inline void matmul_ikj_transposed_b(float* restrict input_matrix1,
    float* restrict input_matrix2,
    float* restrict output,
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
static inline int32_t float_to_bits(float input) {
    union {
        float f;
        int32_t i;
    } fp32 = { .f = input };
    return fp32.i;
}


// ============================================================================
// 2. 基于 HVX 的外积实现 (A * B)
// ============================================================================

/**
 * HVX 优化的矩阵乘法 - 外积法
 *
 * 原理：
 * - 采用 ikj 循环顺序
 * - 对于 A[i][k]，广播到向量，与 B[k][j:j+31] 相乘，累加到 C[i][j:j+31]
 * - 充分利用 vsplat (标量广播) 和向量乘加指令
 *
 * 主要 HVX 指令：
 * - Q6_V_vsplat_R: 将标量广播到向量的所有元素
 * - Q6_Vqf32_vmpy_VsfVsf: 向量乘法 (32 个 float 一次)
 * - Q6_Vqf32_vadd_Vqf32Vqf32: 向量加法
 * - Q6_Vsf_equals_Vqf32: QF32 到 SF 格式转换
 *
 * 对齐处理：
 * - 主循环：处理 32 的倍数部分 (向量化)
 * - 尾部循环：处理剩余元素 (标量化)
 */
static inline void matmul_ikj_hvx_outer_product(float* restrict input_matrix1,
    float* restrict input_matrix2,
    float* restrict output,
    uint32_t m,
    uint32_t k,
    uint32_t n) {
    const int VEC_SIZE = 32; // HVX 向量可以处理 32 个 float

    // 初始化输出矩阵为 0
    for (uint32_t i = 0; i < m * n; i++) {
        output[i] = 0.0f;
    }

    for (uint32_t i = 0; i < m; i++) {
        for (uint32_t l = 0; l < k; l++) {
            // 广播 A[i][l] 到向量
            float a_val = input_matrix1[i * k + l];
            HVX_Vector vA = Q6_V_vsplat_R(float_to_bits(a_val));

            uint32_t j = 0;
            // 向量化主循环：处理 32 的倍数
            for (; j + VEC_SIZE <= n; j += VEC_SIZE) {
                // 加载 B[l][j:j+31]
                HVX_Vector vB;
                if (((uintptr_t)&input_matrix2[l * n + j] & 127) == 0) {
                    // 128 字节对齐，直接加载
                    vB = *((HVX_Vector*)&input_matrix2[l * n + j]);
                } else {
                    // 非对齐，使用 memcpy
                    memcpy(&vB, &input_matrix2[l * n + j], 128);
                }

                // 加载 C[i][j:j+31]
                HVX_Vector vC;
                if (((uintptr_t)&output[i * n + j] & 127) == 0) {
                    vC = *((HVX_Vector*)&output[i * n + j]);
                } else {
                    memcpy(&vC, &output[i * n + j], 128);
                }

                // 向量乘法：vA * vB
                HVX_Vector vMul = Q6_Vqf32_vmpy_VsfVsf(vA, vB);

                // 向量加法：vC += vMul
                HVX_Vector vResult = Q6_Vqf32_vadd_Vqf32Vqf32(vC, vMul);

                // 转换并存储
                HVX_Vector vResult_sf = Q6_Vsf_equals_Vqf32(vResult);
                if (((uintptr_t)&output[i * n + j] & 127) == 0) {
                    *((HVX_Vector*)&output[i * n + j]) = vResult_sf;
                } else {
                    memcpy(&output[i * n + j], &vResult_sf, 128);
                }
            }

            // 尾部处理：标量处理剩余元素
            for (; j < n; j++) {
                output[i * n + j] += a_val * input_matrix2[l * n + j];
            }
        }
    }
}

// ============================================================================
// 3. 基于 HVX 的内积实现 (A * B^T)
// ============================================================================

/**
 * HVX 优化的矩阵乘法 - 内积法 (用于 B 已转置的情况)
 *
 * 原理：
 * - 采用 ijk 循环顺序
 * - 计算 A[i][:] 和 B_transposed[j][:] 的点积
 * - 向量化内层的点积计算，然后归约求和
 *
 * 主要 HVX 指令：
 * - Q6_V_vzero: 创建零向量作为累加器
 * - Q6_Vqf32_vmpy_VsfVsf: 向量乘法
 * - Q6_Vqf32_vadd_Vqf32Vqf32: 向量加法
 * - Q6_V_vror_VR: 向量旋转 (用于归约)
 * - Q6_Vsf_equals_Vqf32: 格式转换
 *
 * 归约过程：
 * - 使用向量旋转和加法实现高效的向量归约
 * - log2(32) = 5 次旋转加法即可完成 32 元素的求和
 *
 * 对齐处理：
 * - 主循环：向量化处理 32 的倍数
 * - 尾部循环：标量处理剩余元素
 */
static inline void matmul_ikj_hvx_inner_product(float* restrict input_matrix1,
    float* restrict input_matrix2,
    float* restrict output,
    uint32_t m,
    uint32_t k,
    uint32_t n) {
    const int VEC_SIZE = 32;

    for (uint32_t i = 0; i < m; i++) {
        for (uint32_t j = 0; j < n; j++) {
            // 初始化累加向量为 0
            HVX_Vector vSum = Q6_V_vzero();

            uint32_t l = 0;
            // 向量化主循环：处理 32 的倍数
            for (; l + VEC_SIZE <= k; l += VEC_SIZE) {
                // 加载 A[i][l:l+31]
                HVX_Vector vA;
                if (((uintptr_t)&input_matrix1[i * k + l] & 127) == 0) {
                    vA = *((HVX_Vector*)&input_matrix1[i * k + l]);
                } else {
                    memcpy(&vA, &input_matrix1[i * k + l], 128);
                }

                // 加载 B_transposed[j][l:l+31] = B[j * k + l : j * k + l + 31]
                HVX_Vector vB;
                if (((uintptr_t)&input_matrix2[j * k + l] & 127) == 0) {
                    vB = *((HVX_Vector*)&input_matrix2[j * k + l]);
                } else {
                    memcpy(&vB, &input_matrix2[j * k + l], 128);
                }

                // 向量乘法
                HVX_Vector vMul = Q6_Vqf32_vmpy_VsfVsf(vA, vB);

                // 累加
                vSum = Q6_Vqf32_vadd_Vqf32Vqf32(vSum, vMul);
            }

            // 向量归约：将 vSum 中的 32 个元素求和
            // 使用树形归约方法，通过向量旋转和加法
            vSum = Q6_Vqf32_vadd_Vqf32Vqf32(vSum, Q6_V_vror_VR(vSum, 16 * sizeof(float)));
            vSum = Q6_Vqf32_vadd_Vqf32Vqf32(vSum, Q6_V_vror_VR(vSum, 8 * sizeof(float)));
            vSum = Q6_Vqf32_vadd_Vqf32Vqf32(vSum, Q6_V_vror_VR(vSum, 4 * sizeof(float)));
            vSum = Q6_Vqf32_vadd_Vqf32Vqf32(vSum, Q6_V_vror_VR(vSum, 2 * sizeof(float)));
            vSum = Q6_Vqf32_vadd_Vqf32Vqf32(vSum, Q6_V_vror_VR(vSum, 1 * sizeof(float)));

            // 提取第一个元素（归约结果）
            HVX_Vector vSum_sf = Q6_Vsf_equals_Vqf32(vSum);
            float sum;
            memcpy(&sum, &vSum_sf, sizeof(float));

            // 尾部处理：标量处理剩余元素
            for (; l < k; l++) {
                sum += input_matrix1[i * k + l] * input_matrix2[j * k + l];
            }

            output[i * n + j] = sum;
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

    int impl_type = 0; // 使用 HVX 外积实现

    if (impl_type) {
        if (transY) {
            matmul_ikj_transposed_b((float*)input_matrix1,
                (float*)input_matrix2,
                output, m, k, n);
        } else {
            matmul_ijk((float*)input_matrix1,
                (float*)input_matrix2,
                output, m, k, n);
        }
    } else {
        // 默认：根据 transY 自动选择 HVX 实现
        if (transY) {
            matmul_ikj_hvx_inner_product((float*)input_matrix1,
                (float*)input_matrix2,
                output, m, k, n);
        } else {
            matmul_ikj_hvx_outer_product((float*)input_matrix1,
                (float*)input_matrix2,
                output, m, k, n);
        }
    }


    return 0;
}

#define SIMULATOR_TEST
#ifdef SIMULATOR_TEST

static int verify_naive(float* A, float* B, float* C, uint32_t m, uint32_t k, uint32_t n, int transY) {
    int ok = 1;
    int output_len = m * n;
    float* ref = (float*)malloc(output_len * sizeof(float));
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