#ifndef HI_MEMORY_OBJECT_ANALYSIS_H
#define HI_MEMORY_OBJECT_ANALYSIS_H

#include "HI_BackendContext.h"
#include "llvm/Pass.h"

class HI_MemoryObjectAnalysis : public llvm::ModulePass
{
public:
    static char ID;

    explicit HI_MemoryObjectAnalysis(
        light_hls::backend::HLSBackendContextPtr context);

    bool runOnModule(llvm::Module &module) override;
    void getAnalysisUsage(llvm::AnalysisUsage &usage) const override;

private:
    light_hls::backend::HLSBackendContextPtr context;
};

#endif
