#ifndef HI_BACKEND_VALIDATION_H
#define HI_BACKEND_VALIDATION_H

#include "HI_BackendContext.h"
#include "llvm/Pass.h"

class HI_BackendValidation : public llvm::ModulePass
{
public:
    static char ID;

    HI_BackendValidation(light_hls::backend::HLSBackendContextPtr context,
                         light_hls::backend::ValidationStage stage);

    bool runOnModule(llvm::Module &module) override;
    void getAnalysisUsage(llvm::AnalysisUsage &usage) const override;

private:
    light_hls::backend::HLSBackendContextPtr context;
    light_hls::backend::ValidationStage stage;
};

#endif
