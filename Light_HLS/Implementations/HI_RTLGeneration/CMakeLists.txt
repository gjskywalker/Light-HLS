cmake_minimum_required(VERSION 3.10)

find_package(LLVM REQUIRED CONFIG)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -std=c++17")
include_directories(${LLVM_INCLUDE_DIRS} ../HI_BackendCommon)
add_definitions(${LLVM_DEFINITIONS})

aux_source_directory(. DIR_HI_RTL_GENERATION_SRCS)
add_library(HI_RTLGeneration ${DIR_HI_RTL_GENERATION_SRCS})
target_link_libraries(HI_RTLGeneration PUBLIC HI_BackendCommon)
