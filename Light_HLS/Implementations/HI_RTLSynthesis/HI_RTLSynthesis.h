#ifndef HI_RTL_SYNTHESIS_H
#define HI_RTL_SYNTHESIS_H

#include "HI_BackendContext.h"
#include "llvm/Pass.h"

class HI_RTLSynthesis : public llvm::ModulePass
{
public:
    static char ID;

    explicit HI_RTLSynthesis(
        light_hls::backend::HLSBackendContextPtr context);

    bool runOnModule(llvm::Module &module) override;
    void getAnalysisUsage(llvm::AnalysisUsage &usage) const override;

private:
    light_hls::backend::HLSBackendContextPtr context;
};

#endif
