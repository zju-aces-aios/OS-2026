cmake_minimum_required(VERSION 3.16)

project(npu_gemm_cli)

add_executable(npu_gemm_test npu_gemm_cli.cpp calculator-api.cpp ${CMAKE_CURRENT_SOURCE_DIR}/dsp/hexagon_Debug_toolv88_v79/calculator_stub.c)

target_include_directories(npu_gemm_test PRIVATE
	${HEXAGON_SDK_ROOT}/incs/
	${HEXAGON_SDK_ROOT}/incs/stddef/
	${HEXAGON_SDK_ROOT}/ipc/fastrpc/rpcmem/inc/
	${HEXAGON_SDK_ROOT}/utils/examples
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/dsp/hexagon_Debug_toolv88_v79/
)

find_library(log-lib log)

set(CDSRPC_LIB_PATH "${HEXAGON_SDK_ROOT}/ipc/fastrpc/remote/ship/android_aarch64/libcdsprpc.so")
if(NOT EXISTS ${CDSRPC_LIB_PATH})
	message(FATAL_ERROR "Hexagon RPC library not found at ${CDSRPC_LIB_PATH}")
endif()

target_link_libraries(npu_gemm_test
	${log-lib}
	${CDSRPC_LIB_PATH}
)