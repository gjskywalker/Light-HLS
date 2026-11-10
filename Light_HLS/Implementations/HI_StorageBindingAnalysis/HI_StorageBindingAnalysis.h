#ifndef HI_STORAGE_BINDING_ANALYSIS_H
#define HI_STORAGE_BINDING_ANALYSIS_H

#include "HI_BackendContext.h"
#include "llvm/Pass.h"

class HI_StorageBindingAnalysis : public llvm::ModulePass
{
public:
    static char ID;

    explicit HI_StorageBindingAnalysis(
        light_hls::backend::HLSBackendContextPtr context);

    bool runOnModule(llvm::Module &module) override;
    void getAnalysisUsage(llvm::AnalysisUsage &usage) const override;

private:
    light_hls::backend::HLSBackendContextPtr context;
};

#endif
