cmake_minimum_required(VERSION 3.10)
project(HI_DataflowTaskGeneration)

add_library(HI_DataflowTaskGeneration HI_DataflowTaskGeneration.cc)
target_link_libraries(HI_DataflowTaskGeneration PUBLIC
  HI_BackendCommon
  HI_BackendValidation
  HI_MemoryObjectAnalysis
  HI_LoopPipelineAnalysis
  HI_ArrayPartitionAnalysis
  HI_StorageBindingAnalysis
  HI_BackendDependenceAnalysis
  HI_OperationScheduling
  HI_PipelineStorageAnalysis
  HI_ResourceBinding
  HI_RTLGeneration)
