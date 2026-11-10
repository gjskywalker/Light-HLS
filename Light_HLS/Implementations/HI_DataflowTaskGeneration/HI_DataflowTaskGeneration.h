#ifndef HI_DATAFLOW_TASK_GENERATION_H
#define HI_DATAFLOW_TASK_GENERATION_H

#include "HI_BackendContext.h"
#include "llvm/Pass.h"

class HI_DataflowTaskGeneration : public llvm::ModulePass
{
public:
    static char ID;

    explicit HI_DataflowTaskGeneration(
        light_hls::backend::HLSBackendContextPtr context);

    bool runOnModule(llvm::Module &module) override;
    void getAnalysisUsage(llvm::AnalysisUsage &usage) const override;

private:
    light_hls::backend::HLSBackendContextPtr context;
};

#endif
