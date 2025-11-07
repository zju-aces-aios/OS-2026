#ifndef _DEBUG
#define _DEBUG
#endif

#define THREAD_COUNT 6

#define VTCM_ENABLED 1
#ifndef ALIGN
#define ALIGN(n) __attribute__((aligned(n)))
#endif
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
static inline int32_t float_to_bits(float input)
{
    union {
        float f;
        int32_t i;
    } fp32 = {.f = input};
    return fp32.i;
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
static inline uint32_t build_l2fetch_params(uint32_t width_bytes, uint32_t height, uint32_t stride_bytes) {
    
    
    uint32_t width_field = (width_bytes & 0xFF) << 8;
    
    
    // 将高度限制在 8 位最大值 (255)
    if (height > 255) {
        height = 255; 
    }
    uint32_t height_field = (height & 0xFF);
    

    uint32_t stride_field = (stride_bytes & 0xFFFF) << 16;
    
    return stride_field | width_field | height_field;
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





static inline void matmul_ijk_hvx(float *restrict input_matrix1,
                 float *restrict input_matrix2,
                 float *restrict output,
                 uint32_t m,
                 uint32_t k,
                 uint32_t n) {
    
	for (int i = 0;i < m; i++) {
        const float *current_row = &input_matrix1[i * k];
        int remain_elements = (n / 32) * 32;// 每次处理32个输出元素
        for(int j=0;j<remain_elements;j+=32){
            HVX_Vector acc = Q6_V_vsplat_R(0);
            for(int l = 0;l < k;l++){
                uint32_t mid_bits = (uint32_t)float_to_bits(current_row[l]);
                HVX_Vector v_current_row = Q6_V_vsplat_R(mid_bits);//准备一行
                HVX_Vector v_current_col = *(HVX_Vector const *)&input_matrix2[l * n + j];                
                HVX_Vector v_prod = Q6_Vqf32_vmpy_VsfVsf(v_current_row, v_current_col);
                acc = Q6_Vqf32_vadd_Vqf32Vqf32(acc, v_prod);
                
            }
            HVX_Vector acc_sf = Q6_Vsf_equals_Vqf32(acc);
            *(HVX_Vector *)&output[i * n + j] = acc_sf;

        }
        //尾部
        for (int j = remain_elements; j < (int)n; j++) {
            float sum = 0.0f;
            for (int l = 0; l < (int)k; l++) {
                sum += current_row[l] * input_matrix2[l * n + j];
            }
            output[i * n + j] = sum;
        }

	}
	return;
}




/*
 * HVX 优化的 matmul (ijk 顺序)，使用 l2fetch 进行数据预取
 */
static inline void matmul_ijk_hvx_l2fetch(float *restrict input_matrix1,
                 float *restrict input_matrix2,
                 float *restrict output,
                 uint32_t m,
                 uint32_t k,
                 uint32_t n) {

    // --- 1. 定义预取参数 ---

    // 矩阵 A (input_matrix1) 的参数:
  // 我们预取一整行 A。这是一个大型线性预取 [cite: 840, 846]。
    // 我们使用 128 字节的宽度，并计算需要多少行。
    const uint32_t line_size_A = 128;
    uint32_t row_A_bytes = k * sizeof(float);
    uint32_t num_lines_A = (row_A_bytes + line_size_A - 1) / line_size_A;
    uint32_t params_A = build_l2fetch_params(line_size_A, num_lines_A, line_size_A);

    // 矩阵 B (input_matrix2) 的参数:
  // 这是一个 2D "盒子预取" [cite: 840, 844]。
    // 宽度是 HVX 向量的宽度 (32 个 float)。
    const uint32_t width_B_bytes = 32 * sizeof(float); // 128 字节
    uint32_t height_B = k;
    uint32_t stride_B_bytes = n * sizeof(float);
    uint32_t params_B = build_l2fetch_params(width_B_bytes, height_B, stride_B_bytes);

    // --- 2. 预取循环的 "Prologue" ---
    
    // 在循环开始前，预取 A 的第 0 行
    Q6_l2fetch_AR((void *)&input_matrix1[0], params_A); // [cite: 1413]

    int remain_elements = (n / 32) * 32;
    if (remain_elements > 0) {
        // 预取 B 的第 0 个列块
        Q6_l2fetch_AR((void *)&input_matrix2[0], params_B);// [cite: 1413]
    }

    // --- 3. 主循环 ---

    for (int i = 0;i < m; i++) {
        const float *current_row = &input_matrix1[i * k];

        // 预取 A 的下一行 (i+1)
        if (i + 1 < m) {
            Q6_l2fetch_AR((void *)&input_matrix1[(i + 1) * k], params_A); // [cite: 1413]
        }
        
        for(int j=0;j<remain_elements;j+=32){
            
            // 预取 B 的下一个列块 (j+32)
            if (j + 32 < remain_elements) {
                // 起始地址是 B[0][j+32]
                Q6_l2fetch_AR((void *)&input_matrix2[j + 32], params_B);// [cite: 1413]
            }

            HVX_Vector acc = Q6_V_vsplat_R(0);
            for(int l = 0;l < k;l++){
                uint32_t mid_bits = (uint32_t)float_to_bits(current_row[l]);
                HVX_Vector v_current_row = Q6_V_vsplat_R(mid_bits);// A[i][l]
                
                // B[l][j...j+31]
                HVX_Vector v_current_col = *(HVX_Vector const *)&input_matrix2[l * n + j];                
                
                HVX_Vector v_prod = Q6_Vqf32_vmpy_VsfVsf(v_current_row, v_current_col);
                acc = Q6_Vqf32_vadd_Vqf32Vqf32(acc, v_prod);
            }
            HVX_Vector acc_sf = Q6_Vsf_equals_Vqf32(acc);
            *(HVX_Vector *)&output[i * n + j] = acc_sf;
        }

        // --- 4. 尾部循环 (标量) ---
        // l2fetch 对标量代码的帮助不大，因为瓶颈在计算上
        for (int j = remain_elements; j < (int)n; j++) {
            float sum = 0.0f;
            for (int l = 0; l < (int)k; l++) {
                sum += current_row[l] * input_matrix2[l * n + j];
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


static inline void matmul_ikj_transposed_b_hvx(
        float *restrict input_matrix1,    // A  : m × k
        float *restrict input_matrix2,    // B^T: n × k  (已转置)
        float *restrict output,         // C  : m × n
        uint32_t m,
        uint32_t k,
        uint32_t n)
{
    uint32_t kvec = (k / 32) * 32; // k 维度的向量化部分

    for (uint32_t i = 0; i < m; i++) { // i 循环 (C 的行)
        const float *a_row = &input_matrix1[i * k]; // A[i, :]

        for (uint32_t j = 0; j < n; j++) { // j 循环 (C 的列)
            const float *b_t_row = &input_matrix2[j * k]; // B_T[j, :]
            
            HVX_Vector acc_v = Q6_V_vsplat_R(0); // 32 宽的向量累加器

            // 'l' 循环 (k 维度), 向量化
            for (uint32_t l = 0; l < kvec; l += 32) {
                // 加载 A[i, l...l+31]
                HVX_Vector va = *(HVX_Vector const *)&a_row[l];
                
                // 加载 B_T[j, l...l+31]
                HVX_Vector vb = *(HVX_Vector const *)&b_t_row[l];

                // FMA: acc_v += va * vb 
                HVX_Vector prod = Q6_Vqf32_vmpy_VsfVsf(va, vb);
                acc_v = Q6_Vqf32_vadd_Vqf32Vqf32(acc_v, prod);
            }

            // --- 1. 水平求和 (Horizontal Reduction) ---
            // 将 acc_v 中的 32 个 float 加起来
            float ALIGN(128) temp_sum[32]; // 必须对齐 HVX 存储
            HVX_Vector acc_sf = Q6_Vsf_equals_Vqf32(acc_v);
            *(HVX_Vector *)temp_sum = acc_sf;
            
            float sum = 0.0f;
            for (int v = 0; v < 32; v++) {
                sum += temp_sum[v];
            }

            // --- 2. 尾部 'l' (k-维度) 循环 ---
            // 处理 k 不是 32 倍数的剩余部分
            for (uint32_t l = kvec; l < k; l++) {
                sum += a_row[l] * b_t_row[l];
            }
            
            output[i * n + j] = sum;
        }
    }
}


/*
 * HVX 优化的 matmul (ikj 顺序，B 已转置) - 带 L2Fetch 的 (i, j, l_v) 逻辑
 */
static inline void matmul_ikj_transposed_b_hvx_l2fetch(
        float *restrict input_matrix1,    // A  : m × k
        float *restrict input_matrix2,    // B^T: n × k  (已转置)
        float *restrict output,         // C  : m × n
        uint32_t m,
        uint32_t k,
        uint32_t n)
{
    // --- 1. 定义预取参数 (1D 线性预取) ---
    const uint32_t line_size = 128; // 128 字节
    
    // 矩阵 A (input_matrix1): 预取 A 的行 A[i, :]
    uint32_t row_A_bytes = k * sizeof(float);
    uint32_t num_lines_A = (row_A_bytes + line_size - 1) / line_size;
    uint32_t params_A = build_l2fetch_params(line_size, num_lines_A, line_size);

    // 矩阵 B^T (input_matrix2): 预取 B^T 的行 B_T[j, :]
    uint32_t row_B_bytes = k * sizeof(float);
    uint32_t num_lines_B = (row_B_bytes + line_size - 1) / line_size;
    uint32_t params_B_T = build_l2fetch_params(line_size, num_lines_B, line_size);

    uint32_t kvec = (k / 32) * 32;

    // --- 2. 预取 Prologue ---
    if (m > 0) {
        Q6_l2fetch_AR((void *)&input_matrix1[0], params_A); // 预取 A[0, :]
    }
    if (n > 0) {
        Q6_l2fetch_AR((void *)&input_matrix2[0], params_B_T); // 预取 B_T[0, :]
    }

    // --- 3. 主循环 (i, j, l_v) ---
    for (uint32_t i = 0; i < m; i++) {
        const float *a_row = &input_matrix1[i * k]; // A[i, :]

        // 预取 A[i+1, :]
        if (i + 1 < m) {
            Q6_l2fetch_AR((void *)&input_matrix1[(i + 1) * k], params_A);
        }

        // 注意：我们不能在这里预取 B_T[j+1, :]，因为它在内部 j 循环中
        // 这使得 (i, j, l_v) 循环的 L2 预取效率低于 (i, j_v, l)
        // 但 (i, j, l_v) 是逻辑正确的。

        for (uint32_t j = 0; j < n; j++) {
            const float *b_t_row = &input_matrix2[j * k]; // B_T[j, :]

            // 预取 B_T[j+1, :]
            if (j + 1 < n) {
                Q6_l2fetch_AR((void *)&input_matrix2[(j + 1) * k], params_B_T);
            }
            
            HVX_Vector acc_v = Q6_V_vsplat_R(0); // 向量累加器

            // 向量化的 'l' 循环 (k 维度)
            for (uint32_t l = 0; l < kvec; l += 32) {
                HVX_Vector va = *(HVX_Vector const *)&a_row[l];
                HVX_Vector vb = *(HVX_Vector const *)&b_t_row[l];
                HVX_Vector prod = Q6_Vqf32_vmpy_VsfVsf(va, vb);
                acc_v = Q6_Vqf32_vadd_Vqf32Vqf32(acc_v, prod);
            }

            // --- 4. 水平求和 ---
            float ALIGN(128) temp_sum[32];
            HVX_Vector acc_sf = Q6_Vsf_equals_Vqf32(acc_v);
            *(HVX_Vector *)temp_sum = acc_sf;
            
            float sum = 0.0f;
            for (int v = 0; v < 32; v++) {
                sum += temp_sum[v];
            }

            // --- 5. 尾部 'l' 循环 ---
            for (uint32_t l = kvec; l < k; l++) {
                sum += a_row[l] * b_t_row[l];
            }
            
            output[i * n + j] = sum;
        }
    }
}



static inline float internal_hvx_sum_and_tail(
    HVX_Vector acc_v,         // 32 宽的向量累加器
    const float *a_row,     // A 矩阵的当前行 (A[i, :])
    const float *b_t_row,   // B^T 矩阵的当前行 (B_T[j, :])
    uint32_t kvec,            // k 的向量化部分 (k/32)*32
    uint32_t k)               // k 的总长度
{
    // --- 1. 水平求和 (Horizontal Reduction) ---
    float ALIGN(128) temp_sum[32]; // 必须 128 字节对齐
    HVX_Vector acc_sf = Q6_Vsf_equals_Vqf32(acc_v);
    *(HVX_Vector *)temp_sum = acc_sf;
    
    float sum = 0.0f;
    for (int v = 0; v < 32; v++) {
        sum += temp_sum[v];
    }

    // --- 2. 尾部 'l' (k-维度) 循环 ---
    // 处理 k 不是 32 倍数的剩余部分
    for (uint32_t l = kvec; l < k; l++) {
        sum += a_row[l] * b_t_row[l];
    }
    
    return sum;
}
static inline void matmul_ikj_transposed_b_hvx_l2fetch_unroll(
        float *restrict input_matrix1,    // A  : m × k
        float *restrict input_matrix2,    // B^T: n × k  (已转置)
        float *restrict output,         // C  : m × n
        uint32_t m,
        uint32_t k,
        uint32_t n)
{
    // --- 1. 定义预取参数 (与之前相同) ---
    const uint32_t line_size = 128;
    
    uint32_t row_A_bytes = k * sizeof(float);
    uint32_t num_lines_A = (row_A_bytes + line_size - 1) / line_size;
    uint32_t params_A = build_l2fetch_params(line_size, num_lines_A, line_size);

    uint32_t row_B_bytes = k * sizeof(float);
    uint32_t num_lines_B = (row_B_bytes + line_size - 1) / line_size;
    uint32_t params_B_T = build_l2fetch_params(line_size, num_lines_B, line_size);

    uint32_t kvec = (k / 32) * 32;

    // --- 2. 预取 Prologue (与之前相同) ---
    if (m > 0) {
        Q6_l2fetch_AR((void *)&input_matrix1[0], params_A); // 预取 A[0, :]
    }
    if (n > 0) {
        Q6_l2fetch_AR((void *)&input_matrix2[0], params_B_T); // 预取 B_T[0, :]
    }
    if (n > 1) {
        Q6_l2fetch_AR((void *)&input_matrix2[k], params_B_T); // 预取 B_T[1, :]
    }

    // --- 3. 主循环 (i, j_v, l_v) ---
    for (uint32_t i = 0; i < m; i++) {
        const float *a_row = &input_matrix1[i * k]; // A[i, :]

        // 预取 A[i+1, :]
        if (i + 1 < m) {
            Q6_l2fetch_AR((void *)&input_matrix1[(i + 1) * k], params_A);
        }

        // --- 优化点：'j' 循环 2x 展开 ---
        uint32_t j = 0;
        uint32_t j_vec = (n / 2) * 2; // 'j' 维度的 2x 展开部分

        for (j = 0; j < j_vec; j += 2) {
            // 预取 B_T[j+2, :] 和 B_T[j+3, :]
            if (j + 2 < n) {
                Q6_l2fetch_AR((void *)&input_matrix2[(j + 2) * k], params_B_T);
            }
            if (j + 3 < n) {
                Q6_l2fetch_AR((void *)&input_matrix2[(j + 3) * k], params_B_T);
            }

            const float *b_t_row_0 = &input_matrix2[(j + 0) * k]; // B_T[j, :]
            const float *b_t_row_1 = &input_matrix2[(j + 1) * k]; // B_T[j+1, :]
            
            // 准备 2 个向量累加器
            HVX_Vector acc_v0 = Q6_V_vsplat_R(0); // 用于 C[i, j]
            HVX_Vector acc_v1 = Q6_V_vsplat_R(0); // 用于 C[i, j+1]

            // 向量化的 'l' 循环 (k 维度)
            for (uint32_t l = 0; l < kvec; l += 32) {
                // 加载 A[i, l...] (仅加载 1 次)
                HVX_Vector va = *(HVX_Vector const *)&a_row[l];
                
                // --- 计算 C[i, j] ---
                HVX_Vector vb0 = *(HVX_Vector const *)&b_t_row_0[l];
                HVX_Vector prod0 = Q6_Vqf32_vmpy_VsfVsf(va, vb0);
                acc_v0 = Q6_Vqf32_vadd_Vqf32Vqf32(acc_v0, prod0);

                // --- 计算 C[i, j+1] (复用 va) ---
                HVX_Vector vb1 = *(HVX_Vector const *)&b_t_row_1[l];
                HVX_Vector prod1 = Q6_Vqf32_vmpy_VsfVsf(va, vb1);
                acc_v1 = Q6_Vqf32_vadd_Vqf32Vqf32(acc_v1, prod1);
            }

            // --- 4. 水平求和 + 尾部处理 (现在调用辅助函数) ---
            output[i * n + j + 0] = internal_hvx_sum_and_tail(acc_v0, a_row, b_t_row_0, kvec, k);
            output[i * n + j + 1] = internal_hvx_sum_and_tail(acc_v1, a_row, b_t_row_1, kvec, k);
        }
        // --- 展开结束 ---


        // --- 5. 尾部 'j' 循环 ---
        // 如果 n 是奇数, j_vec = n-1, 这里会多执行 1 次
        for (; j < n; j++) {
            const float *b_t_row = &input_matrix2[j * k]; // B_T[j, :]
            
            // (注意：这里的 B_T[j+1] 预取可能在上一个循环的末尾已经发出)
            
            HVX_Vector acc_v = Q6_V_vsplat_R(0); // 向量累加器

            for (uint32_t l = 0; l < kvec; l += 32) {
                HVX_Vector va = *(HVX_Vector const *)&a_row[l];
                HVX_Vector vb = *(HVX_Vector const *)&b_t_row[l];
                HVX_Vector prod = Q6_Vqf32_vmpy_VsfVsf(va, vb);
                acc_v = Q6_Vqf32_vadd_Vqf32Vqf32(acc_v, prod);
            }
            
            output[i * n + j] = internal_hvx_sum_and_tail(acc_v, a_row, b_t_row, kvec, k);
        }
    }
}



// 拿到 float 的二进制表示

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
		matmul_ikj_transposed_b_hvx_l2fetch_unroll((float*)input_matrix1, 
								(float*)input_matrix2, 
								output, m, k, n);
	} else {
		matmul_ijk_hvx_l2fetch((float*)input_matrix1,
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