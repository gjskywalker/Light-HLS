#ifndef HI_DATAFLOW_PERFORMANCE_ANALYSIS_H
#define HI_DATAFLOW_PERFORMANCE_ANALYSIS_H

#include "HI_BackendContext.h"
#include "llvm/Pass.h"

class HI_DataflowPerformanceAnalysis : public llvm::ModulePass
{
public:
    static char ID;

    explicit HI_DataflowPerformanceAnalysis(
        light_hls::backend::HLSBackendContextPtr context);

    bool runOnModule(llvm::Module &module) override;
    void getAnalysisUsage(llvm::AnalysisUsage &usage) const override;

private:
    light_hls::backend::HLSBackendContextPtr context;
};

#endif
