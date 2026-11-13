#include "HI_ResourceBinding.h"
#include "HI_PipelineValueLifetime.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <limits>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace llvm;
using namespace light_hls::backend;

namespace
{

using OperatorKey = std::tuple<std::string, unsigned, std::string>;
using TimeKey = std::pair<std::string, int>;
using OperatorAllocation = std::vector<std::vector<InstructionSchedule *>>;

std::vector<const Value *> integerOperands(const Instruction &instruction)
{
    std::vector<const Value *> values;
    for (const Use &operand : instruction.operands())
        if (scalarBitWidth(operand->getType()))
            values.push_back(operand.get());
    return values;
}

unsigned long long operandSelectionCost(const std::vector<InstructionSchedule *> &operations)
{
    if (operations.size() < 2)
        return 0;
    const auto representative = integerOperands(*operations.front()->instruction);
    unsigned long long cost = 0;
    for (unsigned index = 0; index < representative.size(); ++index)
    {
        std::set<const Value *> sources;
        for (const auto *operation : operations)
            sources.insert(integerOperands(*operation->instruction).at(index));
        // Count selector input bits, not LUTs. Pipeline instruction values can
        // denote different iterations even when their SSA identity agrees.
        const bool invariant = sources.size() == 1 &&
            (isa<ConstantInt>(*sources.begin()) || isa<Argument>(*sources.begin()));
        if (!invariant)
            cost += static_cast<unsigned long long>(scalarBitWidth(representative[index]->getType())) *
                    operations.size();
    }
    return cost;
}

unsigned long long allocationCost(const OperatorAllocation &allocation)
{
    unsigned long long cost = 0;
    for (const auto &instance : allocation)
        cost += operandSelectionCost(instance);
    return cost;
}

OperatorAllocation selectAllocation(
    const std::map<TimeKey, std::vector<InstructionSchedule *>> &operationsByTime,
    unsigned count, BindingResult &binding)
{
    OperatorAllocation ordinal(count), affinity(count);
    for (const auto &time : operationsByTime)
    {
        std::set<unsigned> used;
        for (unsigned index = 0; index < time.second.size(); ++index)
        {
            InstructionSchedule *operation = time.second[index];
            ordinal[index].push_back(operation);
            unsigned best = count;
            unsigned long long bestCost = std::numeric_limits<unsigned long long>::max();
            for (unsigned candidate = 0; candidate < count; ++candidate)
            {
                if (used.count(candidate))
                    continue;
                auto trial = affinity[candidate];
                trial.push_back(operation);
                const auto increment = operandSelectionCost(trial) - operandSelectionCost(affinity[candidate]);
                if (increment < bestCost)
                {
                    best = candidate;
                    bestCost = increment;
                }
            }
            affinity[best].push_back(operation);
            used.insert(best);
        }
    }
    const auto originalCost = allocationCost(ordinal);
    const auto candidateCost = allocationCost(affinity);
    binding.ordinalMuxBitCost += originalCost;
    binding.selectedMuxBitCost += std::min(originalCost, candidateCost);
    // Keep the old deterministic allocation unless the complete group wins.
    // FU count, schedule, and modulo-resource exclusivity do not change.
    if (candidateCost < originalCost)
    {
        ++binding.affinityReboundGroups;
        return affinity;
    }
    return ordinal;
}

std::string bindingOpcode(const Instruction &instruction)
{
    if (const auto *call = dyn_cast<IntrinsicInst>(&instruction))
        return call->getCalledFunction()->getName().str();
    if (const auto *comparison = dyn_cast<CmpInst>(&instruction))
    {
        return std::string(isa<FCmpInst>(instruction) ? "fcmp_" : "icmp_") +
               CmpInst::getPredicateName(comparison->getPredicate()).str();
    }
    return instruction.getOpcodeName();
}

std::string operandWidthSignature(const Instruction &instruction)
{
    std::string signature;
    for (const Use &operandUse : instruction.operands())
    {
        const unsigned type = scalarBitWidth(operandUse->getType());
        if (!type)
            continue;
        if (!signature.empty())
            signature += '_';
        signature += std::to_string(type);
    }
    return signature.empty() ? "none" : signature;
}

unsigned stateBitWidth(unsigned stateCount)
{
    if (stateCount <= 1)
        return 1;
    unsigned width = 0;
    unsigned maximumState = stateCount - 1;
    while (maximumState)
    {
        ++width;
        maximumState >>= 1;
    }
    return width;
}

unsigned countBitWidth(unsigned maximumValue)
{
    unsigned width = 0;
    do
    {
        ++width;
        maximumValue >>= 1;
    } while (maximumValue);
    return width;
}

unsigned operandMuxCount(const Instruction &instruction)
{
    if (isa<BinaryOperator>(instruction) || isa<ICmpInst>(instruction))
        return 2;
    if (isa<SelectInst>(instruction))
        return 3;
    if (isa<CastInst>(instruction) || isa<FreezeInst>(instruction))
        return 1;
    return instruction.getNumOperands();
}

bool isMemoryAddressInstruction(const Instruction &instruction)
{
    return isBackendLoad(&instruction) || isBackendStore(&instruction) ||
           isa<GetElementPtrInst>(instruction) || isa<PtrToIntInst>(instruction) ||
           isa<IntToPtrInst>(instruction) || isa<AddrSpaceCastInst>(instruction);
}

const LoopPipelineInfo *findPipeline(const HLSBackendContext &context,
                                     const BasicBlock *block)
{
    for (const LoopPipelineInfo &pipeline : context.pipelineRegions)
    {
        if (pipeline.header == block)
            return &pipeline;
    }
    return nullptr;
}

bool needsGlobalRegister(const HLSBackendContext &context,
                         const Instruction &instruction)
{
    if (context.combinationalValues.count(&instruction) || context.forwardedLoads.count(&instruction) ||
        context.constantValues.count(&instruction) || context.ordinaryRegisterAliases.count(&instruction))
        return false;
    const LoopPipelineInfo *pipeline =
        findPipeline(context, instruction.getParent());
    return !pipeline || pipeline->liveOutValues.count(&instruction) != 0;
}

int bindingTime(const HLSBackendContext &context, const BlockSchedule &block,
                int startCycle)
{
    const LoopPipelineInfo *pipeline = findPipeline(context, block.block);
    if (!pipeline || pipeline->achievedII <= 0)
        return startCycle;
    return startCycle % pipeline->achievedII;
}

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

std::vector<unsigned> accessBanks(const MemoryObject &memory,
                                  const MemoryAccessInfo &access)
{
    if (!access.possibleBanks.empty())
        return access.possibleBanks;
    std::vector<unsigned> banks;
    for (unsigned bank = 0; bank < memory.banks; ++bank)
        banks.push_back(bank);
    return banks;
}

} // namespace

char HI_ResourceBinding::ID = 0;

HI_ResourceBinding::HI_ResourceBinding(HLSBackendContextPtr context)
    : ModulePass(ID), context(std::move(context))
{
}

bool HI_ResourceBinding::runOnModule(Module &module)
{
    if (!context || context->hasErrors() || !context->schedule.valid ||
        !context->top)
    {
        return false;
    }

    context->module = &module;
    context->binding = BindingResult();
    context->ordinaryRegisterAliases.clear();
    for (const auto &bypass : context->ordinaryBackedgeBypasses)
    {
        const PHINode *phi = bypass.first;
        const Instruction *update = bypass.second;
        // Both values agree after a self-edge. Write their common register on
        // the exit edge too, preserving the final update for cross-block users.
        // An old-PHI live-out would need the previous value, so never alias it.
        const bool localPhi = std::all_of(phi->user_begin(), phi->user_end(),
            [&](const User *user) {
                const auto *instruction = dyn_cast<Instruction>(user);
                return instruction && (isa<DbgInfoIntrinsic>(instruction) ||
                    (instruction->getParent() == phi->getParent() && !isa<PHINode>(instruction)));
            });
        // Without a result live-out, synthesis already removes the unused
        // update register. Keep the original PHI enable in that case.
        const bool resultLiveOut = std::any_of(update->user_begin(), update->user_end(),
            [&](const User *user) {
                const auto *instruction = dyn_cast<Instruction>(user);
                return instruction && !isa<DbgInfoIntrinsic>(instruction) &&
                       instruction->getParent() != update->getParent();
            });
        if (localPhi && resultLiveOut && !context->ordinaryLiveOutExpressionRoots.count(phi) &&
            phi->getType() == update->getType() &&
            !context->ordinaryRegisterAliases.count(update))
            context->ordinaryRegisterAliases[update] = phi;
    }
    for (MemoryObject &memory : context->memoryObjects)
    {
        memory.portUsage.assign(memory.ports, MemoryPortUsage());
        memory.bankPortUsage.assign(
            memory.banks,
            std::vector<MemoryPortUsage>(memory.ports, MemoryPortUsage()));
    }

    std::map<OperatorKey, std::map<TimeKey, std::vector<InstructionSchedule *>>>
        operationsByTime;
    struct MemoryReservation {
        std::string block;
        const MemoryAccessInfo *access;
        int start;
        unsigned port;
    };
    std::vector<MemoryReservation> usedMemoryPorts;
    for (BlockSchedule &block : context->schedule.blocks)
    {
        for (InstructionSchedule &schedule : block.instructions)
        {
            if (!schedule.instruction || schedule.resultWidth == 0)
                continue;
            if (isMemoryAddressInstruction(*schedule.instruction) ||
                context->combinationalValues.count(schedule.instruction))
                continue;
            const OperatorKey key(bindingOpcode(*schedule.instruction),
                                  schedule.resultWidth,
                                  operandWidthSignature(*schedule.instruction) +
                                  (isIterativeFloating(*schedule.instruction)
                                       ? "_" + schedule.name : ""));
            operationsByTime[key][TimeKey(
                block.name, bindingTime(*context, block, schedule.startCycle))]
                .push_back(&schedule);
        }
    }

    std::map<OperatorKey, std::vector<std::size_t>> instanceIndices;
    for (const auto &operatorEntry : operationsByTime)
    {
        unsigned instanceCount = 0;
        for (const auto &timeEntry : operatorEntry.second)
            instanceCount = std::max<unsigned>(instanceCount,
                                               timeEntry.second.size());
        for (unsigned index = 0; index < instanceCount; ++index)
        {
            OperatorInstance instance;
            instance.opcode = std::get<0>(operatorEntry.first);
            instance.bitWidth = std::get<1>(operatorEntry.first);
            instance.name = "op_" +
                            sanitizeVerilogIdentifier(instance.opcode, "operation") +
                            "_" + std::to_string(instance.bitWidth) + "_" +
                            std::get<2>(operatorEntry.first) + "_" +
                            std::to_string(index);
            const InstructionSchedule *representative =
                operatorEntry.second.begin()->second.front();
            for (const Use &operandUse :
                 representative->instruction->operands())
            {
                const unsigned operandType = scalarBitWidth(operandUse->getType());
                if (operandType)
                    instance.operandWidths.push_back(operandType);
            }
            // Shared multipliers must accommodate every operation in the group,
            // not just the representative. Scheduling and latency are unchanged.
            if (representative->instruction->getOpcode() == Instruction::Mul)
                for (unsigned operand = 0; operand < instance.operandWidths.size(); ++operand)
                {
                    unsigned width = 1;
                    for (const auto &timeEntry : operatorEntry.second)
                        for (const InstructionSchedule *operation : timeEntry.second)
                            width = std::max(width, unsignedArithmeticWidth(
                                operation->instruction->getOperand(operand), context->module->getDataLayout()));
                    instance.operandWidths[operand] = width;
                }
            instanceIndices[operatorEntry.first].push_back(
                context->binding.operators.size());
            context->binding.operators.push_back(std::move(instance));
        }
    }


    for (BlockSchedule &block : context->schedule.blocks)
    {
        for (InstructionSchedule &schedule : block.instructions)
        {
            auto access = context->memoryAccesses.find(schedule.instruction);
            if (access == context->memoryAccesses.end())
                continue;
            MemoryAccessBinding binding;
            binding.instruction = schedule.instruction;
            binding.operation = schedule.name;
            binding.memory = access->second.memory;
            MemoryObject *memory = findMemoryObject(*context, binding.memory);
            if (!memory)
            {
                context->addError(
                    "BACKEND_MEMORY_BINDING_TARGET_UNKNOWN",
                    "Resource binding cannot find MemoryObject '" +
                        binding.memory + "'.",
                    binding.operation);
                continue;
            }
            binding.possibleBanks = accessBanks(*memory, access->second);
            binding.bankDynamic = binding.possibleBanks.size() != 1;
            binding.bank = binding.bankDynamic ? 0 : binding.possibleBanks.front();
            const LoopPipelineInfo *pipeline = findPipeline(*context, schedule.instruction->getParent());
            const unsigned interval = pipeline ? pipeline->achievedII : 0;
            // Scalar-bank input wires can fan out independently of the write
            // port. They do not reserve a RAM address/enable request slot.
            bool assigned = memory->hasCombinationalScalarReads() && !access->second.isWrite;
            binding.port = 0;
            for (unsigned port = 0; !assigned && port < memory->ports; ++port)
            {
                const bool available = std::none_of(usedMemoryPorts.begin(), usedMemoryPorts.end(),
                    [&](const MemoryReservation &prior) {
                        return prior.block == block.name && prior.port == port &&
                            memoryRequestsConflict(access->second, schedule.startCycle,
                                *prior.access, prior.start, interval);
                    });
                if (!available)
                    continue;
                binding.port = port;
                usedMemoryPorts.push_back({block.name, &access->second, schedule.startCycle, port});
                assigned = true;
                break;
            }
            if (!assigned)
            {
                context->addError(
                    "BACKEND_MEMORY_PORT_BINDING_OUT_OF_RANGE",
                    "Memory access '" + binding.operation +
                        "' cannot be assigned across its possible banks of MemoryObject '" +
                        binding.memory + "', which provides " +
                        std::to_string(memory->ports) +
                        " configured port(s).",
                    binding.operation);
                continue;
            }
            MemoryPortUsage &usage = memory->portUsage[binding.port];
            usage.hasWrite |= access->second.isWrite;
            usage.hasRead |= !access->second.isWrite;
            for (unsigned bank : binding.possibleBanks)
            {
                if (bank >= memory->bankPortUsage.size())
                    continue;
                MemoryPortUsage &bankUsage =
                    memory->bankPortUsage[bank][binding.port];
                bankUsage.hasWrite |= access->second.isWrite;
                bankUsage.hasRead |= !access->second.isWrite;
            }
            context->binding.memoryAccesses.push_back(std::move(binding));
        }
    }

    // A local RAM samples data only on its enabled write edge. A port with
    // one static writer needs no state/bank gating on the payload itself.
    // Internal PIPO payloads have the same edge-sampling contract. Keep
    // multiple writers and publicly visible interface data unchanged.
    using MemoryPortKey = std::tuple<std::string, unsigned, unsigned>;
    std::map<MemoryPortKey, std::vector<MemoryAccessBinding *>> localWriters;
    for (MemoryAccessBinding &binding : context->binding.memoryAccesses)
    {
        const auto access = context->memoryAccesses.find(binding.instruction);
        const MemoryObject *memory = findMemoryObject(*context, binding.memory);
        const bool internalPIPO = memory && memory->argument && context->configuration.rtlClockEnable &&
            context->configuration.internalPIPOWriteArguments.count(memory->argument->getArgNo()) &&
            !(memory->banks > 1 && memory->bankDepth == 1 && memory->banks == memory->depth);
        if (!memory || (memory->storage != MemoryStorageKind::LocalRAM && !internalPIPO) ||
            access == context->memoryAccesses.end() || !access->second.isWrite)
            continue;
        for (unsigned bank : binding.possibleBanks)
            localWriters[{binding.memory, bank, binding.port}].push_back(&binding);
    }
    for (const auto &entry : localWriters)
        if (entry.second.size() == 1)
            entry.second.front()->directWriteDataBanks.push_back(std::get<1>(entry.first));

    for (const auto &operatorEntry : operationsByTime)
    {
        const std::vector<std::size_t> &indices =
            instanceIndices.at(operatorEntry.first);
        const auto allocation = selectAllocation(operatorEntry.second, indices.size(), context->binding);
        for (std::size_t ordinal = 0; ordinal < allocation.size(); ++ordinal)
        {
            OperatorInstance &instance = context->binding.operators[indices[ordinal]];
            for (InstructionSchedule *schedule : allocation[ordinal])
            {
                instance.operations.push_back(schedule->name);
                context->binding.operations.push_back(
                    {schedule->instruction, schedule->name, instance.name});
            }
            if (allocation[ordinal].size() > 1)
            {
                const auto operands = integerOperands(*allocation[ordinal].front()->instruction);
                for (unsigned index = 0; index < operands.size(); ++index)
                    if ((isa<ConstantInt>(operands[index]) || isa<Argument>(operands[index])) &&
                        std::all_of(allocation[ordinal].begin(), allocation[ordinal].end(),
                            [&](const InstructionSchedule *operation) {
                                return integerOperands(*operation->instruction).at(index) == operands[index];
                            }))
                        instance.invariantOperands[index] = operands[index];
            }
        }
    }

    for (const Argument &argument : context->top->args())
    {
        if (const unsigned type = scalarBitWidth(argument.getType()))
        {
            ++context->binding.registerCount;
            context->binding.registerBits += type;
        }
    }
    for (const BasicBlock &block : *context->top)
    {
        for (const Instruction &instruction : block)
        {
            if (const unsigned type = scalarBitWidth(instruction.getType());
                type && needsGlobalRegister(*context, instruction))
            {
                ++context->binding.registerCount;
                context->binding.registerBits += type;
            }
        }
    }
    for (const LoopPipelineInfo &pipeline : context->pipelineRegions)
    {
        if (!pipeline.header || pipeline.contextSlots <= 1)
            continue;
        for (const Value *value : pipeline.contextValues)
        {
            if (pipeline.storageAliases.count(value))
                continue;
            const unsigned type = scalarBitWidth(value->getType());
            if (!type)
                continue;
            const unsigned slots = pipelineValueStorageSlots(pipeline, value);
            context->binding.registerCount += slots;
            context->binding.registerBits +=
                static_cast<unsigned long long>(slots) *
                type;
        }
        if (pipeline.stageIndexedController || !pipeline.derivedValues.empty())
        {
            const unsigned iterationWidth =
                countBitWidth(pipeline.tripCount - 1);
            const unsigned copies = pipeline.stageIndexedController ?
                pipeline.iterationTagLastStage + 1 : pipeline.contextSlots;
            context->binding.registerCount += copies;
            context->binding.registerBits +=
                static_cast<unsigned long long>(copies) *
                iterationWidth;
        }

        const unsigned slotWidth = countBitWidth(pipeline.contextSlots - 1);
        const unsigned launchWidth = countBitWidth(pipeline.tripCount);
        ++context->binding.registerCount;
        context->binding.registerBits += pipeline.pipelineLatency + 1;
        if (pipeline.lastTokenController)
        {
            ++context->binding.registerCount;
            context->binding.registerBits += pipeline.pipelineLatency + 1;
        }
        if (!pipeline.stageIndexedController)
        {
            context->binding.registerCount += pipeline.pipelineLatency + 2;
            context->binding.registerBits +=
                static_cast<unsigned long long>(pipeline.pipelineLatency + 2) * slotWidth;
        }
        ++context->binding.registerCount;
        context->binding.registerBits += launchWidth;
        if (pipeline.achievedII > 1)
        {
            ++context->binding.registerCount;
            context->binding.registerBits += countBitWidth(
                static_cast<unsigned>(pipeline.achievedII - 1));
        }
    }
    if (const unsigned returnType = scalarBitWidth(context->top->getReturnType()))
    {
        ++context->binding.registerCount;
        context->binding.registerBits += returnType;
    }

    context->binding.stateRegisterBits =
        stateBitWidth(context->schedule.stateCount);
    ++context->binding.registerCount;
    context->binding.registerBits += context->binding.stateRegisterBits;

    for (const OperatorInstance &instance : context->binding.operators)
    {
        if (instance.operations.size() <= 1)
            continue;
        const OperationBinding *representative = nullptr;
        for (const OperationBinding &binding : context->binding.operations)
        {
            if (binding.instance == instance.name)
            {
                representative = &binding;
                break;
            }
        }
        if (!representative || !representative->instruction)
            continue;
        const unsigned muxes = operandMuxCount(*representative->instruction) - instance.invariantOperands.size();
        context->binding.muxCount += muxes;
        context->binding.muxInputCount +=
            muxes * static_cast<unsigned>(instance.operations.size());
    }

    context->binding.valid = !context->hasErrors();
    return false;
}

void HI_ResourceBinding::getAnalysisUsage(AnalysisUsage &usage) const
{
    usage.setPreservesAll();
}
