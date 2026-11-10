project(Light_HLS_Top) 

set(Outputname "Light_HLS_Top")

cmake_minimum_required(VERSION 3.10) 

find_package(LLVM REQUIRED CONFIG) 
find_package(Polly) 
find_library(HI_POLLY_LIBRARY NAMES Polly HINTS ${LLVM_LIBRARY_DIRS} REQUIRED)
find_library(HI_POLLY_ISL_LIBRARY NAMES PollyISL HINTS ${LLVM_LIBRARY_DIRS} REQUIRED)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}") 
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}") 


message(STATUS "Using PollyConfig.cmake in: ${Polly_CMAKE_DIR}") 

# Needed to use support library 
message(STATUS "C++ flags==== ${CMAKE_CXX_FLAGS}") 
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -std=c++17 -g") 

include_directories(${LLVM_INCLUDE_DIRS}) 
include_directories(${Polly_INCLUDE_DIRS}) 

include_directories(./ 
../../Implementations/ 
../../Implementations/Include
../../Implementations/HI_ArrayInfo
../../Implementations/HI_FindFunctions 
../../Implementations/HI_print 
../../Implementations/HI_LoopInFormationCollect 
../../Implementations/HI_DependenceList 
../../Implementations/HI_SysExec 
../../Implementations/HI_Polly_Info 
../../Implementations/HI_LoopDependenceAnalysis
../../Implementations/HI_SimpleTimingEvaluation
../../Implementations/HI_DuplicateInstRm
../../Implementations/HI_VarWidthReduce
../../Implementations/HI_SeparateConstOffsetFromGEP
../../Implementations/HI_NoDirectiveTimingResourceEvaluation
../../Implementations/HI_InstructionMoveBackward
../../Implementations/HI_HLSDuplicateInstRm
../../Implementations/HI_AggressiveLSR_MUL
../../Implementations/HI_ArrayAccessPattern
../../Implementations/HI_WithDirectiveTimingResourceEvaluation
../../Implementations/HI_StringProcess
../../Implementations/HI_FunctionInstantiation
../../Implementations/HI_ReplaceSelectAccess
../../Implementations/HI_LoopUnroll
../../Implementations/HI_RemoveRedundantAccess
../../Implementations/HI_IR2SourceCode
../../Implementations/HI_LoopLabeler
../../Implementations/HI_FunctionInterfaceInfo
../../Implementations/HI_Mul2Shl
../../Implementations/HI_MulOrderOpt
../../Implementations/HI_FaddOrderOpt
../../Implementations/HI_MuxInsertionArrayPartition
../../Implementations/HI_LoadALAP
../../Implementations/HI_PragmaTargetExtraction
../../Implementations/HI_BackendCommon
../../Implementations/HI_BackendValidation
../../Implementations/HI_MemoryObjectAnalysis
../../Implementations/HI_LoopPipelineAnalysis
../../Implementations/HI_ArrayPartitionAnalysis
../../Implementations/HI_StorageBindingAnalysis
../../Implementations/HI_DataflowAnalysis
../../Implementations/HI_DataflowTaskGeneration
../../Implementations/HI_DataflowPerformanceAnalysis
../../Implementations/HI_DataflowRTLGeneration
../../Implementations/HI_BackendDependenceAnalysis
../../Implementations/HI_OperationScheduling
../../Implementations/HI_PipelineStorageAnalysis
../../Implementations/HI_ResourceBinding
../../Implementations/HI_RTLGeneration
../../Implementations/HI_VerilogEmission
../../Implementations/HI_RTLSynthesis
)

link_directories("../LLVM_Learner_Libs")
link_directories(${LLVM_LIBRARY_DIRS})

option(HI_USE_LLD "Link Light_HLS_Top with lld" OFF)
find_program(HI_LLD_LINKER NAMES ld.lld)
if(HI_USE_LLD AND HI_LLD_LINKER AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld")
endif()

option(HI_BUILD_LEARNER_LIBS "Build and stage the learner static libraries" ON)
set(HI_LEARNER_BUILD_JOBS "8" CACHE STRING
  "Parallel jobs for building the learner static libraries")
if(HI_BUILD_LEARNER_LIBS)
  set(MAKE_CMD "${CMAKE_CURRENT_SOURCE_DIR}/../../BuildLibs.sh")
  set(HI_LEARNER_STAMP "${CMAKE_CURRENT_BINARY_DIR}/hi_learner_libs.stamp")
  file(GLOB_RECURSE HI_LEARNER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../../Implementations/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../Implementations/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../Implementations/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../Implementations/CMakeLists.txt")
  add_custom_command(
    OUTPUT "${HI_LEARNER_STAMP}"
    COMMAND "${MAKE_CMD}" --parallel "${HI_LEARNER_BUILD_JOBS}"
      "-DLLVM_DIR:PATH=${LLVM_DIR}" "-DPolly_DIR:PATH=${Polly_DIR}"
      "-DCMAKE_C_COMPILER:FILEPATH=${CMAKE_C_COMPILER}"
      "-DCMAKE_CXX_COMPILER:FILEPATH=${CMAKE_CXX_COMPILER}"
      "-DCMAKE_BUILD_TYPE:STRING=${CMAKE_BUILD_TYPE}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${HI_LEARNER_STAMP}"
    DEPENDS ${HI_LEARNER_SOURCES} "${MAKE_CMD}"
      "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
      "${CMAKE_CURRENT_SOURCE_DIR}/../LLVM_Learner_Libs/CMakeLists.txt"
    COMMENT "Building and staging HI learner libraries"
    VERBATIM)
  add_custom_target(HI_Learner_Libs_Build DEPENDS "${HI_LEARNER_STAMP}")
endif()
add_definitions(${LLVM_DEFINITIONS}) 
add_definitions(${Polly_DEFINITIONS}) 

add_executable(${Outputname} Light_HLS_Top.cc ConfigParse.cc Pre_Process.cc) 
target_compile_definitions(${Outputname} PRIVATE
  LIGHT_HLS_INCLUDE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/include")
if(HI_BUILD_LEARNER_LIBS)
  add_dependencies(${Outputname} HI_Learner_Libs_Build)
endif()

file(GLOB HI_LEARNER_ARCHIVES
  "${CMAKE_CURRENT_SOURCE_DIR}/../LLVM_Learner_Libs/libHI_*.a")
set(HI_LINK_DEPENDS ${HI_LEARNER_ARCHIVES})
if(HI_BUILD_LEARNER_LIBS)
  list(APPEND HI_LINK_DEPENDS "${HI_LEARNER_STAMP}")
endif()
set_property(TARGET ${Outputname} APPEND PROPERTY LINK_DEPENDS
  ${HI_LINK_DEPENDS})

llvm_map_components_to_libnames(llvm_libs support core irreader engine nativecodegen) 

target_link_libraries(${Outputname} pthread dl 
HI_FindFunctions 
HI_print 
HI_ArrayInfo
HI_DependenceList 
HI_SysExec 
HI_LoopInFormationCollect
HI_Polly_Info HI_LoopDependenceAnalysis 
HI_SimpleTimingEvaluation 
HI_DuplicateInstRm 
HI_VarWidthReduce 
HI_SeparateConstOffsetFromGEP
HI_NoDirectiveTimingResourceEvaluation
HI_InstructionMoveBackward
HI_HLSDuplicateInstRm
HI_AggressiveLSR_MUL
HI_ArrayAccessPattern
HI_WithDirectiveTimingResourceEvaluation
HI_StringProcess
HI_FunctionInstantiation
HI_ReplaceSelectAccess
HI_LoopUnroll
HI_RemoveRedundantAccess
HI_IR2SourceCode
HI_LoopLabeler
HI_FunctionInterfaceInfo
HI_Mul2Shl
HI_MulOrderOpt
HI_FaddOrderOpt
HI_MuxInsertionArrayPartition
HI_LoadALAP
HI_PragmaTargetExtraction
HI_BackendValidation
HI_MemoryObjectAnalysis
HI_LoopPipelineAnalysis
HI_ArrayPartitionAnalysis
HI_StorageBindingAnalysis
HI_DataflowAnalysis
HI_DataflowTaskGeneration
HI_DataflowPerformanceAnalysis
HI_DataflowRTLGeneration
HI_BackendDependenceAnalysis
HI_OperationScheduling
HI_PipelineStorageAnalysis
HI_ResourceBinding
HI_RTLGeneration
HI_VerilogEmission
HI_RTLSynthesis
HI_BackendCommon
) #link llvm component 

target_link_libraries(${Outputname} ${HI_POLLY_LIBRARY} ${HI_POLLY_ISL_LIBRARY})

message(STATUS "Polly Libraries are: ${HI_POLLY_LIBRARY};${HI_POLLY_ISL_LIBRARY}")


# set(llvm_libs "")

#link llvm library 
target_link_libraries(${Outputname} 
LLVMLTO 
LLVMPasses 
LLVMObjCARCOpts 
LLVMMIRParser 
LLVMSymbolize 
LLVMDebugInfoPDB 
LLVMDebugInfoDWARF 
LLVMCoverage 
LLVMMCA 
LLVMTableGen 
LLVMDlltoolDriver 
LLVMXRay 
LLVMOrcJIT 
LLVMXCoreDisassembler 
LLVMXCoreCodeGen 
LLVMXCoreDesc 
LLVMXCoreInfo 
LLVMObjectYAML 
LLVMLibDriver 
LLVMOption 
LLVMWindowsManifest 
LLVMTextAPI 
LLVMFuzzMutate 
LLVMX86Disassembler 
LLVMX86AsmParser 
LLVMX86CodeGen 
LLVMGlobalISel 
LLVMSelectionDAG 
LLVMAsmPrinter 
LLVMX86Desc 
LLVMMCDisassembler 
LLVMX86Info 
LLVMMCJIT 
LLVMLineEditor 
LLVMInterpreter 
LLVMExecutionEngine 
LLVMRuntimeDyld 
LLVMCodeGen 
LLVMTarget 
LLVMCoroutines 
LLVMipo 
LLVMInstrumentation 
LLVMVectorize 
LLVMScalarOpts 
LLVMLinker 
LLVMIRReader 
LLVMAsmParser 
LLVMInstCombine 
LLVMBitWriter 
LLVMAggressiveInstCombine 
LLVMTransformUtils 
LLVMAnalysis 
LLVMProfileData 
LLVMObject 
LLVMMCParser 
LLVMMC 
LLVMDebugInfoCodeView 
LLVMDebugInfoMSF 
LLVMBitReader 
LLVMCore 
LLVMBinaryFormat 
LLVMSupport 
LLVMDemangle
clangFrontend 
clangParse 
clangSema 
clangSupport
clangAnalysis 
clangASTMatchers
clangAST 
clangLex 
clangBasic 
clangDriver 
clangSerialization 
clangFrontendTool
clangCodeGen 
clangStaticAnalyzerFrontend 
clangStaticAnalyzerCheckers 
clangStaticAnalyzerCore 
clangRewrite 
clangEdit 
clangTooling
clangARCMigrate
clangRewriteFrontend
clangLex
clangBasic
) 

# message(STATUS "LLVM Libraries0 are: ${llvm_libs}") 
# message(STATUS "LLVM Libraries1 are: ${ARG_LINK_COMPONENTS}") 
# message(STATUS "LLVM Libraries2 are: ${LLVM_LINK_COMPONENTS}") 
# message(STATUS "LLVM Definitions are: ${LLVM_PACKAGE_VERSION}") 

# The experimental driver keeps the production source-to-RTL flow unchanged.
# It links the same backend but does not run optional post-search optimizations.
add_executable(Light_HLS_DAPO EXCLUDE_FROM_ALL Light_HLS_DAPO.cc ConfigParse.cc Pre_Process.cc)
target_compile_definitions(Light_HLS_DAPO PRIVATE
  LIGHT_HLS_INCLUDE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/include")
get_target_property(HI_DAPO_LINK_LIBRARIES ${Outputname} LINK_LIBRARIES)
target_link_libraries(Light_HLS_DAPO PRIVATE ${HI_DAPO_LINK_LIBRARIES})
set_property(TARGET Light_HLS_DAPO APPEND PROPERTY LINK_DEPENDS ${HI_LINK_DEPENDS})
if(HI_BUILD_LEARNER_LIBS)
  add_dependencies(Light_HLS_DAPO HI_Learner_Libs_Build)
endif()
