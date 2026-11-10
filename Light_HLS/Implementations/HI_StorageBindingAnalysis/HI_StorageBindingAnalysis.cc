#include "HI_StorageBindingAnalysis.h"

#include <map>
#include <set>
#include <string>
#include <utility>

using namespace llvm;
using namespace light_hls::backend;

namespace
{

MemoryObject *findMemoryObject(HLSBackendContext &context,
                               const std::string &name)
{
    for (MemoryObject &memory : context.memoryObjects)
    {
        if (memory.name == name)
            return &memory;
    }
    return nullptr;
}

unsigned portsForType(MemoryBindingType type, unsigned currentPorts)
{
    switch (type)
    {
    case MemoryBindingType::RAM1P:
        return 1;
    case MemoryBindingType::RAM2P:
        return 2;
    case MemoryBindingType::Automatic:
        return currentPorts;
    }
    return currentPorts;
}

} // namespace

char HI_StorageBindingAnalysis::ID = 0;

HI_StorageBindingAnalysis::HI_StorageBindingAnalysis(
    HLSBackendContextPtr context)
    : ModulePass(ID), context(std::move(context))
{
}

bool HI_StorageBindingAnalysis::runOnModule(Module &module)
{
    if (!context || context->hasErrors() || !context->top)
        return false;

    context->module = &module;
    std::map<std::string, const StorageBindingDirective *> directives;
    for (const StorageBindingDirective &directive :
         context->configuration.storageBindings)
    {
        if (directive.scope != context->configuration.topFunction)
            continue;
        if (!directives.emplace(directive.memory, &directive).second)
        {
            context->addError(
                "BACKEND_STORAGE_BINDING_DUPLICATE",
                "MemoryObject '" + directive.memory +
                    "' has more than one BIND_STORAGE directive.",
                directive.memory);
        }
    }
    if (context->hasErrors())
        return false;

    std::set<std::string> resolved;
    for (const auto &entry : directives)
    {
        MemoryObject *memory = findMemoryObject(*context, entry.first);
        if (!memory)
            continue;
        resolved.insert(entry.first);
        const StorageBindingDirective &directive = *entry.second;
        if (memory->storage != MemoryStorageKind::LocalRAM)
        {
            context->addError(
                "BACKEND_STORAGE_BINDING_REQUIRES_LOCAL_MEMORY",
                "BIND_STORAGE target '" + memory->name +
                    "' is an external interface. Use INTERFACE storage_type "
                    "for top-level memories.",
                memory->name);
            continue;
        }

        memory->bindingType = directive.type;
        memory->implementation = directive.implementation;
        memory->storageBindingExplicit = true;
        memory->ports = portsForType(directive.type, memory->ports);
        memory->readLatency = directive.readLatency;
        memory->portUsage.assign(memory->ports, MemoryPortUsage());
        memory->bankPortUsage.assign(
            memory->banks,
            std::vector<MemoryPortUsage>(memory->ports, MemoryPortUsage()));
    }

    for (const auto &entry : directives)
    {
        if (!resolved.count(entry.first))
        {
            context->addError(
                "BACKEND_STORAGE_BINDING_TARGET_UNKNOWN",
                "BIND_STORAGE target '" + entry.first +
                    "' is not a MemoryObject in the top function.",
                entry.first);
        }
    }
    return false;
}

void HI_StorageBindingAnalysis::getAnalysisUsage(AnalysisUsage &usage) const
{
    usage.setPreservesAll();
}
