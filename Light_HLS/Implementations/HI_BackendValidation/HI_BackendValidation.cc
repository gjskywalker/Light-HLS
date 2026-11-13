#include "HI_BackendValidation.h"
#include "HI_PipelineValueLifetime.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include <string>

using namespace llvm;
using namespace light_hls::backend;

namespace
{

std::string instructionLocation(const Instruction &instruction)
{
    std::string text;
    raw_string_ostream output(text);
    output << instruction;
    output.flush();
    return instruction.getParent()->getName().str() + ": " + text;
}

bool isIgnoredIntrinsic(const Instruction &instruction)
{
    if (isa<DbgInfoIntrinsic>(instruction))
        return true;
    const auto *call = dyn_cast<CallBase>(&instruction);
    if (!call)
        return false;
    const Function *callee = call->getCalledFunction();
    if (!callee || !callee->isIntrinsic())
        return false;
    return callee->getIntrinsicID() == Intrinsic::lifetime_start ||
           callee->getIntrinsicID() == Intrinsic::lifetime_end;
}

bool isSupportedCast(const CastInst &cast)
{
    if (isa<FPTruncInst, FPExtInst>(cast))
        return isSupportedFloat(cast.getSrcTy()) && isSupportedFloat(cast.getDestTy());
    if (isa<SIToFPInst, UIToFPInst>(cast))
        return isSupportedFloat(cast.getDestTy()) &&
            cast.getSrcTy()->isIntegerTy() && scalarBitWidth(cast.getSrcTy()) >= 2 &&
            scalarBitWidth(cast.getSrcTy()) <= 64;
    if (isa<FPToSIInst, FPToUIInst>(cast))
        return isSupportedFloat(cast.getSrcTy()) &&
            cast.getDestTy()->isIntegerTy() && scalarBitWidth(cast.getDestTy()) >= 2 &&
            scalarBitWidth(cast.getDestTy()) <= 64;
    if (isa<BitCastInst>(cast))
        return scalarBitWidth(cast.getSrcTy()) &&
            scalarBitWidth(cast.getSrcTy()) == scalarBitWidth(cast.getDestTy());
    return cast.getSrcTy()->isIntegerTy() && cast.getDestTy()->isIntegerTy() &&
           (isa<TruncInst>(cast) || isa<ZExtInst>(cast) || isa<SExtInst>(cast));
}

bool isSupportedInstruction(const Instruction &instruction)
{
    if (isa<ExtractElementInst>(instruction) || isa<InsertElementInst>(instruction))
    {
        const auto *type = dyn_cast<FixedVectorType>(instruction.getOperand(0)->getType());
        const auto *index = dyn_cast<ConstantInt>(instruction.getOperand(instruction.getNumOperands() - 1));
        return type && type->getNumElements() == 1 && index && index->isZero() && scalarBitWidth(type);
    }
    if (isIgnoredIntrinsic(instruction))
        return true;
    if (const auto *intrinsic = dyn_cast<IntrinsicInst>(&instruction))
        return isSupportedFloatingIntrinsic(intrinsic->getIntrinsicID()) && isSupportedFloat(instruction.getType());
    if (const auto *phi = dyn_cast<PHINode>(&instruction))
        return scalarBitWidth(phi->getType()) != 0;
    if (const auto *comparison = dyn_cast<FCmpInst>(&instruction))
        return isSupportedFloat(comparison->getOperand(0)->getType());
    if (instruction.getOpcode() == Instruction::FNeg)
        return isSupportedFloat(instruction.getType());
    if (const auto *comparison = dyn_cast<ICmpInst>(&instruction))
        return comparison->getOperand(0)->getType()->isIntegerTy();
    if (const auto *select = dyn_cast<SelectInst>(&instruction))
        return scalarBitWidth(select->getType()) != 0;
    if (isa<BranchInst>(instruction) || isa<ReturnInst>(instruction) ||
        isa<FreezeInst>(instruction))
    {
        return true;
    }
    if (const auto *binary = dyn_cast<BinaryOperator>(&instruction))
    {
        switch (binary->getOpcode())
        {
        case Instruction::FAdd:
        case Instruction::FSub:
        case Instruction::FMul:
        case Instruction::FDiv:
            return isSupportedFloat(binary->getType());
        case Instruction::Add:
        case Instruction::Sub:
        case Instruction::Mul:
        case Instruction::UDiv:
        case Instruction::SDiv:
        case Instruction::URem:
        case Instruction::SRem:
        case Instruction::Shl:
        case Instruction::LShr:
        case Instruction::AShr:
        case Instruction::And:
        case Instruction::Or:
        case Instruction::Xor:
            return binary->getType()->isIntegerTy();
        default:
            return false;
        }
    }
    if (const auto *cast = dyn_cast<CastInst>(&instruction))
        return isSupportedCast(*cast);
    return false;
}

bool isSupportedMemoryInstruction(const Instruction &instruction,
                                  bool hasFIFOArguments)
{
    if (isMaskedMemory(&instruction, Intrinsic::masked_load) ||
        isMaskedMemory(&instruction, Intrinsic::masked_store))
    {
        const Value *value = isBackendLoad(&instruction) ? static_cast<const Value *>(&instruction) : backendStoredValue(&instruction);
        const auto *type = dyn_cast<FixedVectorType>(value->getType());
        const Value *predicate = backendMemoryPredicate(&instruction);
        // Predication emits an undefined inactive load result that is consumed
        // only on its active path. Defined passthrough values need separate
        // datapath support; do not silently accept those external IR forms.
        if (isBackendLoad(&instruction) &&
            !isa<UndefValue>(cast<CallBase>(instruction).getArgOperand(3)))
            return false;
        return type && type->getNumElements() == 1 && scalarBitWidth(type) &&
               predicate && predicate->getType()->isIntegerTy(1);
    }
    if (const auto *allocation = dyn_cast<AllocaInst>(&instruction))
    {
        const Type *type = allocation->getAllocatedType();
        while (const auto *array = dyn_cast<ArrayType>(type))
            type = array->getElementType();
        const auto *arraySize =
            dyn_cast<ConstantInt>(allocation->getArraySize());
        return scalarBitWidth(type) && arraySize &&
               !arraySize->isZero();
    }
    if (const auto *load = dyn_cast<LoadInst>(&instruction))
        return (!load->isVolatile() || hasFIFOArguments) && !load->isAtomic() &&
               scalarBitWidth(load->getType());
    if (const auto *store = dyn_cast<StoreInst>(&instruction))
        return (!store->isVolatile() || hasFIFOArguments) && !store->isAtomic() &&
               scalarBitWidth(store->getValueOperand()->getType());
    if (const auto *gep = dyn_cast<GetElementPtrInst>(&instruction))
    {
        Type *indexedType = gep->getSourceElementType();
        unsigned indexOrdinal = 0;
        for (const Value *index : gep->indices())
        {
            if (!index->getType()->isIntegerTy())
                return false;
            if (indexOrdinal > 0)
            {
                const auto *array = dyn_cast<ArrayType>(indexedType);
                if (!array)
                    return false;
                indexedType = array->getElementType();
            }
            ++indexOrdinal;
        }
        return true;
    }
    if (const auto *ptrToInt = dyn_cast<PtrToIntInst>(&instruction))
        return ptrToInt->getType()->isIntegerTy();
    if (const auto *intToPtr = dyn_cast<IntToPtrInst>(&instruction))
        return intToPtr->getOperand(0)->getType()->isIntegerTy();
    return false;
}

const MemoryObject *findMemoryObject(const HLSBackendContext &context,
                                     const std::string &name)
{
    for (const MemoryObject &memory : context.memoryObjects)
    {
        if (memory.name == name)
            return &memory;
    }
    return nullptr;
}

MemoryAccessMode memoryAccessMode(const MemoryPortUsage &usage)
{
    if (usage.hasRead && usage.hasWrite)
        return MemoryAccessMode::ReadWrite;
    if (usage.hasRead)
        return MemoryAccessMode::ReadOnly;
    if (usage.hasWrite)
        return MemoryAccessMode::WriteOnly;
    return MemoryAccessMode::Unused;
}

bool hasUnsupportedConstantOperand(const Instruction &instruction)
{
    for (const Use &operandUse : instruction.operands())
    {
        const Value *operand = operandUse.get();
        if (!isa<Constant>(operand))
            continue;
        if (isa<ConstantInt>(operand) || isa<ConstantFP>(operand) || isa<UndefValue>(operand) ||
            isa<PoisonValue>(operand))
        {
            continue;
        }
        if (isa<BasicBlock>(operand) || isa<Function>(operand))
            continue;
        return true;
    }
    return false;
}

void requireSupportedFeature(HLSBackendContext &context, bool required,
                             bool supported, const std::string &code,
                             const std::string &feature)
{
    if (required && !supported)
    {
        context.addError(code, "Required backend feature '" + feature +
                                   "' is not supported by the current RTL backend.");
    }
}

} // namespace

char HI_BackendValidation::ID = 0;

HI_BackendValidation::HI_BackendValidation(HLSBackendContextPtr context,
                                           ValidationStage stage)
    : ModulePass(ID), context(std::move(context)), stage(stage)
{
}

bool HI_BackendValidation::runOnModule(Module &module)
{
    if (!context)
        return false;

    context->module = &module;
    if (stage == ValidationStage::PostRTL)
    {
        if (!context->hasErrors() && !context->schedule.valid)
            context->addError("BACKEND_SCHEDULE_MISSING",
                              "RTL validation requires a valid operation schedule.");
        if (!context->hasErrors() && !context->binding.valid)
            context->addError("BACKEND_BINDING_MISSING",
                              "RTL validation requires a valid resource binding.");
        if (!context->hasErrors() &&
            (!context->rtl.valid || context->rtl.verilog.empty()))
        {
            context->addError("BACKEND_RTL_MISSING",
                              "RTL generation did not produce a Verilog module.");
        }
        if (!context->hasErrors() &&
            context->rtl.verilog.find("module " + context->rtl.moduleName) ==
                std::string::npos)
        {
            context->addError("BACKEND_RTL_MODULE_MISMATCH",
                              "The emitted Verilog does not contain the expected top module.");
        }
        if (!context->hasErrors())
        {
            std::map<const Instruction *, double> bypassDelays;
            std::function<double(const Instruction *)> bypassDelay = [&](const Instruction *value) {
                const auto cached = bypassDelays.find(value);
                if (cached != bypassDelays.end())
                    return cached->second;
                const bool wiring = (isa<CastInst>(value) &&
                    value->getOperand(0)->getType()->isIntegerTy()) || isa<FreezeInst>(value);
                const auto delay = context->delayModel.find(value);
                const auto latency = context->latencyModel.find(value);
                if (!value->getType()->isIntegerTy() || value->mayReadOrWriteMemory() ||
                    value->mayHaveSideEffects() ||
                    !(wiring || isa<BinaryOperator>(value) || isa<ICmpInst>(value) || isa<SelectInst>(value)) ||
                    (!wiring && (delay == context->delayModel.end() ||
                        !std::isfinite(delay->second) || delay->second < 0 ||
                        latency == context->latencyModel.end() || latency->second != 0)))
                    return static_cast<double>(INFINITY);
                double inputDelay = 0;
                for (const Use &operand : value->operands())
                    if (const auto *producer = dyn_cast<Instruction>(operand.get()))
                        if (context->combinationalValues.count(producer))
                            inputDelay = std::max(inputDelay, bypassDelay(producer));
                return bypassDelays[value] = inputDelay + (wiring ? 0.0 : delay->second);
            };
            for (const auto &bypass : context->ordinaryBackedgeBypasses)
            {
                const PHINode *phi = bypass.first;
                const Instruction *update = bypass.second;
                const BasicBlock *block = phi->getParent();
                const auto *branch = dyn_cast<BranchInst>(block->getTerminator());
                const auto operation = context->schedule.instructionMap.find(update);
                const auto blockSchedule = std::find_if(context->schedule.blocks.begin(),
                    context->schedule.blocks.end(),
                    [&](const BlockSchedule &schedule) { return schedule.block == block; });
                bool valid = update->getParent() == block && phi->getNumIncomingValues() == 2 &&
                    phi->getBasicBlockIndex(block) >= 0 &&
                    phi->getIncomingValueForBlock(block) == update &&
                    !context->combinationalValues.count(update) &&
                    !context->constantValues.count(update) && branch && branch->isConditional() &&
                    ((branch->getSuccessor(0) == block) != (branch->getSuccessor(1) == block)) &&
                    operation != context->schedule.instructionMap.end() &&
                    operation->second->latency == 1 &&
                    blockSchedule != context->schedule.blocks.end() && !blockSchedule->pipelined &&
                    operation->second->startCycle + 1 == static_cast<int>(blockSchedule->stateCount) &&
                    bypassDelay(update) <= context->configuration.clockPeriodNs * 0.7;
                for (const LoopPipelineInfo &pipeline : context->pipelineRegions)
                    valid &= pipeline.header != block &&
                        std::find(pipeline.blocks.begin(), pipeline.blocks.end(), block) == pipeline.blocks.end();
                valid &= std::count_if(context->binding.operations.begin(), context->binding.operations.end(),
                    [&](const OperationBinding &binding) { return binding.instruction == update; }) == 1;
                if (!valid)
                    context->addError("BACKEND_ORDINARY_BACKEDGE_BYPASS_INVALID",
                        "A self-edge bypass must sample a registered, characterized operator on its issue edge.",
                        instructionLocation(*update));
            }
            for (const auto &alias : context->ordinaryRegisterAliases)
            {
                const auto bypass = context->ordinaryBackedgeBypasses.find(alias.second);
                bool valid = bypass != context->ordinaryBackedgeBypasses.end() &&
                    bypass->second == alias.first && alias.first->getType() == alias.second->getType() &&
                    !context->ordinaryLiveOutExpressionRoots.count(alias.second);
                for (const User *user : alias.second->users())
                {
                    const auto *instruction = dyn_cast<Instruction>(user);
                    valid &= instruction && (isa<DbgInfoIntrinsic>(instruction) ||
                        (instruction->getParent() == alias.second->getParent() && !isa<PHINode>(instruction)));
                }
                if (!valid)
                    context->addError("BACKEND_ORDINARY_REGISTER_ALIAS_INVALID",
                        "Shared recurrence storage requires a validated issue-edge bypass and no old-PHI live-out.",
                        instructionLocation(*alias.first));
            }
            DominatorTree dominators(*context->top);
            if (const BasicBlock *successor = context->ordinaryEntrySuccessor)
            {
                const BasicBlock &entry = context->top->getEntryBlock();
                const auto *branch = dyn_cast<BranchInst>(entry.getTerminator());
                bool valid = branch && branch->isUnconditional() && branch->getSuccessor(0) == successor &&
                    context->schedule.blocks.front().stateCount == 0 &&
                    !context->ordinaryReturnExits.count(successor);
                for (const Instruction &instruction : entry)
                    valid &= &instruction == branch || isa<AllocaInst>(instruction) ||
                        isIgnoredIntrinsic(instruction) || context->constantValues.count(&instruction) ||
                        (context->combinationalValues.count(&instruction) &&
                         ordinaryEntryValueDelay(*context, &instruction) <= context->configuration.clockPeriodNs * 0.7);
                for (const PHINode &phi : successor->phis())
                {
                    const Value *initial = phi.getIncomingValueForBlock(&entry);
                    valid &= initial && ordinaryEntryValueDelay(*context, initial) <=
                        context->configuration.clockPeriodNs * 0.7;
                }
                for (const LoopPipelineInfo &pipeline : context->pipelineRegions)
                    valid &= pipeline.header != successor &&
                        std::find(pipeline.blocks.begin(), pipeline.blocks.end(), successor) == pipeline.blocks.end();
                if (!valid)
                    context->addError("BACKEND_ORDINARY_ENTRY_FUSION_INVALID",
                        "Entry fusion requires a timed pure preheader, live-port PHI initialization, and an ordinary successor.");
            }
            for (const BasicBlock *exit : context->ordinaryReturnExits)
            {
                const auto *ret = dyn_cast<ReturnInst>(exit->getTerminator());
                const auto schedule = std::find_if(context->schedule.blocks.begin(), context->schedule.blocks.end(),
                    [&](const BlockSchedule &block) { return block.block == exit; });
                bool valid = exit != &context->top->getEntryBlock() &&
                    ret && !pred_empty(exit) &&
                    schedule != context->schedule.blocks.end() && schedule->stateCount == 0;
                for (const Instruction &instruction : *exit)
                    valid &= &instruction == ret || isIgnoredIntrinsic(instruction) ||
                        context->constantValues.count(&instruction) || context->combinationalValues.count(&instruction);
                if (ret)
                    if (const auto *value = dyn_cast_or_null<Instruction>(ret->getReturnValue()))
                        if (context->combinationalValues.count(value))
                            valid &= bypassDelay(value) <= context->configuration.clockPeriodNs * 0.7;
                for (const BasicBlock *predecessor : predecessors(exit))
                {
                    valid &= isa<BranchInst>(predecessor->getTerminator());
                    for (const LoopPipelineInfo &pipeline : context->pipelineRegions)
                        valid &= pipeline.header != predecessor &&
                            std::find(pipeline.blocks.begin(), pipeline.blocks.end(), predecessor) == pipeline.blocks.end();
                }
                if (!valid)
                    context->addError("BACKEND_ORDINARY_RETURN_FUSION_INVALID",
                        "Return fusion requires a timed combinational exit reached only from ordinary branches.",
                        exit->getName().str());
            }
            LoopInfo loops(dominators);
            for (const auto &feedback : context->ordinaryFeedbackValues)
            {
                const auto *update = dyn_cast<BinaryOperator>(feedback.first);
                const PHINode *phi = feedback.second;
                const BasicBlock *block = feedback.first->getParent();
                const auto *branch = dyn_cast<BranchInst>(block->getTerminator());
                const bool constantStep = update &&
                    (((update->getOpcode() == Instruction::Add ||
                       update->getOpcode() == Instruction::Sub) &&
                         update->getOperand(0) == phi &&
                         isa<ConstantInt>(update->getOperand(1))) ||
                     (update->getOpcode() == Instruction::Add &&
                         update->getOperand(1) == phi &&
                         isa<ConstantInt>(update->getOperand(0))));
                const Loop *loop = loops.getLoopFor(block);
                bool valid = constantStep && loop && loop->getHeader() == phi->getParent() &&
                    loop->getLoopLatch() == block &&
                    phi->getNumIncomingValues() == 2 &&
                    phi->getBasicBlockIndex(block) >= 0 &&
                    phi->getIncomingValueForBlock(block) == update &&
                    context->combinationalValues.count(update) &&
                    branch && branch->isConditional() &&
                    ((branch->getSuccessor(0) == phi->getParent()) !=
                     (branch->getSuccessor(1) == phi->getParent()));
                for (const User *user : feedback.first->users())
                {
                    const auto *consumer = dyn_cast<Instruction>(user);
                    valid &= consumer && (isa<DbgInfoIntrinsic>(consumer) || consumer == phi ||
                        (consumer->getParent() == block &&
                         !isa<PHINode>(consumer)));
                }
                for (const LoopPipelineInfo &pipeline : context->pipelineRegions)
                    valid &= pipeline.header != block && pipeline.header != phi->getParent() &&
                        std::find(pipeline.blocks.begin(), pipeline.blocks.end(), block) ==
                            pipeline.blocks.end() &&
                        std::find(pipeline.blocks.begin(), pipeline.blocks.end(), phi->getParent()) ==
                            pipeline.blocks.end();
                if (!valid)
                    context->addError(
                        "BACKEND_ORDINARY_FEEDBACK_INVALID",
                        "A feedback wire requires a held ordinary-loop header PHI, a unique latch, a constant step, and latch-local users.",
                        instructionLocation(*feedback.first));
            }
            std::set<std::pair<const BasicBlock *, const PHINode *>> readPhiEdges;
            for (const OrdinaryReadPhiBypass &bypass : context->ordinaryReadPhiBypasses)
            {
                const auto operation = context->schedule.instructionMap.find(bypass.load);
                const auto access = context->memoryAccesses.find(bypass.load);
                const auto block = std::find_if(context->schedule.blocks.begin(),
                    context->schedule.blocks.end(), [&](const BlockSchedule &item) {
                        return item.block == bypass.predecessor;
                    });
                const MemoryObject *memory = nullptr;
                if (access != context->memoryAccesses.end())
                    for (const MemoryObject &item : context->memoryObjects)
                        if (item.name == access->second.memory)
                            memory = &item;
                bool valid = isa<LoadInst>(bypass.load) &&
                    bypass.load->getParent() == bypass.predecessor &&
                    bypass.phi->getBasicBlockIndex(bypass.predecessor) >= 0 &&
                    bypass.phi->getIncomingValueForBlock(bypass.predecessor) == bypass.load &&
                    !context->forwardedLoads.count(bypass.load) &&
                    !context->combinationalValues.count(bypass.load) &&
                    operation != context->schedule.instructionMap.end() &&
                    block != context->schedule.blocks.end() && !block->pipelined &&
                    memory && (memory->readLatency >= 1 || memory->fifoReadOnRequest) &&
                    (memory->banks == 1 || access->second.possibleBanks.size() == 1) &&
                    operation->second->latency == static_cast<int>(memory->readLatency + 1) &&
                    operation->second->startCycle + memory->readLatency + 1 == block->stateCount &&
                    readPhiEdges.insert({bypass.predecessor, bypass.phi}).second;
                for (const LoopPipelineInfo &pipeline : context->pipelineRegions)
                    for (const BasicBlock *edgeBlock : {bypass.predecessor, bypass.phi->getParent()})
                        valid &= pipeline.header != edgeBlock &&
                            std::find(pipeline.blocks.begin(), pipeline.blocks.end(), edgeBlock) ==
                                pipeline.blocks.end();
                valid &= std::count_if(context->binding.memoryAccesses.begin(),
                    context->binding.memoryAccesses.end(), [&](const MemoryAccessBinding &binding) {
                        return binding.instruction == bypass.load && binding.possibleBanks.size() == 1;
                    }) == 1;
                if (!valid)
                    context->addError("BACKEND_ORDINARY_READ_PHI_BYPASS_INVALID",
                        "A read-to-PHI bypass must sample a single-bank response on its original capture edge and retain the load register.",
                        instructionLocation(*bypass.load));
            }
            for (const MemoryObject &memory : context->memoryObjects)
            {
                if (memory.readLatency == 0 && !memory.fifoReadOnRequest &&
                    !memory.hasCombinationalScalarReads())
                    context->addError("BACKEND_ZERO_LATENCY_MEMORY_INVALID",
                        "Zero-latency reads require scalarized external banks or a validated request-edge FIFO.",
                        memory.name);
                if (!memory.fifoReadOnRequest)
                    continue;
                bool valid = memory.argument && context->configuration.rtlClockEnable &&
                    context->configuration.fifoArguments.count(memory.argument->getArgNo()) &&
                    context->configuration.fifoReadOnRequestArguments.count(memory.argument->getArgNo()) &&
                    memory.readLatency == 0 && memory.ports == 1 && memory.banks == 1 &&
                    memory.accessMode == MemoryAccessMode::ReadOnly && context->pipelineRegions.empty();
                for (const auto &entry : context->memoryAccesses)
                    if (entry.second.memoryBase == memory.base)
                    {
                        valid &= entry.second.isFIFO && !entry.second.isWrite;
                        if (context->forwardedLoads.count(entry.first))
                        {
                            LoopPipelineInfo region;
                            region.header = entry.first->getParent();
                            std::map<const Instruction *, int> starts;
                            for (const BlockSchedule &block : context->schedule.blocks)
                                if (block.block == region.header)
                                    for (const InstructionSchedule &operation : block.instructions)
                                        starts[operation.instruction] = operation.startCycle;
                            std::set<int> samples;
                            valid &= starts.count(entry.first) &&
                                pipelineSamplingCycles(*context, region, entry.first, starts, samples) &&
                                samples == std::set<int>{starts.at(entry.first)};
                        }
                    }
                if (!valid)
                    context->addError("BACKEND_FIFO_POP_CAPTURE_INVALID",
                        "Request-edge FIFO sampling requires an internal clock-enabled ordinary consumer and one accepting sample edge for forwarded heads.",
                        memory.name);
            }
            for (const auto &entry : context->memoryAccesses)
            {
                if (!entry.second.isFIFO)
                    continue;
                const auto count = std::count_if(
                    context->binding.memoryAccesses.begin(),
                    context->binding.memoryAccesses.end(),
                    [&](const MemoryAccessBinding &binding) {
                        return binding.instruction == entry.first;
                    });
                if (count != 1)
                    context->addError(
                        "BACKEND_FIFO_EVENT_BINDING_MISSING",
                        "Every FIFO event, including an unused read, requires exactly one scheduled port binding.",
                        instructionLocation(*entry.first));
            }
            for (const MemoryAccessBinding &binding :
                 context->binding.memoryAccesses)
            {
                // Recheck the exact request-edge proof after binding, including
                // cross-iteration collisions and conservative unknown addresses.
                const auto selected = context->schedule.instructionMap.find(binding.instruction);
                const auto current = context->memoryAccesses.find(binding.instruction);
                const MemoryObject *object = findMemoryObject(*context, binding.memory);
                if (selected != context->schedule.instructionMap.end() &&
                    current != context->memoryAccesses.end() && object &&
                    !(object->hasCombinationalScalarReads() && !current->second.isWrite))
                    for (const MemoryAccessBinding &prior : context->binding.memoryAccesses)
                    {
                        if (&prior == &binding)
                            break;
                        if (prior.port != binding.port || prior.memory != binding.memory ||
                            prior.instruction->getParent() != binding.instruction->getParent())
                            continue;
                        const auto previous = context->memoryAccesses.find(prior.instruction);
                        const auto start = context->schedule.instructionMap.find(prior.instruction);
                        if (previous == context->memoryAccesses.end() || start == context->schedule.instructionMap.end() ||
                            (object->hasCombinationalScalarReads() && !previous->second.isWrite))
                            continue;
                        unsigned interval = 0;
                        for (const LoopPipelineInfo &pipeline : context->pipelineRegions)
                            if (pipeline.header == binding.instruction->getParent())
                                interval = pipeline.achievedII;
                        if (memoryRequestsConflict(current->second, selected->second->startCycle,
                                previous->second, start->second->startCycle, interval))
                            context->addError("BACKEND_MEMORY_PORT_COLLISION",
                                "Bound requests can collide on one physical bank/port.", binding.operation);
                    }
                const MemoryObject *memory =
                    findMemoryObject(*context, binding.memory);
                const auto access =
                    context->memoryAccesses.find(binding.instruction);
                if (!memory || access == context->memoryAccesses.end() ||
                    binding.port >= memory->portUsage.size() ||
                    binding.possibleBanks.empty())
                {
                    context->addError(
                        "BACKEND_MEMORY_PORT_DIRECTION_METADATA_INVALID",
                        "Memory-port direction metadata is incomplete for access '" +
                            binding.operation + "'.",
                        binding.operation);
                    break;
                }
                bool bankMetadataValid = true;
                for (unsigned bank : binding.possibleBanks)
                {
                    if (bank >= memory->bankPortUsage.size() ||
                        binding.port >= memory->bankPortUsage[bank].size())
                    {
                        bankMetadataValid = false;
                        break;
                    }
                    const MemoryPortUsage &bankUsage =
                        memory->bankPortUsage[bank][binding.port];
                    bankMetadataValid &= access->second.isWrite
                                             ? bankUsage.hasWrite
                                             : bankUsage.hasRead;
                }
                if (!bankMetadataValid)
                {
                    context->addError(
                        "BACKEND_MEMORY_BANK_DIRECTION_METADATA_INVALID",
                        "Partition-bank direction metadata is incomplete for access '" +
                            binding.operation + "'.",
                        binding.operation);
                    break;
                }
                bool directWriteValid = true;
                const bool internalPIPO = memory->argument && context->configuration.rtlClockEnable &&
                    context->configuration.internalPIPOWriteArguments.count(memory->argument->getArgNo()) &&
                    !context->configuration.fifoArguments.count(memory->argument->getArgNo()) &&
                    !(memory->banks > 1 && memory->bankDepth == 1 && memory->banks == memory->depth);
                std::set<unsigned> directBanks;
                for (unsigned bank : binding.directWriteDataBanks)
                {
                    directWriteValid &= (memory->storage == MemoryStorageKind::LocalRAM || internalPIPO) &&
                        access->second.isWrite && directBanks.insert(bank).second &&
                        std::find(binding.possibleBanks.begin(), binding.possibleBanks.end(), bank) !=
                            binding.possibleBanks.end();
                    unsigned writers = 0;
                    for (const MemoryAccessBinding &other : context->binding.memoryAccesses)
                    {
                        const auto otherAccess = context->memoryAccesses.find(other.instruction);
                        if (other.memory == binding.memory && other.port == binding.port &&
                            otherAccess != context->memoryAccesses.end() && otherAccess->second.isWrite &&
                            std::find(other.possibleBanks.begin(), other.possibleBanks.end(), bank) !=
                                other.possibleBanks.end())
                            ++writers;
                    }
                    directWriteValid &= writers == 1;
                }
                if (!directWriteValid)
                {
                    context->addError("BACKEND_DIRECT_LOCAL_WRITE_BINDING_INVALID",
                        "Direct write data requires exactly one producer for each local or internal PIPO bank/port.",
                        binding.operation);
                    break;
                }
                const MemoryPortUsage &usage = memory->portUsage[binding.port];
                const bool directionMatches = access->second.isWrite
                                                  ? usage.hasWrite
                                                  : usage.hasRead;
                if (!directionMatches)
                {
                    context->addError(
                        "BACKEND_MEMORY_PORT_DIRECTION_MISMATCH",
                        "Memory access '" + binding.operation + "' is bound to " +
                            memoryAccessModeName(memoryAccessMode(usage)) +
                            " port " + std::to_string(binding.port) +
                            " of MemoryObject '" + binding.memory + "'.",
                        binding.operation);
                    break;
                }
            }
        }
        return false;
    }

    if (!context->top)
    {
        context->addError("BACKEND_TOP_NOT_FOUND",
                          "The configured top function was not found in the LLVM module.",
                          context->configuration.topFunction);
        return false;
    }

    context->requiredFeatures.sequentialControlFlow = true;
    context->requiredFeatures.integerArithmetic = true;
    context->requiredFeatures.loopPipeline =
        !context->configuration.pragmas.pipeline.empty();
    context->requiredFeatures.arrayPartition =
        context->configuration.pragmas.arrayPartitionCount != 0;
    context->requiredFeatures.dataflow =
        context->configuration.pragmas.dataflowCount != 0;
    context->requiredFeatures.streamInterface =
        context->configuration.pragmas.streamCount != 0;

    Function &top = *context->top;
    // Do not admit formats without a matching RTL implementation.
    for (const BasicBlock &block : top)
        for (const Instruction &instruction : block)
        {
            if (instruction.getType()->isFloatingPointTy() &&
                !isSupportedFloat(instruction.getType()))
                context->addError("BACKEND_FLOAT_FORMAT_UNSUPPORTED",
                    "Only IEEE binary16, binary32 and binary64 are implemented.");
        }
    if (top.getReturnType()->isFloatingPointTy() && !isSupportedFloat(top.getReturnType()))
        context->addError("BACKEND_FLOAT_FORMAT_UNSUPPORTED", "Unsupported floating-point return format.");
    for (const Argument &argument : top.args())
        if (argument.getType()->isFloatingPointTy() && !isSupportedFloat(argument.getType()))
            context->addError("BACKEND_FLOAT_FORMAT_UNSUPPORTED", "Unsupported floating-point argument format.");
    if (!top.getReturnType()->isVoidTy() && !top.getReturnType()->isIntegerTy())
    {
        if (top.getReturnType()->isFloatingPointTy())
            context->requiredFeatures.floatingPoint = true;
        else
            context->addError("BACKEND_UNSUPPORTED_RETURN_TYPE",
                              "Only void and integer scalar return types are supported.",
                              top.getName().str());
    }

    for (const Argument &argument : top.args())
    {
        if (argument.getType()->isPointerTy())
            context->requiredFeatures.memoryInterface = true;
        else if (argument.getType()->isFloatingPointTy())
            context->requiredFeatures.floatingPoint = true;
        else if (!argument.getType()->isIntegerTy())
            context->addError("BACKEND_UNSUPPORTED_ARGUMENT_TYPE",
                              "Only integer scalar arguments are supported.",
                              argument.getName().str());
    }

    for (BasicBlock &block : top)
    {
        if (!isa<BranchInst>(block.getTerminator()) &&
            !isa<ReturnInst>(block.getTerminator()))
        {
            context->addError("BACKEND_UNSUPPORTED_TERMINATOR",
                              "Only branch and return terminators are supported.",
                              block.getName().str());
        }

        for (Instruction &instruction : block)
        {
            if (instruction.getType()->isFloatingPointTy())
                context->requiredFeatures.floatingPoint = true;
            for (const Use &operandUse : instruction.operands())
            {
                if (operandUse->getType()->isFloatingPointTy())
                    context->requiredFeatures.floatingPoint = true;
            }

            const bool isMemoryInstruction =
                isBackendLoad(&instruction) || isBackendStore(&instruction) ||
                isa<LoadInst>(instruction) || isa<StoreInst>(instruction) ||
                isa<GetElementPtrInst>(instruction) ||
                isa<AllocaInst>(instruction) || isa<AtomicRMWInst>(instruction) ||
                isa<AtomicCmpXchgInst>(instruction) ||
                isa<PtrToIntInst>(instruction) || isa<IntToPtrInst>(instruction) ||
                isa<AddrSpaceCastInst>(instruction);
            if (isMemoryInstruction)
            {
                context->requiredFeatures.memoryInterface = true;
                // MemoryObjectAnalysis verifies the exact FIFO provenance of
                // every volatile candidate before scheduling or RTL emission.
                if (!isSupportedMemoryInstruction(
                        instruction, !context->configuration.fifoArguments.empty()))
                {
                    context->addError(
                        "BACKEND_UNSUPPORTED_MEMORY_OPERATION",
                        "Supported forms are static integer/IEEE scalar arrays, non-volatile scalar "
                        "loads/stores, verified FIFO events, integer address arithmetic, and one-lane "
                        "predicated memory. Atomics, wider vectors and defined masked-load passthrough "
                        "values are not supported.",
                        instructionLocation(instruction));
                }
                continue;
            }

            if (!isSupportedInstruction(instruction))
            {
                context->addError("BACKEND_UNSUPPORTED_INSTRUCTION",
                                  "Unsupported LLVM instruction '" +
                                      std::string(instruction.getOpcodeName()) + "'.",
                                  instructionLocation(instruction));
            }
            else if (hasUnsupportedConstantOperand(instruction))
            {
                context->addError("BACKEND_UNSUPPORTED_CONSTANT",
                                  "The instruction uses a constant expression that the "
                                  "current RTL backend cannot lower.",
                                  instructionLocation(instruction));
            }
        }
    }

    requireSupportedFeature(*context, context->requiredFeatures.loopPipeline,
                            context->capabilities.loopPipeline,
                            "BACKEND_UNSUPPORTED_PIPELINE", "Loop pipelining");
    if (context->requiredFeatures.floatingPoint)
        context->addWarning("BACKEND_FLOAT_TIMING_UNCHARACTERIZED",
            "FP cycle counts follow the RTL implementation; target clock timing and resource estimates require synthesis validation.");
    requireSupportedFeature(*context, context->requiredFeatures.arrayPartition,
                            context->capabilities.arrayPartition,
                            "BACKEND_UNSUPPORTED_ARRAY_PARTITION", "Array partitioning");
    requireSupportedFeature(*context, context->requiredFeatures.dataflow,
                            context->capabilities.dataflow,
                            "BACKEND_UNSUPPORTED_DATAFLOW", "DATAFLOW scheduling");
    requireSupportedFeature(*context, context->requiredFeatures.floatingPoint,
                            context->capabilities.floatingPoint,
                            "BACKEND_UNSUPPORTED_FLOATING_POINT", "Floating-point operations");
    requireSupportedFeature(*context, context->requiredFeatures.memoryInterface,
                            context->capabilities.memoryInterface,
                            "BACKEND_UNSUPPORTED_MEMORY_INTERFACE",
                            "Pointer, array, or memory interfaces");
    requireSupportedFeature(*context, context->requiredFeatures.streamInterface,
                            context->capabilities.streamInterface,
                            "BACKEND_UNSUPPORTED_STREAM_INTERFACE", "Stream interfaces");
    return false;
}

void HI_BackendValidation::getAnalysisUsage(AnalysisUsage &usage) const
{
    usage.setPreservesAll();
}
