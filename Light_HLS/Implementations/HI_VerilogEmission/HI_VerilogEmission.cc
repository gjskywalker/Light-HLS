#include "HI_VerilogEmission.h"

#include "llvm/Support/raw_ostream.h"

#include <string>
#include <utility>

using namespace llvm;
using namespace light_hls::backend;

char HI_VerilogEmission::ID = 0;

HI_VerilogEmission::HI_VerilogEmission(HLSBackendContextPtr context)
    : ModulePass(ID), context(std::move(context))
{
}

bool HI_VerilogEmission::runOnModule(Module &module)
{
    if (!context)
        return false;
    context->module = &module;
    std::string errorMessage;
    if (!emitBackendArtifacts(*context, errorMessage))
    {
        context->addError("BACKEND_ARTIFACT_WRITE_FAILED", errorMessage,
                          context->configuration.outputDirectory);
        errs() << "Light-HLS backend failed to write artifacts: " << errorMessage
               << "\n";
    }
    else
    {
        errs() << "Light-HLS backend artifacts: "
               << context->configuration.outputDirectory << "\n";
    }
    return false;
}

void HI_VerilogEmission::getAnalysisUsage(AnalysisUsage &usage) const
{
    usage.setPreservesAll();
}
