#ifndef HI_RESOURCE_BINDING_H
#define HI_RESOURCE_BINDING_H

#include "HI_BackendContext.h"
#include "llvm/Pass.h"

class HI_ResourceBinding : public llvm::ModulePass
{
public:
    static char ID;

    explicit HI_ResourceBinding(
        light_hls::backend::HLSBackendContextPtr context);

    bool runOnModule(llvm::Module &module) override;
    void getAnalysisUsage(llvm::AnalysisUsage &usage) const override;

private:
    light_hls::backend::HLSBackendContextPtr context;
};

#endif
