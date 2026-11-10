#ifndef HI_VERILOG_EMISSION_H
#define HI_VERILOG_EMISSION_H

#include "HI_BackendContext.h"
#include "llvm/Pass.h"

class HI_VerilogEmission : public llvm::ModulePass
{
public:
    static char ID;

    explicit HI_VerilogEmission(
        light_hls::backend::HLSBackendContextPtr context);

    bool runOnModule(llvm::Module &module) override;
    void getAnalysisUsage(llvm::AnalysisUsage &usage) const override;

private:
    light_hls::backend::HLSBackendContextPtr context;
};

#endif
