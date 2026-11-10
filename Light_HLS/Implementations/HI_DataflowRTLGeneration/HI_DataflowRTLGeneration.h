#ifndef HI_DATAFLOW_RTL_GENERATION_H
#define HI_DATAFLOW_RTL_GENERATION_H

#include "HI_BackendContext.h"
#include "llvm/Pass.h"

class HI_DataflowRTLGeneration : public llvm::ModulePass
{
public:
    static char ID;

    explicit HI_DataflowRTLGeneration(
        light_hls::backend::HLSBackendContextPtr context);

    bool runOnModule(llvm::Module &module) override;
    void getAnalysisUsage(llvm::AnalysisUsage &usage) const override;

private:
    light_hls::backend::HLSBackendContextPtr context;
};

#endif
