cmake_minimum_required(VERSION 3.10)

find_package(LLVM REQUIRED CONFIG)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -std=c++17")

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

aux_source_directory(. DIR_HI_BACKEND_COMMON_SRCS)
add_library(HI_BackendCommon ${DIR_HI_BACKEND_COMMON_SRCS})
target_compile_definitions(HI_BackendCommon PRIVATE
    LIGHT_HLS_FP_RESOURCE_MODEL="${CMAKE_CURRENT_SOURCE_DIR}/../../RTL/floating_resources_u250.cfg")
