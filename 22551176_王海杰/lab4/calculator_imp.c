#include <stdint.h>
#include <string.h>
#include <hexagon_types.h>
#include <hexagon_protos.h>
#include <hvx_api.h>

// HVX向量配置：32个float元素（128字节），128字节对齐宏
#define HVX_FLOAT_ELEMS 32
#define HVX_VECTOR_BYTES (HVX_FLOAT_ELEMS * sizeof(float))
#define ALIGN_128 __attribute__((aligned(128)))

/**
 * 朴素标量矩阵乘法（Baseline）
 * @param C: 输出矩阵 (M×N)
 * @param A: 输入矩阵A (M×K)
 * @param B: 输入矩阵B (K×N)
 * @param M: 矩阵A/C的行数
 * @param K: 矩阵A的列数/矩阵B的行数
 * @param N: 矩阵B/C的列数
 */
void gemm_baseline(float *C, const float *A, const float *B, int M, int K, int N) {
    // 初始化输出矩阵为0
    memset(C, 0, M * N * sizeof(float));

    // 三重循环：i（A行）→ j（B列）→ k（公共维度）
    for (int i = 0; i < M; i++) {
        const float *A_row = &A[i * K];
        float *C_row = &C[i * N];
        for (int j = 0; j < N; j++) {
            float sum = 0.0f;
            for (int k = 0; k < K; k++) {
                sum += A_row[k] * B[k * N + j];
            }
            C_row[j] = sum;
        }
    }
}

/**
 * 辅助函数：矩阵转置（用于HVX内积实现，将B转置为B^T）
 * @param B_T: 输出转置矩阵 (N×K)
 * @param B: 输入原矩阵 (K×N)
 * @param K: 原矩阵B的行数
 * @param N: 原矩阵B的列数
 */
static void matrix_transpose(ALIGN_128 float *B_T, const float *B, int K, int N) {
    for (int k = 0; k < K; k++) {
        for (int j = 0; j < N; j++) {
            B_T[j * K + k] = B[k * N + j];
        }
    }
}

/**
 * HVX内积实现（A * B^T）：点积向量化，优化缓存局部性
 * 逻辑：先转置B为B^T，A行向量与B^T行向量做向量化点积，再归约求和
 */
void gemm_hvx_dot(float *C, const float *A, const float *B, int M, int K, int N) {
    // 1. 转置B为B^T（解决B列访问的缓存问题），确保128字节对齐
    ALIGN_128 float B_T[N * K];
    matrix_transpose(B_T, B, K, N);

    // 初始化输出矩阵为0
    memset(C, 0, M * N * sizeof(float));

    // 2. 遍历A的每一行（M行）
    for (int i = 0; i < M; i++) {
        const float *A_row = &A[i * K];
        float *C_row = &C[i * N];

        // 遍历B^T的每一行（对应原B的每一列，N行）
        for (int j = 0; j < N; j++) {
            const float *BT_row = &B_T[j * K];
            HVX_Vector sum_qf = Q6_V_vzero();  // 初始化QF32累加器（防溢出）
            int k = 0;

            // 3. 向量化主循环：处理能被32整除的元素段
            for (; k <= K - HVX_FLOAT_ELEMS; k += HVX_FLOAT_ELEMS) {
                // 加载A、B^T的32个float元素（SF格式）
                HVX_Vector a_vec = Q6_V_L32_A16_sf(&A_row[k]);
                HVX_Vector b_vec = Q6_V_L32_A16_sf(&BT_row[k]);
                // 向量乘（SF→QF32）+ 累加
                HVX_Vector mul_qf = Q6_Vqf32_vmpy_VsfVsf(a_vec, b_vec);
                sum_qf = Q6_Vqf32_vadd_Vqf32Vqf32(sum_qf, mul_qf);
            }

            // 4. 尾部处理：不足32个元素的标量计算
            float tail_sum = 0.0f;
            for (; k < K; k++) {
                tail_sum += A_row[k] * BT_row[k];
            }

            // 5. 向量归约：32元素→1标量（旋转累加）
            // 步骤1：右移16字节（4个float）→ 累加
            HVX_Vector temp = Q6_V_vror_VR(sum_qf, 16);
            sum_qf = Q6_Vqf32_vadd_Vqf32Vqf32(sum_qf, temp);
            // 步骤2：右移8字节（2个float）→ 累加
            temp = Q6_V_vror_VR(sum_qf, 8);
            sum_qf = Q6_Vqf32_vadd_Vqf32Vqf32(sum_qf, temp);
            // 步骤3：右移4字节（1个float）→ 累加
            temp = Q6_V_vror_VR(sum_qf, 4);
            sum_qf = Q6_Vqf32_vadd_Vqf32Vqf32(sum_qf, temp);

            // 6. 格式转换（QF32→SF）+ 合并尾部和，写入结果
            HVX_Vector sum_sf = Q6_Vsf_equals_Vqf32(sum_qf);
            float vec_sum;
            Q6_V_S32_A16_sf(&vec_sum, sum_sf, 0);  // 提取归约后的标量
            C_row[j] = vec_sum + tail_sum;
        }
    }
}

/**
 * HVX外积实现（A * B）：标量广播向量化，优化数据复用
 * 逻辑：A单个元素广播为向量，与B的1行32元素相乘，直接累加到C的对应子向量
 */
void gemm_hvx_outer(float *C, const float *A, const float *B, int M, int K, int N) {
    // 初始化输出矩阵为0，确保C的内存对齐（避免向量加载异常）
    ALIGN_128 float *C_aligned = (ALIGN_128 float *)C;
    memset(C_aligned, 0, M * N * sizeof(float));

    // 1. 遍历A的每一行（M行）
    for (int i = 0; i < M; i++) {
        const float *A_row = &A[i * K];
        float *C_row = &C_aligned[i * N];

        // 遍历A行的每一个元素（K列，作为外积的标量）
        for (int k = 0; k < K; k++) {
            float a_scalar = A_row[k];
            const float *B_row = &B[k * N];  // B的第k行（对应外积的向量）

            // 广播A的标量为HVX向量（32个相同元素）
            HVX_Vector a_vec = Q6_V_vsplat_R(float_to_bits(a_scalar));
            int j = 0;

            // 2. 向量化主循环：处理能被32整除的元素段
            for (; j <= N - HVX_FLOAT_ELEMS; j += HVX_FLOAT_ELEMS) {
                // 加载B的32个元素（SF格式）
                HVX_Vector b_vec = Q6_V_L32_A16_sf(&B_row[j]);
                // 加载当前C的32个累加值（SF格式）
                HVX_Vector c_vec = Q6_V_L32_A16_sf(&C_row[j]);

                // 向量乘（a_vec * b_vec）→ 转QF32 + 累加（c_vec转QF32）
                HVX_Vector mul_qf = Q6_Vqf32_vmpy_VsfVsf(a_vec, b_vec);
                HVX_Vector c_qf = Q6_Vqf32_equals_Vsf(c_vec);
                HVX_Vector sum_qf = Q6_Vqf32_vadd_Vqf32Vqf32(c_qf, mul_qf);

                // 格式转换（QF32→SF）+ 写回C的对应位置
                HVX_Vector sum_sf = Q6_Vsf_equals_Vqf32(sum_qf);
                Q6_V_S32_A16_sf(&C_row[j], sum_sf, 0);
            }

            // 3. 尾部处理：不足32个元素的标量计算
            for (; j < N; j++) {
                C_row[j] += a_scalar * B_row[j];
            }
        }
    }
}

/**
 * 矩阵乘法入口函数（统一调用接口，根据类型选择实现）
 * @param impl_type: 实现类型（0=朴素, 1=HVX内积, 2=HVX外积）
 */
void gemm_calc(int impl_type, float *C, const float *A, const float *B, int M, int K, int N) {
    switch (impl_type) {
        case 0:
            gemm_baseline(C, A, B, M, K, N);
            break;
        case 1:
            gemm_hvx_dot(C, A, B, M, K, N);
            break;
        case 2:
            gemm_hvx_outer(C, A, B, M, K, N);
            break;
        default:
            // 默认使用朴素实现
            gemm_baseline(C, A, B, M, K, N);
            break;
    }
}