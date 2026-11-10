cmake_minimum_required(VERSION 3.10)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

aux_source_directory(. DIR_HI_RTL_SYNTHESIS_SRCS)

add_library(HI_RTLSynthesis ${DIR_HI_RTL_SYNTHESIS_SRCS})
target_link_libraries(HI_RTLSynthesis PUBLIC HI_BackendCommon)
