#ifndef HI_PIPELINE_VALUE_LIFETIME_H
#define HI_PIPELINE_VALUE_LIFETIME_H

#include "HI_BackendContext.h"

namespace light_hls::backend
{

// Follow combinational expressions to the edges that actually sample a value.
// Return false for cross-iteration or cross-block uses requiring a fallback.
bool pipelineSamplingCycles(const HLSBackendContext &context, const LoopPipelineInfo &pipeline,
                            const llvm::Value *value,
                            const std::map<const llvm::Instruction *, int> &startCycles,
                            std::set<int> &cycles, bool includeRetirement = false,
                            bool *retirementOnly = nullptr);

unsigned pipelineValueStorageSlots(const LoopPipelineInfo &pipeline, const llvm::Value *value);

} // namespace light_hls::backend

#endif
