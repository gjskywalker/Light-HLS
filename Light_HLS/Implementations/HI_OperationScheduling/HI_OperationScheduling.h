#ifndef HI_OPERATION_SCHEDULING_H
#define HI_OPERATION_SCHEDULING_H

#include "HI_BackendContext.h"
#include "llvm/Pass.h"

class HI_OperationScheduling : public llvm::ModulePass
{
public:
    static char ID;

    explicit HI_OperationScheduling(
        light_hls::backend::HLSBackendContextPtr context);

    bool runOnModule(llvm::Module &module) override;
    void getAnalysisUsage(llvm::AnalysisUsage &usage) const override;

private:
    light_hls::backend::HLSBackendContextPtr context;
};

#endif
