#include "calculator-api.h"
#include <vector>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <chrono> 
#include <stddef/AEEStdErr.h>
#include <android/log.h>
#include "rpcmem.h"
#include "remote.h"
#include "calculator.h"
#include "os_defines.h"

using namespace std;

const char *TAG = "calculator";

// This function sets DSP_LIBRARY_PATH environment variable
int calculator_init(const char* dsp_library_path) {
    if (setenv("DSP_LIBRARY_PATH", dsp_library_path, 1) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Failed to set DSP_LIBRARY_PATH");
        return -1;
    }
    __android_log_print(ANDROID_LOG_INFO, TAG, "DSP_LIBRARY_PATH set to: %s", dsp_library_path);
    return 0;
}

int calculator_gemm_cpp(const float* matrix1,
						const float* matrix2,
						uint32_t m, uint32_t k, uint32_t n,
						float* output_matrix,
						bool transX, bool transY) {

    using std::chrono::high_resolution_clock;
    using std::chrono::duration;

    // 1. 开始总计时
    auto total_start_time = high_resolution_clock::now();
	auto total_end_time = high_resolution_clock::now();

    const char* dsp_path = "/data/local/tmp";
	__android_log_print(ANDROID_LOG_DEBUG, TAG, "初始化 DSP 环境");

    // ==================== 阶段 1: DSP 环境初始化 ====================
    auto start_time = high_resolution_clock::now();
    if (calculator_init(dsp_path) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "初始化失败");
        return -1;
    }
    auto end_time = high_resolution_clock::now();
    duration<double, std::milli> duration_ = end_time - start_time;
    __android_log_print(ANDROID_LOG_INFO, TAG, "[PROFILING] DSP 环境初始化耗时: %.3f ms", duration_.count());
    // =============================================================

    remote_handle64 handle = 0;
    char* uri = nullptr;
    int nErr = 0;
    int ret = 0;

    float* dsp_matrix1 = nullptr;
    float* dsp_matrix2 = nullptr;
    float* dsp_output = nullptr;

    size_t matrix1_bytes = m * k * sizeof(float);
    size_t matrix2_bytes = k * n * sizeof(float);
    size_t output_bytes = m * n * sizeof(float);

    __android_log_print(ANDROID_LOG_INFO, TAG, "GEMM: m=%u, k=%u, n=%u", m, k, n);

    // ==================== 阶段 2: ION 内存分配 ====================
    __android_log_print(ANDROID_LOG_INFO, TAG, "GEMM: Allocating ION memory for inputs and output...");
    start_time = high_resolution_clock::now();
    dsp_matrix1 = (float*)rpcmem_alloc(RPCMEM_HEAP_ID_SYSTEM, RPCMEM_DEFAULT_FLAGS, matrix1_bytes);
    if (!dsp_matrix1) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "GEMM Error: rpcmem_alloc failed for matrix1.");
        ret = -1;
        goto bail;
    }

    dsp_matrix2 = (float*)rpcmem_alloc(RPCMEM_HEAP_ID_SYSTEM, RPCMEM_DEFAULT_FLAGS, matrix2_bytes);
    if (!dsp_matrix2) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "GEMM Error: rpcmem_alloc failed for matrix2.");
        ret = -1;
        goto bail;
    }
    
    dsp_output = (float*)rpcmem_alloc(RPCMEM_HEAP_ID_SYSTEM, RPCMEM_DEFAULT_FLAGS, output_bytes);
    if (!dsp_output) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "GEMM Error: rpcmem_alloc failed for output.");
        ret = -1;
        goto bail;
    }
    end_time = high_resolution_clock::now();
    duration_ = end_time - start_time;
    __android_log_print(ANDROID_LOG_INFO, TAG, "[PROFILING] ION 内存分配耗时: %.3f ms", duration_.count());
    // =============================================================

    // ==================== 阶段 3: 数据传输 (H2D) ====================
    start_time = high_resolution_clock::now();
    memcpy(dsp_matrix1, matrix1, matrix1_bytes);
    memcpy(dsp_matrix2, matrix2, matrix2_bytes);
    end_time = high_resolution_clock::now();
    duration_ = end_time - start_time;
    __android_log_print(ANDROID_LOG_INFO, TAG, "[PROFILING] 数据传输 (Host -> Device) 耗时: %.3f ms", duration_.count());
    // =============================================================

    __android_log_print(ANDROID_LOG_INFO, TAG, "GEMM: Opening handle...");
    uri = (char*)calculator_URI "&_dom=cdsp";
    
    // ==================== 阶段 4: DSP 句柄准备 ====================
    start_time = high_resolution_clock::now();
    // TODO: remote_session_control 应该只需要在进程生命周期内调用一次，
    if(remote_session_control) {
        struct remote_rpc_control_unsigned_module data;
        data.enable = 1;
        data.domain = CDSP_DOMAIN_ID;
        if (0 != (nErr = remote_session_control(DSPRPC_CONTROL_UNSIGNED_MODULE, (void*)&data, sizeof(data)))) {
            __android_log_print(ANDROID_LOG_ERROR, TAG, "GEMM Error: remote_session_control failed, returned 0x%x", nErr);
            ret = -1;
            goto bail;
        }
    } else {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "GEMM Error: remote_session_control is not supported.");
        ret = -1;
        goto bail;
    }

    nErr = calculator_open(uri, &handle);
    if (nErr != 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "GEMM Error: Handle open failed, returned 0x%x", nErr);
        ret = -1;
        goto bail;
    }
    end_time = high_resolution_clock::now();
    duration_ = end_time - start_time;
    __android_log_print(ANDROID_LOG_INFO, TAG, "[PROFILING] DSP 句柄准备耗时: %.3f ms", duration_.count());
    // =============================================================


    __android_log_print(ANDROID_LOG_INFO, TAG, "GEMM: Calling remote function calculator_gemm...");

    // ==================== 阶段 5: DSP/NPU 执行 ====================
    start_time = high_resolution_clock::now();
    nErr = calculator_gemm(handle,
							dsp_matrix1, m * k,
							dsp_matrix2, k * n,
							dsp_output, m * n,
							m, k, n,
							transX, transY);
    end_time = high_resolution_clock::now();
    duration_ = end_time - start_time;
    __android_log_print(ANDROID_LOG_INFO, TAG, "[PROFILING] DSP/NPU 计算耗时: %.3f ms", duration_.count());
    // =============================================================
                           
    if (nErr != 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "GEMM Error: calculator_gemm call failed, returned 0x%x", nErr);
        ret = -1;
    } else {
        __android_log_print(ANDROID_LOG_INFO, TAG, "GEMM: Remote call successful.");
        // ==================== 阶段 6: 数据传回 (D2H) ====================
        start_time = high_resolution_clock::now();
        memcpy(output_matrix, dsp_output, output_bytes);
        end_time = high_resolution_clock::now();
        duration_ = end_time - start_time;
        __android_log_print(ANDROID_LOG_INFO, TAG, "[PROFILING] 数据传回 (Device -> Host) 耗时: %.3f ms", duration_.count());
        // =============================================================
    }


bail:
    __android_log_print(ANDROID_LOG_INFO, TAG, "GEMM: Cleaning up resources...");
    start_time = high_resolution_clock::now();
    if (handle) {
        if (calculator_close(handle) != 0) {
            __android_log_print(ANDROID_LOG_ERROR, TAG, "GEMM Warning: Handle close failed.");
        }
    }
    if (dsp_matrix1) rpcmem_free(dsp_matrix1);
    if (dsp_matrix2) rpcmem_free(dsp_matrix2);
    if (dsp_output) rpcmem_free(dsp_output);
    end_time = high_resolution_clock::now();
    duration_ = end_time - start_time;
    __android_log_print(ANDROID_LOG_INFO, TAG, "[PROFILING] 资源清理耗时: %.3f ms", duration_.count());
    // =============================================================

    // 2. 结束总计时
    total_end_time = high_resolution_clock::now();
    duration_ = total_end_time - total_start_time;
    __android_log_print(ANDROID_LOG_INFO, TAG, "[PROFILING] 函数总执行耗时: %.3f ms", duration_.count());

    return ret;
}
