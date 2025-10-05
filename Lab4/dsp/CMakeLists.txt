# Sets the minimum version of CMake required to build your native library.
# This ensures that a certain set of CMake features is available to
# your build.

cmake_minimum_required(VERSION 3.14.3)

project("Calculator_plus_dsp" C CXX ASM)

enable_language(ASM)

if(HEXAGON_SDK_ROOT)
    include(${HEXAGON_SDK_ROOT}/build/cmake/hexagon_fun.cmake)
else()
    include(${HEXAGON_CMAKE_ROOT}/hexagon_fun.cmake)
endif()

set(common_incs
        ${CMAKE_CURRENT_BINARY_DIR}/
        ${HEXAGON_SDK_ROOT}/incs/
        ${HEXAGON_SDK_ROOT}/incs/stddef/
        ${HEXAGON_SDK_ROOT}/utils/examples
        )


include_directories(${common_incs})

set(srcs
        ${CMAKE_CURRENT_BINARY_DIR}/calculator_skel
        ${CMAKE_CURRENT_SOURCE_DIR}/calculator_imp
    )

add_library(calculator_skel SHARED ${srcs})

build_idl("../inc/calculator.idl" calculator_skel)


add_library(test_calculator_sim SHARED ${srcs})

target_compile_definitions(test_calculator_sim PRIVATE SIMULATOR_TEST)

set_target_properties(test_calculator_sim PROPERTIES
    OUTPUT_NAME "test_calculator"
)

target_link_libraries(test_calculator_sim PRIVATE
    C
    pthreads
    posix
)