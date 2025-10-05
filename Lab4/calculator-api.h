#ifndef CALCULATOR_H
#define CALCULATOR_H

#include <stdint.h>
#include <AEEStdDef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file calculator.h
 * @brief NPU Hexagon Calculator 接口定义
 * 
 * 这个头文件定义了使用Qualcomm Hexagon DSP进行计算的C++接口
 */

/**
 * Calculator类，用于封装计算结果和错误状态
 */
class Calculator {
public:
    int64 result;        ///< 计算结果
    int nErr;            ///< 错误代码
    
    Calculator() : result(0), nErr(0) {}
};

/**
 * 初始化DSP环境
 * 
 * @param dsp_library_path DSP库文件路径，通常是 "/data/local/tmp"
 * @return 0 表示成功，-1 表示失败
 * 
 * @note 这个函数必须在调用其他计算函数之前调用
 * @example
 * @code
 * if (calculator_init("/data/local/tmp") != 0) {
 *     printf("DSP初始化失败\n");
 *     return -1;
 * }
 * @endcode
 */
int calculator_init(const char* dsp_library_path);

#ifdef __cplusplus
}
#endif


/**
 * 使用NPU计算矩阵乘法
 * 
 * @param matrix1 输入矩阵1
 * @param matrix2 输入矩阵2
 * @param m 矩阵1的行数
 * @param k 矩阵1的列数（也是矩阵2的行数）
 * @param n 矩阵2的列数
 * @param output_matrix 输出矩阵，大小为 m x n
 * @return 0 表示成功，非0 表示失败
 * 
 * @note 调用此函数前必须先调用calculator_init()进行初始化
 */
int calculator_gemm_cpp(const float* matrix1,
						const float* matrix2,
						uint32_t m, uint32_t k, uint32_t n,
						float* output_matrix,
						bool transX, bool transY);

#endif // CALCULATOR_H
