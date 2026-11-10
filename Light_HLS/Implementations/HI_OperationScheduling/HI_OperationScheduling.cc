#include "HI_OperationScheduling.h"
#include "HI_PipelineValueLifetime.h"

#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Instructions.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;
using namespace light_hls::backend;

namespace
{

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

bool isDatapathInstruction(const Instruction &instruction)
{
    return !isa<PHINode>(instruction) && !isa<AllocaInst>(instruction) &&
           !instruction.isTerminator() &&
           !isIgnoredIntrinsic(instruction);
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

unsigned getResultWidth(const Instruction &instruction)
{
    if (const auto *integerType = dyn_cast<IntegerType>(instruction.getType()))
        return integerType->getBitWidth();
    return 0;
}

void discoverHardwareConstants(HLSBackendContext &context)
{
    context.constantValues.clear();
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (BasicBlock &block : *context.top)
            for (Instruction &instruction : block)
            {
                if (!instruction.getType()->isIntegerTy() || isa<PHINode>(instruction) ||
                    instruction.mayHaveSideEffects() || isa<LoadInst>(instruction) ||
                    context.constantValues.count(&instruction))
                    continue;
                ConstantInt *constant = nullptr;
                if (const auto *pointerCast = dyn_cast<PtrToIntInst>(&instruction))
                {
                    const auto provenance = context.addressProvenance.find(pointerCast);
                    // Memory interfaces use object-relative element offsets,
                    // not host addresses. Only a proven root has offset zero.
                    if (provenance != context.addressProvenance.end())
                        for (const MemoryObject &memory : context.memoryObjects)
                            if (memory.base == provenance->second &&
                                pointerCast->getPointerOperand()->stripPointerCasts() == memory.base)
                                constant = ConstantInt::get(cast<IntegerType>(instruction.getType()), 0);
                }
                else
                {
                    SmallVector<Constant *, 4> operands;
                    for (const Use &operand : instruction.operands())
                    {
                        Constant *known = dyn_cast<ConstantInt>(operand.get());
                        const auto folded = context.constantValues.find(operand.get());
                        if (!known && folded != context.constantValues.end())
                            known = const_cast<ConstantInt *>(folded->second);
                        if (!known)
                            break;
                        operands.push_back(known);
                    }
                    if (!operands.empty() && operands.size() == instruction.getNumOperands())
                        constant = dyn_cast_or_null<ConstantInt>(ConstantFoldInstOperands(
                            &instruction, operands, context.module->getDataLayout()));
                }
                if (constant)
                {
                    context.constantValues[&instruction] = constant;
                    changed = true;
                }
            }
    }
}

int getLatency(const HLSBackendContext &context, const Instruction &instruction)
{
    if (context.combinationalValues.count(&instruction) ||
        isa<GetElementPtrInst>(instruction) || isa<IntToPtrInst>(instruction) ||
        isa<AddrSpaceCastInst>(instruction))
        return 0;
    if (isa<LoadInst>(instruction))
    {
        auto access = context.memoryAccesses.find(&instruction);
        if (access != context.memoryAccesses.end())
        {
            const MemoryObject *memory =
                findMemoryObject(context, access->second.memory);
            if (memory)
            {
                bool forwarded = context.forwardedLoads.count(&instruction) != 0;
                for (const LoopPipelineInfo &pipeline : context.pipelineRegions)
                    forwarded |= pipeline.forwardedLoads.count(&instruction) != 0;
                return static_cast<int>(memory->readLatency + (forwarded ? 0 : 1));
            }
        }
        return 2;
    }
    if (isa<StoreInst>(instruction))
        return 1;
    if (isa<ICmpInst>(instruction) || isa<SelectInst>(instruction) ||
        isa<CastInst>(instruction) || isa<FreezeInst>(instruction))
    {
        return 1;
    }
    auto model = context.latencyModel.find(&instruction);
    if (model == context.latencyModel.end())
        return 1;
    return std::max(1, std::min(model->second, 64));
}

bool canForwardMemoryResponse(const HLSBackendContext &context,
                               const Instruction &instruction,
                               bool pipelineAddressLifetime = false)
{
    if (!isa<LoadInst>(instruction))
        return false;
    const auto access = context.memoryAccesses.find(&instruction);
    if (access == context.memoryAccesses.end())
        return false;
    const MemoryObject *memory = findMemoryObject(context, access->second.memory);
    if (!memory)
        return false;
    // Internal acyclic FWFT consumers may sample the head in their accepting
    // operation. The wrapper gates pop and every simultaneous effect together.
    const bool acceptingFIFOHead = memory->fifoReadOnRequest &&
        context.configuration.rtlClockEnable && context.pipelineRegions.empty();
    return acceptingFIFOHead || memory->hasCombinationalScalarReads() || (memory->readLatency == 1 &&
        (memory->banks == 1 || access->second.possibleBanks.size() == 1 || pipelineAddressLifetime));
}

const PHINode *ordinaryFeedbackPhi(const HLSBackendContext &context,
                                   const Instruction &instruction,
                                   const LoopInfo &loops)
{
    const auto *update = dyn_cast<BinaryOperator>(&instruction);
    if (!update || (update->getOpcode() != Instruction::Add &&
                    update->getOpcode() != Instruction::Sub))
        return nullptr;
    const auto *phi = dyn_cast<PHINode>(update->getOperand(0));
    const auto *step = dyn_cast<ConstantInt>(update->getOperand(1));
    if (!phi && update->getOpcode() == Instruction::Add)
    {
        phi = dyn_cast<PHINode>(update->getOperand(1));
        step = dyn_cast<ConstantInt>(update->getOperand(0));
    }
    const BasicBlock *block = instruction.getParent();
    if (!phi || !step ||
        phi->getNumIncomingValues() != 2 ||
        phi->getBasicBlockIndex(block) < 0 ||
        phi->getIncomingValueForBlock(block) != update)
        return nullptr;
    const Loop *loop = loops.getLoopFor(block);
    const auto *branch = dyn_cast<BranchInst>(block->getTerminator());
    if (!loop || loop->getHeader() != phi->getParent() ||
        loop->getLoopLatch() != block || !branch || !branch->isConditional() ||
        ((branch->getSuccessor(0) == phi->getParent()) ==
         (branch->getSuccessor(1) == phi->getParent())))
        return nullptr;
    for (const LoopPipelineInfo &pipeline : context.pipelineRegions)
        if (pipeline.header == block || pipeline.header == phi->getParent() ||
            std::find(pipeline.blocks.begin(), pipeline.blocks.end(), block) !=
                pipeline.blocks.end() ||
            std::find(pipeline.blocks.begin(), pipeline.blocks.end(), phi->getParent()) !=
                pipeline.blocks.end())
            return nullptr;
    // The header PHI remains held throughout the loop body, including nested
    // loops and memory requests. Only the unique latch samples its next value.
    return phi;
}

bool ordinaryExitExpression(const Instruction &instruction, const LoopInfo &loops,
                            const HLSBackendContext &context, std::set<const PHINode *> &roots)
{
    const BasicBlock *block = instruction.getParent();
    const Loop *loop = loops.getLoopFor(block);
    const auto *branch = dyn_cast<BranchInst>(block->getTerminator());
    if (!loop || loop->getHeader() != block || loop->getNumBlocks() != 1 ||
        !branch || !branch->isConditional() ||
        ((branch->getSuccessor(0) == block) == (branch->getSuccessor(1) == block)))
        return false;
    const BasicBlock *exit = branch->getSuccessor(branch->getSuccessor(0) == block ? 1 : 0);
    if (!isa<ReturnInst>(exit->getTerminator()) ||
        std::any_of(context.pipelineRegions.begin(), context.pipelineRegions.end(),
            [&](const LoopPipelineInfo &pipeline) {
                return pipeline.header == block ||
                    std::find(pipeline.blocks.begin(), pipeline.blocks.end(), block) != pipeline.blocks.end();
            }))
        return false;
    bool escapes = false;
    for (const User *user : instruction.users())
    {
        const auto *consumer = dyn_cast<Instruction>(user);
        if (!consumer || isa<PHINode>(consumer))
            return false;
        if (isa<DbgInfoIntrinsic>(consumer) || consumer->getParent() == block)
            continue;
        if (consumer->getParent() != exit)
            return false;
        escapes = true;
    }
    if (!escapes)
        return false;
    std::set<const Value *> visited;
    std::function<bool(const Value *)> stable = [&](const Value *value) {
        if (!visited.insert(value).second)
            return true;
        if (isa<ConstantInt>(value) || isa<Argument>(value) || context.constantValues.count(value))
            return value->getType()->isIntegerTy();
        const auto *producer = dyn_cast<Instruction>(value);
        if (!producer || producer->getParent() != block || !producer->getType()->isIntegerTy())
            return false;
        if (const auto *phi = dyn_cast<PHINode>(producer))
        {
            if (phi->getNumIncomingValues() != 2 || phi->getBasicBlockIndex(block) < 0)
                return false;
            roots.insert(phi);
            return true;
        }
        if (!(isa<BinaryOperator>(producer) || isa<CastInst>(producer) ||
              isa<ICmpInst>(producer) || isa<SelectInst>(producer) || isa<FreezeInst>(producer)))
            return false;
        return std::all_of(producer->op_begin(), producer->op_end(),
            [&](const Use &operand) { return stable(operand.get()); });
    };
    return stable(&instruction);
}

double backedgeExpressionDelay(const HLSBackendContext &context,
                               const Instruction &instruction,
                               std::map<const Instruction *, double> &cache);

void selectCombinationalValues(HLSBackendContext &context, const LoopInfo &loops)
{
    context.combinationalValues.clear();
    context.ordinaryFeedbackValues.clear();
    context.ordinaryLiveOutExpressionRoots.clear();
    std::vector<std::pair<const Instruction *, std::set<const PHINode *>>> exitExpressions;
    const double budget = context.configuration.clockPeriodNs * 0.7;
    std::map<const Instruction *, double> pathDelay;
    for (BasicBlock &block : *context.top)
    {
        for (Instruction &instruction : block)
        {
            const bool wiring = isa<CastInst>(instruction) ||
                                isa<FreezeInst>(instruction);
            const bool arithmetic = isa<BinaryOperator>(instruction) ||
                                    isa<ICmpInst>(instruction) ||
                                    isa<SelectInst>(instruction);
            if (!instruction.getType()->isIntegerTy() || (!wiring && !arithmetic))
                continue;
            const PHINode *feedback = ordinaryFeedbackPhi(context, instruction, loops);
            std::set<const PHINode *> heldRoots;
            const bool exitExpression = ordinaryExitExpression(instruction, loops, context, heldRoots);
            const auto model = context.delayModel.find(&instruction);
            double delay = wiring ? 0.0 : budget;
            if (!wiring && model != context.delayModel.end() &&
                std::isfinite(model->second) && model->second >= 0)
                delay = model->second;
            // Reserve routing and control slack; unknown operators retain a register.
            const bool known = wiring || (model != context.delayModel.end() &&
                std::isfinite(model->second) && model->second >= 0);
            double inputDelay = 0;
            for (const Use &operand : instruction.operands())
                if (const auto *producer = dyn_cast<Instruction>(operand.get()))
                    inputDelay = std::max(inputDelay, pathDelay[producer]);
            if (inputDelay + delay > budget)
            {
                for (const Use &operand : instruction.operands())
                    if (const auto *producer = dyn_cast<Instruction>(operand.get()))
                    {
                        context.combinationalValues.erase(producer);
                        pathDelay[producer] = 0;
                    }
                inputDelay = 0;
            }
            const bool localUsers = std::all_of(
                instruction.user_begin(), instruction.user_end(),
                [&](const User *user) {
                    const auto *consumer = dyn_cast<Instruction>(user);
                    return consumer && (isa<DbgInfoIntrinsic>(consumer) || consumer == feedback ||
                        (consumer->getParent() == &block &&
                         !isa<PHINode>(consumer)));
                });
            const auto latency = context.latencyModel.find(&instruction);
            if (known && (localUsers || exitExpression) && inputDelay + delay <= budget &&
                (wiring || (latency != context.latencyModel.end() &&
                            latency->second == 0)))
            {
                context.combinationalValues.insert(&instruction);
                if (feedback)
                    context.ordinaryFeedbackValues[&instruction] = feedback;
                pathDelay[&instruction] = inputDelay + delay;
                if (exitExpression)
                    exitExpressions.emplace_back(&instruction, std::move(heldRoots));
            }
        }
    }
    // IR block order need not be dominance order. Recheck the entire exit
    // expression after selection, including the newly exposed live-out paths.
    for (const auto &entry : exitExpressions)
    {
        if (!context.combinationalValues.count(entry.first))
            continue;
        std::map<const Instruction *, double> cache;
        std::set<const Instruction *> visited;
        std::vector<const Instruction *> pending{entry.first};
        bool withinBudget = true;
        while (!pending.empty())
        {
            const Instruction *value = pending.back();
            pending.pop_back();
            if (!visited.insert(value).second)
                continue;
            withinBudget &= backedgeExpressionDelay(context, *value, cache) <= budget;
            if (context.combinationalValues.count(value))
                for (const User *user : value->users())
                    if (const auto *consumer = dyn_cast<Instruction>(user))
                        if (!isa<DbgInfoIntrinsic>(consumer) && !isa<PHINode>(consumer) &&
                            !consumer->isTerminator())
                            pending.push_back(consumer);
        }
        if (!withinBudget)
            context.combinationalValues.erase(entry.first);
        else
            context.ordinaryLiveOutExpressionRoots.insert(entry.second.begin(), entry.second.end());
    }
    // A later consumer may have inserted a timing cut at a candidate update.
    for (auto entry = context.ordinaryFeedbackValues.begin();
         entry != context.ordinaryFeedbackValues.end();)
        if (!context.combinationalValues.count(entry->first))
            entry = context.ordinaryFeedbackValues.erase(entry);
        else
            ++entry;
}

double backedgeExpressionDelay(const HLSBackendContext &context,
                               const Instruction &instruction,
                               std::map<const Instruction *, double> &cache)
{
    const auto cached = cache.find(&instruction);
    if (cached != cache.end())
        return cached->second;
    const bool wiring = (isa<CastInst>(instruction) &&
                         instruction.getOperand(0)->getType()->isIntegerTy()) ||
                        isa<FreezeInst>(instruction);
    if (!instruction.getType()->isIntegerTy() || instruction.mayReadOrWriteMemory() ||
        instruction.mayHaveSideEffects() ||
        !(wiring || isa<BinaryOperator>(instruction) || isa<ICmpInst>(instruction) ||
          isa<SelectInst>(instruction)))
        return INFINITY;
    const auto delay = context.delayModel.find(&instruction);
    const auto latency = context.latencyModel.find(&instruction);
    if (!wiring && (delay == context.delayModel.end() ||
        !std::isfinite(delay->second) || delay->second < 0 ||
        latency == context.latencyModel.end() || latency->second != 0))
        return INFINITY;
    double inputDelay = 0;
    for (const Use &operand : instruction.operands())
        if (const auto *producer = dyn_cast<Instruction>(operand.get()))
            if (context.combinationalValues.count(producer))
                inputDelay = std::max(inputDelay, backedgeExpressionDelay(context, *producer, cache));
    return cache[&instruction] = inputDelay + (wiring ? 0.0 : delay->second);
}

std::string uniqueName(const std::string &base, std::set<std::string> &used)
{
    std::string result = base;
    unsigned suffix = 0;
    while (!used.insert(result).second)
        result = base + "_" + std::to_string(++suffix);
    return result;
}

std::vector<const BasicBlock *> hardwareSuccessors(
    const BasicBlock *block,
    const std::map<const BasicBlock *, const BasicBlock *> &pipelineExits)
{
    const auto pipeline = pipelineExits.find(block);
    if (pipeline != pipelineExits.end())
        return {pipeline->second};
    return {succ_begin(block), succ_end(block)};
}

bool detectControlFlowCycle(
    const Function &function,
    const std::map<const BasicBlock *, const BlockSchedule *> &blockSchedules,
    const std::map<const BasicBlock *, const BasicBlock *> &pipelineExits)
{
    std::map<const BasicBlock *, unsigned> color;
    std::function<bool(const BasicBlock *)> visit = [&](const BasicBlock *block) {
        color[block] = 1;
        for (const BasicBlock *successor : hardwareSuccessors(block, pipelineExits))
        {
            if (!blockSchedules.count(successor))
                continue;
            if (color[successor] == 1)
                return true;
            if (color[successor] == 0 && visit(successor))
                return true;
        }
        color[block] = 2;
        return false;
    };
    return visit(&function.getEntryBlock());
}

long long getAcyclicLatency(
    const BasicBlock *block,
    const std::map<const BasicBlock *, const BlockSchedule *> &blockSchedules,
    const std::map<const BasicBlock *, const BasicBlock *> &pipelineExits,
    std::map<const BasicBlock *, long long> &memo)
{
    auto cached = memo.find(block);
    if (cached != memo.end())
        return cached->second;
    const BlockSchedule *schedule = blockSchedules.at(block);
    long long successorLatency = 0;
    for (const BasicBlock *successor : hardwareSuccessors(block, pipelineExits))
    {
        if (blockSchedules.count(successor))
        {
            successorLatency =
                std::max(successorLatency,
                         getAcyclicLatency(successor, blockSchedules,
                                           pipelineExits, memo));
        }
    }
    const long long blockLatency = schedule->executionLatency > 0
                                       ? schedule->executionLatency
                                       : schedule->stateCount;
    const long long latency = blockLatency + successorLatency;
    memo[block] = latency;
    return latency;
}

LoopPipelineInfo *findPipeline(HLSBackendContext &context,
                               const BasicBlock *block)
{
    for (LoopPipelineInfo &pipeline : context.pipelineRegions)
    {
        if (pipeline.header == block)
            return &pipeline;
    }
    return nullptr;
}

bool isTerminalPipelineExit(const HLSBackendContext &context,
                            const BasicBlock &block)
{
    const bool isPipelineExit = std::any_of(
        context.pipelineRegions.begin(), context.pipelineRegions.end(),
        [&](const LoopPipelineInfo &pipeline) {
            return pipeline.exit == &block;
        });
    if (!isPipelineExit || !isa<ReturnInst>(block.getTerminator()))
        return false;
    return std::all_of(block.begin(), block.end(),
                       [](const Instruction &instruction) {
                           return isa<PHINode>(instruction) ||
                                  isa<ReturnInst>(instruction) ||
                                  isIgnoredIntrinsic(instruction);
                       });
}

int positiveCeilingDivide(int numerator, unsigned denominator)
{
    if (numerator <= 0 || denominator == 0)
        return 1;
    return std::max(
        1, (numerator + static_cast<int>(denominator) - 1) /
               static_cast<int>(denominator));
}

const ScalarRecurrenceDependence *findScalarRecurrence(
    const LoopPipelineInfo &pipeline, const PHINode *phi)
{
    for (const ScalarRecurrenceDependence &dependence :
         pipeline.scalarRecurrences)
    {
        if (dependence.phi == phi)
            return &dependence;
    }
    return nullptr;
}

void discoverPipelineHardwareValues(const HLSBackendContext &context,
                                    BasicBlock &block,
                                    LoopPipelineInfo &pipeline,
                                    std::set<Instruction *> &liveOperations)
{
    pipeline.contextValues.clear();
    pipeline.derivedValues.clear();
    pipeline.liveOutValues.clear();
    std::vector<const Value *> worklist;

    for (Instruction &instruction : block)
    {
        if (context.constantValues.count(&instruction))
            continue;
        const auto event = context.memoryAccesses.find(&instruction);
        if (isa<StoreInst>(instruction) ||
            (event != context.memoryAccesses.end() && event->second.isFIFO))
            worklist.push_back(&instruction);

        if (!instruction.getType()->isIntegerTy())
            continue;
        for (const User *user : instruction.users())
        {
            const auto *userInstruction = dyn_cast<Instruction>(user);
            if (!userInstruction || userInstruction->getParent() == &block ||
                isa<DbgInfoIntrinsic>(userInstruction))
            {
                continue;
            }
            if (isa<PHINode>(userInstruction) &&
                userInstruction->getParent() == pipeline.exit &&
                std::none_of(context.pipelineRegions.begin(), context.pipelineRegions.end(),
                    [&](const LoopPipelineInfo &successor) { return successor.header == pipeline.exit; }))
            {
                worklist.push_back(&instruction);
                continue;
            }
            if (isa<ReturnInst>(userInstruction) &&
                userInstruction->getParent() == pipeline.exit &&
                isTerminalPipelineExit(context, *pipeline.exit))
            {
                worklist.push_back(&instruction);
                continue;
            }
            pipeline.liveOutValues.insert(&instruction);
            worklist.push_back(&instruction);
        }
    }

    if (pipeline.exit)
    {
        for (const Instruction &instruction : *pipeline.exit)
        {
            const auto *phi = dyn_cast<PHINode>(&instruction);
            if (!phi)
                break;
            const Value *incoming = phi->getIncomingValueForBlock(&block);
            if (incoming)
                worklist.push_back(incoming);
        }
    }

    std::set<const Value *> visited;
    while (!worklist.empty())
    {
        const Value *value = worklist.back();
        worklist.pop_back();
        if (!value || !visited.insert(value).second)
            continue;

        const auto *instruction = dyn_cast<Instruction>(value);
        if (!instruction || instruction->getParent() != &block)
            continue;

        if (const auto *phi = dyn_cast<PHINode>(instruction))
        {
            const auto phiInfo = std::find_if(
                pipeline.phis.begin(), pipeline.phis.end(),
                [&](const PipelinePhiInfo &candidate) {
                    return candidate.phi == phi;
                });
            if (phiInfo != pipeline.phis.end() && phiInfo->affine)
            {
                pipeline.derivedValues.insert(phi);
                continue;
            }
            if (phi->getType()->isIntegerTy())
                pipeline.contextValues.insert(phi);
            const ScalarRecurrenceDependence *dependence =
                findScalarRecurrence(pipeline, phi);
            if (dependence && dependence->exact && dependence->source)
                worklist.push_back(dependence->source);
            else if (phiInfo != pipeline.phis.end() &&
                     phiInfo->recurrenceValue)
                worklist.push_back(phiInfo->recurrenceValue);
            continue;
        }

        if (isIgnoredIntrinsic(*instruction) || instruction->isTerminator() ||
            context.constantValues.count(instruction))
            continue;
        liveOperations.insert(const_cast<Instruction *>(instruction));
        if (instruction->getType()->isIntegerTy() &&
            !context.combinationalValues.count(instruction))
            pipeline.contextValues.insert(instruction);
        for (const Use &operandUse : instruction->operands())
            worklist.push_back(operandUse.get());
    }
}

bool schedulePipelineBlock(HLSBackendContext &context, BasicBlock &block,
                           LoopPipelineInfo &pipeline,
                           BlockSchedule &blockSchedule,
                           unsigned &valueIndex)
{
    std::vector<Instruction *> operations;
    std::set<Instruction *> liveOperations;
    std::map<std::pair<std::string, unsigned>, unsigned> accessCount;
    std::map<std::string, unsigned> requestsPerIteration;
    std::set<std::string> conservativeMemories;
    if (!pipeline.dependenceAnalysisValid)
    {
        context.addError(
            "BACKEND_DEPENDENCE_ANALYSIS_MISSING",
            "Pipeline scheduling requires backend dependence analysis.",
            pipeline.label);
        return false;
    }
    discoverPipelineHardwareValues(context, block, pipeline, liveOperations);
    for (Instruction &instruction : block)
    {
        if (!isDatapathInstruction(instruction) ||
            !liveOperations.count(&instruction))
            continue;
        operations.push_back(&instruction);
        const auto access = context.memoryAccesses.find(&instruction);
        if (access == context.memoryAccesses.end())
            continue;
        const MemoryObject *memory =
            findMemoryObject(context, access->second.memory);
        if (!memory)
            continue;
        if (memory->hasCombinationalScalarReads() && !access->second.isWrite)
            continue;
        ++requestsPerIteration[memory->name];
        if (access->second.iterationBanks.empty())
            conservativeMemories.insert(memory->name);
        for (unsigned bank : accessBanks(*memory, access->second))
            ++accessCount[{access->second.memory, bank}];
    }

    pipeline.memoryMII = 1;
    for (const auto &entry : accessCount)
    {
        const MemoryObject *memory =
            findMemoryObject(context, entry.first.first);
        const unsigned ports = memory ? memory->ports : 1;
        if (!conservativeMemories.count(entry.first.first))
        {
            const unsigned capacity = ports * memory->banks;
            pipeline.memoryMII = std::max(pipeline.memoryMII, static_cast<int>(
                (requestsPerIteration.at(memory->name) + capacity - 1) / capacity));
            continue;
        }
        const int lowerBound = static_cast<int>(
            (entry.second + ports - 1) / ports);
        pipeline.memoryMII = std::max(pipeline.memoryMII, lowerBound);
    }

    int candidateII = std::max(
        1, std::max(pipeline.requestedII, pipeline.memoryMII));
    pipeline.forwardedLoads.clear();
    for (Instruction *instruction : operations)
    {
        // The pipeline lifetime analysis retains a dynamic bank address until
        // its response edge, including registered address expressions.
        if (canForwardMemoryResponse(context, *instruction, true))
            pipeline.forwardedLoads.insert(instruction);
    }
    std::map<const Instruction *, int> selectedStart;
    for (unsigned attempt = 0; attempt < 4096 + operations.size(); ++attempt)
    {
        std::map<const Instruction *, int> completionCycle;
        struct Reservation { const MemoryAccessInfo *access; int start; unsigned port; };
        std::vector<Reservation> reservations;
        std::map<const Instruction *, int> startCycles;
        int maximumCompletion = 0;
        bool retryPorts = false;

        for (Instruction *instruction : operations)
        {
            int startCycle = 0;
            for (const Use &operandUse : instruction->operands())
            {
                const auto *operandInstruction =
                    dyn_cast<Instruction>(operandUse.get());
                if (!operandInstruction || operandInstruction->getParent() != &block)
                    continue;
                const auto completion = completionCycle.find(operandInstruction);
                if (completion != completionCycle.end())
                    startCycle = std::max(startCycle, completion->second);
            }

            for (const MemoryDependenceInfo &dependence :
                 pipeline.memoryDependences)
            {
                if (!dependence.exists || dependence.distance != 0 ||
                    dependence.sink != instruction)
                {
                    continue;
                }
                const auto sourceStart = startCycles.find(dependence.source);
                if (sourceStart != startCycles.end())
                {
                    startCycle = std::max(
                        startCycle,
                        sourceStart->second + dependence.sourceEventOffset +
                            static_cast<int>(dependence.minimumSeparation) -
                            dependence.sinkEventOffset);
                }
            }

            const auto memoryAccess = context.memoryAccesses.find(instruction);
            if (memoryAccess != context.memoryAccesses.end())
            {
                const MemoryObject *memory = findMemoryObject(
                    context, memoryAccess->second.memory);
                const unsigned ports = memory ? memory->ports : 1;
                const bool scalarRead = memory && memory->hasCombinationalScalarReads() &&
                                        !memoryAccess->second.isWrite;
                bool assigned = scalarRead;
                const int searchEnd = startCycle + std::max(64, 2 * candidateII);
                for (; !assigned && startCycle < searchEnd; ++startCycle)
                {
                    for (unsigned port = 0; port < ports; ++port)
                    {
                        const bool conflict = std::any_of(reservations.begin(), reservations.end(),
                            [&](const Reservation &prior) {
                                return prior.port == port && memoryRequestsConflict(
                                    memoryAccess->second, startCycle, *prior.access, prior.start, candidateII);
                            });
                        if (!conflict)
                        {
                            reservations.push_back({&memoryAccess->second, startCycle, port});
                            assigned = true;
                            break;
                        }
                    }
                    if (assigned)
                        break;
                }
                if (!assigned)
                {
                    retryPorts = true;
                    break;
                }
            }

            const int latency = getLatency(context, *instruction);
            startCycles[instruction] = startCycle;
            completionCycle[instruction] = startCycle + latency;
            maximumCompletion =
                std::max(maximumCompletion, startCycle + latency);
        }

        if (retryPorts)
        {
            pipeline.memoryMII = ++candidateII;
            continue;
        }
        bool retryCapture = false;
        for (auto it = pipeline.forwardedLoads.begin();
             it != pipeline.forwardedLoads.end();)
        {
            std::set<int> samplingCycles;
            const int responseCycle = completionCycle.at(*it);
            if (!pipelineSamplingCycles(context, pipeline, *it, startCycles,
                                        samplingCycles) ||
                samplingCycles != std::set<int>{responseCycle})
            {
                it = pipeline.forwardedLoads.erase(it);
                retryCapture = true;
            }
            else
                ++it;
        }
        if (retryCapture)
            continue;

        int requiredII = candidateII;
        pipeline.recurrenceMII = 1;
        for (const ScalarRecurrenceDependence &dependence :
             pipeline.scalarRecurrences)
        {
            if (!dependence.exact || !dependence.source ||
                dependence.sinks.empty())
            {
                const auto completion =
                    completionCycle.find(dependence.source);
                if (completion != completionCycle.end())
                {
                    const int fallbackII = completion->second + 1;
                    pipeline.recurrenceMII =
                        std::max(pipeline.recurrenceMII, fallbackII);
                    requiredII = std::max(requiredII, fallbackII);
                }
                continue;
            }
            const auto completion = completionCycle.find(dependence.source);
            if (completion == completionCycle.end())
                continue;
            for (const Instruction *sink : dependence.sinks)
            {
                const auto start = startCycles.find(sink);
                if (start == startCycles.end())
                    continue;
                const int recurrenceII = positiveCeilingDivide(
                    completion->second - start->second,
                    dependence.distance);
                pipeline.recurrenceMII =
                    std::max(pipeline.recurrenceMII, recurrenceII);
                requiredII = std::max(requiredII, recurrenceII);
            }
        }

        pipeline.conservativeMemoryDependence = false;
        pipeline.memoryDependenceMII = 1;
        for (const MemoryDependenceInfo &dependence :
             pipeline.memoryDependences)
        {
            if (!dependence.exists)
                continue;
            if (!dependence.exact)
                pipeline.conservativeMemoryDependence = true;
            if (dependence.distance == 0)
                continue;
            const auto sourceStart = startCycles.find(dependence.source);
            const auto sinkStart = startCycles.find(dependence.sink);
            if (sourceStart == startCycles.end() ||
                sinkStart == startCycles.end())
            {
                pipeline.conservativeMemoryDependence = true;
                requiredII = std::max(requiredII, maximumCompletion + 1);
                continue;
            }
            const int dependenceII = positiveCeilingDivide(
                sourceStart->second + dependence.sourceEventOffset +
                    static_cast<int>(dependence.minimumSeparation) -
                    sinkStart->second - dependence.sinkEventOffset,
                dependence.distance);
            pipeline.memoryDependenceMII =
                std::max(pipeline.memoryDependenceMII, dependenceII);
            requiredII = std::max(requiredII, dependenceII);
        }

        if (requiredII > candidateII)
        {
            candidateII = requiredII;
            continue;
        }
        selectedStart = std::move(startCycles);
        break;
    }

    if (selectedStart.size() != operations.size())
    {
        context.addError(
            "BACKEND_PIPELINE_SCHEDULING_FAILED",
            "Modulo scheduling did not find a legal initiation interval.",
            pipeline.label);
        return false;
    }

    pipeline.achievedII = candidateII;
    for (const Instruction *load : pipeline.forwardedLoads)
        pipeline.contextValues.erase(load);
    pipeline.resourceMII = 1;
    const auto *terminalReturn = pipeline.exit ? dyn_cast<ReturnInst>(pipeline.exit->getTerminator()) : nullptr;
    const Value *returnValue = terminalReturn ? terminalReturn->getReturnValue() : nullptr;
    const auto *returnPhi = dyn_cast_or_null<PHINode>(returnValue);
    if (returnPhi && returnPhi->getParent() == pipeline.exit && returnPhi->hasOneUse())
        returnValue = returnPhi->getIncomingValueForBlock(pipeline.header);
    const auto *returnSource = dyn_cast_or_null<Instruction>(returnValue);
    pipeline.forwardedReturnValue = nullptr;
    if (returnSource && selectedStart.count(returnSource) &&
        (isa<BinaryOperator>(returnSource) || isa<ICmpInst>(returnSource) ||
         isa<SelectInst>(returnSource) || isa<CastInst>(returnSource)) &&
        getLatency(context, *returnSource) <= 1 &&
        std::all_of(pipeline.exit->begin(), pipeline.exit->end(), [](const Instruction &instruction) {
            return isa<PHINode>(instruction) || isa<DbgInfoIntrinsic>(instruction) || isa<ReturnInst>(instruction);
        }) &&
        std::all_of(returnSource->user_begin(), returnSource->user_end(), [&](const User *user) {
            if (user == terminalReturn || user == returnPhi || isa<DbgInfoIntrinsic>(user))
                return true;
            return std::any_of(pipeline.scalarRecurrences.begin(), pipeline.scalarRecurrences.end(),
                [&](const ScalarRecurrenceDependence &dependence) {
                    return dependence.exact && dependence.source == returnSource && dependence.phi == user;
                });
        }))
        pipeline.forwardedReturnValue = returnSource;
    // A zero-latency scalar-bank path can commit its last effect in stage zero.
    int retirementCycle = 0;
    for (Instruction *instruction : operations)
    {
        int writeCycle = selectedStart.at(instruction);
        if (isa<LoadInst>(instruction) && !pipeline.forwardedLoads.count(instruction))
            writeCycle += getLatency(context, *instruction) - 1;
        retirementCycle = std::max(retirementCycle, writeCycle);
        // A final side effect may coincide with done. An exported registered
        // value must instead be available before the retirement sampling edge.
        for (const User *user : instruction->users())
            if (const auto *consumer = dyn_cast<Instruction>(user);
                consumer && consumer->getParent() != &block && !isa<DbgInfoIntrinsic>(consumer))
                if (instruction != pipeline.forwardedReturnValue ||
                    (consumer != terminalReturn && consumer != returnPhi))
                    retirementCycle = std::max(retirementCycle,
                        selectedStart.at(instruction) + getLatency(context, *instruction));
    }
    if (pipeline.forwardedReturnValue && selectedStart.at(pipeline.forwardedReturnValue) != retirementCycle)
        pipeline.forwardedReturnValue = nullptr;
    pipeline.pipelineLatency = static_cast<unsigned>(retirementCycle);
    pipeline.stageCount = static_cast<unsigned>(
        (pipeline.pipelineLatency + candidateII - 1) / candidateII);
    pipeline.contextSlots = std::max(2U, pipeline.stageCount + 1);
    for (const ScalarRecurrenceDependence &dependence :
         pipeline.scalarRecurrences)
    {
        pipeline.contextSlots =
            std::max(pipeline.contextSlots, dependence.distance + 1);
    }
    pipeline.operationStartCycle = selectedStart;
    if (pipeline.conservativeMemoryDependence &&
        pipeline.memoryDependenceMII > pipeline.requestedII &&
        pipeline.memoryDependenceMII >= pipeline.memoryMII &&
        pipeline.memoryDependenceMII >= pipeline.recurrenceMII)
    {
        pipeline.limitingFactor = "conservative mixed read/write memory dependence";
    }
    else if (pipeline.memoryDependenceMII > pipeline.requestedII &&
             pipeline.memoryDependenceMII >= pipeline.memoryMII &&
             pipeline.memoryDependenceMII >= pipeline.recurrenceMII)
    {
        pipeline.limitingFactor = "loop-carried memory dependence";
    }
    else if (pipeline.memoryMII > pipeline.requestedII &&
             pipeline.memoryMII >= pipeline.recurrenceMII)
    {
        pipeline.limitingFactor = "memory port capacity";
    }
    else if (pipeline.recurrenceMII > pipeline.requestedII)
    {
        pipeline.limitingFactor = "loop-carried recurrence";
    }
    else
    {
        pipeline.limitingFactor = "requested initiation interval";
    }

    if (pipeline.achievedII > pipeline.requestedII)
    {
        context.addWarning(
            "BACKEND_PIPELINE_II_RELAXED",
            "Requested II=" + std::to_string(pipeline.requestedII) +
                " cannot be achieved; the RTL backend uses II=" +
                std::to_string(pipeline.achievedII) + " because of " +
                pipeline.limitingFactor + ".",
            pipeline.label);
    }

    for (Instruction *instruction : operations)
    {
        InstructionSchedule schedule;
        schedule.instruction = instruction;
        const auto valueName = context.valueNames.find(instruction);
        schedule.name = valueName == context.valueNames.end()
                            ? "void_" + std::to_string(valueIndex++)
                            : valueName->second;
        schedule.opcode = instruction->getOpcodeName();
        schedule.block = blockSchedule.name;
        schedule.resultWidth = getResultWidth(*instruction);
        schedule.startCycle = selectedStart.at(instruction);
        schedule.latency = getLatency(context, *instruction);
        pipeline.operationStage[schedule.name] = static_cast<unsigned>(
            schedule.startCycle / pipeline.achievedII);
        pipeline.iterationOffset[schedule.name] =
            schedule.startCycle % pipeline.achievedII;
        blockSchedule.instructions.push_back(std::move(schedule));
    }

    blockSchedule.pipelined = true;
    blockSchedule.stateCount = 1;
    blockSchedule.executionLatency =
        2 + pipeline.pipelineLatency +
        static_cast<long long>(pipeline.tripCount - 1) * pipeline.achievedII;
    return true;
}

} // namespace

char HI_OperationScheduling::ID = 0;

HI_OperationScheduling::HI_OperationScheduling(HLSBackendContextPtr context)
    : ModulePass(ID), context(std::move(context))
{
}

bool HI_OperationScheduling::runOnModule(Module &module)
{
    if (!context || context->hasErrors() || !context->top)
        return false;

    context->module = &module;
    context->schedule = ScheduleResult();
    context->valueNames.clear();
    context->forwardedLoads.clear();
    context->ordinaryBackedgeBypasses.clear();
    context->ordinaryReadPhiBypasses.clear();
    context->ordinaryReturnExits.clear();
    context->ordinaryEntrySuccessor = nullptr;
    discoverHardwareConstants(*context);
    LoopInfo &loops = getAnalysis<LoopInfoWrapperPass>(*context->top).getLoopInfo();
    selectCombinationalValues(*context, loops);

    for (const BasicBlock &block : *context->top)
    {
        const auto *ret = dyn_cast<ReturnInst>(block.getTerminator());
        if (&block == &context->top->getEntryBlock() || !ret || pred_empty(&block) ||
            !std::all_of(block.begin(), block.end(), [&](const Instruction &instruction) {
                return &instruction == ret || isIgnoredIntrinsic(instruction) ||
                    context->constantValues.count(&instruction) ||
                    context->combinationalValues.count(&instruction);
            }))
            continue;
        const auto *returnValue = dyn_cast_or_null<Instruction>(ret->getReturnValue());
        std::map<const Instruction *, double> delays;
        if (returnValue && context->combinationalValues.count(returnValue) &&
            backedgeExpressionDelay(*context, *returnValue, delays) >
                context->configuration.clockPeriodNs * 0.7)
            continue;
        const bool ordinaryPredecessors = std::all_of(pred_begin(&block), pred_end(&block),
            [&](const BasicBlock *predecessor) {
                return isa<BranchInst>(predecessor->getTerminator()) &&
                    std::none_of(context->pipelineRegions.begin(), context->pipelineRegions.end(),
                        [&](const LoopPipelineInfo &pipeline) {
                            return pipeline.header == predecessor ||
                                std::find(pipeline.blocks.begin(), pipeline.blocks.end(), predecessor) != pipeline.blocks.end();
                        });
            });
        if (ordinaryPredecessors)
            context->ordinaryReturnExits.insert(&block);
    }

    Function &top = *context->top;
    ScalarEvolution &scalarEvolution =
        getAnalysis<ScalarEvolutionWrapperPass>(top).getSE();
    std::set<std::string> usedValueNames;
    unsigned argumentIndex = 0;
    for (Argument &argument : top.args())
    {
        if (!argument.getType()->isIntegerTy())
        {
            ++argumentIndex;
            continue;
        }
        const std::string sourceName = argument.hasName()
                                           ? argument.getName().str()
                                           : "arg" + std::to_string(argumentIndex);
        context->valueNames[&argument] = uniqueName(
            "arg_" + sanitizeVerilogIdentifier(sourceName,
                                                 "arg" + std::to_string(argumentIndex)),
            usedValueNames);
        ++argumentIndex;
    }

    unsigned valueIndex = 0;
    for (BasicBlock &block : top)
    {
        for (Instruction &instruction : block)
        {
            if (!instruction.getType()->isIntegerTy())
                continue;
            const std::string sourceName = instruction.hasName()
                                               ? instruction.getName().str()
                                               : "v" + std::to_string(valueIndex);
            context->valueNames[&instruction] = uniqueName(
                "v_" + sanitizeVerilogIdentifier(sourceName,
                                                   "v" + std::to_string(valueIndex)),
                usedValueNames);
            ++valueIndex;
        }
    }

    std::set<std::string> usedBlockNames;
    unsigned nextState = 1;
    unsigned blockIndex = 0;
    for (BasicBlock &block : top)
    {
        BlockSchedule blockSchedule;
        blockSchedule.block = &block;
        const std::string sourceName =
            block.hasName() ? block.getName().str()
                            : "block" + std::to_string(blockIndex);
        blockSchedule.name = uniqueName(
            sanitizeVerilogIdentifier(sourceName,
                                      "block" + std::to_string(blockIndex)),
            usedBlockNames);

        if (LoopPipelineInfo *pipeline = findPipeline(*context, &block))
        {
            if (!schedulePipelineBlock(*context, block, *pipeline,
                                       blockSchedule, valueIndex))
            {
                return false;
            }
            blockSchedule.firstState = nextState;
            nextState += blockSchedule.stateCount;
            context->schedule.blocks.push_back(std::move(blockSchedule));
            ++blockIndex;
            continue;
        }

        for (Instruction &instruction : block)
            if (canForwardMemoryResponse(*context, instruction))
                context->forwardedLoads.insert(&instruction);
        const unsigned initialValueIndex = valueIndex;
        const unsigned initialOrderingCycles = context->schedule.memoryOrderingCycles;
        const unsigned initialPortConflicts = context->schedule.memoryPortConflicts;
        const unsigned initialPortCycles = context->schedule.memoryPortSerializationCycles;
        while (true)
        {
            valueIndex = initialValueIndex;
            context->schedule.memoryOrderingCycles = initialOrderingCycles;
            context->schedule.memoryPortConflicts = initialPortConflicts;
            context->schedule.memoryPortSerializationCycles = initialPortCycles;
            blockSchedule.instructions.clear();
            std::map<const Instruction *, int> completionCycle;
            std::vector<std::pair<const MemoryAccessInfo *, int>> memoryRequests;
            std::map<std::string, std::map<unsigned, std::map<int, unsigned>>> memoryPortUsage;
            int maximumCompletion = 0;
            for (Instruction &instruction : block)
            {
                if (!isDatapathInstruction(instruction) || context->constantValues.count(&instruction))
                    continue;

                int startCycle = 0;
                for (const Use &operandUse : instruction.operands())
                {
                    const auto *operandInstruction = dyn_cast<Instruction>(operandUse.get());
                    if (!operandInstruction || operandInstruction->getParent() != &block)
                        continue;
                    auto completion = completionCycle.find(operandInstruction);
                    if (completion != completionCycle.end())
                        startCycle = std::max(startCycle, completion->second);
                }

                auto memoryAccess = context->memoryAccesses.find(&instruction);
                if (memoryAccess != context->memoryAccesses.end())
                {
                    const MemoryObject *memory =
                        findMemoryObject(*context, memoryAccess->second.memory);
                    const unsigned ports = memory ? memory->ports : 1;
                    const std::vector<unsigned> banks =
                        memory ? accessBanks(*memory, memoryAccess->second)
                               : std::vector<unsigned>{0};
                    const int dataReadyCycle = startCycle;
                    for (const auto &previous : memoryRequests)
                    {
                        const MemoryAccessInfo &source = *previous.first;
                        const MemoryAccessInfo &sink = memoryAccess->second;
                        if (source.memory != sink.memory)
                            continue;
                        if (sink.isFIFO)
                        {
                            // FIFO reads consume tokens: RAR must be ordered too.
                            startCycle = std::max(startCycle, previous.second + 1);
                            continue;
                        }
                        if (!source.isWrite && !sink.isWrite)
                            continue;
                        if (source.bankMappingExact && sink.bankMappingExact &&
                            std::none_of(source.possibleBanks.begin(), source.possibleBanks.end(),
                                         [&](unsigned bank)
                                         {
                                             return std::find(sink.possibleBanks.begin(),
                                                              sink.possibleBanks.end(),
                                                              bank) != sink.possibleBanks.end();
                                         }))
                            continue;
                        const SCEV *difference = scalarEvolution.getMinusSCEV(
                            scalarEvolution.getSCEV(const_cast<Value *>(source.pointerOperand)),
                            scalarEvolution.getSCEV(const_cast<Value *>(sink.pointerOperand)));
                        const auto *constant = dyn_cast<SCEVConstant>(difference);
                        if (constant && !constant->getAPInt().isZero())
                            continue;
                        // Scalar inputs can be sampled on the same edge that
                        // commits a following write. RAW/WAW still need a later
                        // edge; synchronous RAM requests retain their ordering.
                        const int separation = memory && memory->hasCombinationalScalarReads() &&
                            !source.isWrite && sink.isWrite ? 0 : 1;
                        startCycle = std::max(startCycle, previous.second + separation);
                    }
                    context->schedule.memoryOrderingCycles += startCycle - dataReadyCycle;
                    const int requestedCycle = startCycle;
                    memoryRequests.emplace_back(&memoryAccess->second, startCycle);
                    auto conflicts = [&](int cycle)
                    {
                        if (memory && memory->hasCombinationalScalarReads() &&
                            !memoryAccess->second.isWrite)
                            return false;
                        return std::any_of(
                            banks.begin(), banks.end(),
                            [&](unsigned bank) {
                                return memoryPortUsage[memoryAccess->second.memory][bank][cycle] >=
                                       ports;
                            });
                    };
                    while (conflicts(startCycle))
                    {
                        ++startCycle;
                    }
                    memoryRequests.back().second = startCycle;
                    if (startCycle != requestedCycle)
                    {
                        ++context->schedule.memoryPortConflicts;
                        context->schedule.memoryPortSerializationCycles +=
                            static_cast<unsigned>(startCycle - requestedCycle);
                    }
                    if (!memory || !memory->hasCombinationalScalarReads() ||
                        memoryAccess->second.isWrite)
                        for (unsigned bank : banks)
                            ++memoryPortUsage[memoryAccess->second.memory][bank][startCycle];
                }

                const int latency = getLatency(*context, instruction);
                completionCycle[&instruction] = startCycle + latency;
                maximumCompletion = std::max(maximumCompletion, startCycle + latency);

                InstructionSchedule schedule;
                schedule.instruction = &instruction;
                auto valueName = context->valueNames.find(&instruction);
                schedule.name = valueName == context->valueNames.end()
                                    ? "void_" + std::to_string(valueIndex++)
                                    : valueName->second;
                schedule.opcode = instruction.getOpcodeName();
                schedule.block = blockSchedule.name;
                schedule.resultWidth = getResultWidth(instruction);
                schedule.startCycle = startCycle;
                schedule.latency = latency;
                blockSchedule.instructions.push_back(std::move(schedule));
            }

            LoopPipelineInfo localRegion;
            localRegion.header = &block;
            std::map<const Instruction *, int> startCycles;
            for (const InstructionSchedule &schedule : blockSchedule.instructions)
                startCycles[schedule.instruction] = schedule.startCycle;
            bool retryCapture = false;
            for (auto it = context->forwardedLoads.begin(); it != context->forwardedLoads.end();)
            {
                if ((*it)->getParent() != &block)
                {
                    ++it;
                    continue;
                }
                std::set<int> cycles;
                if (!pipelineSamplingCycles(*context, localRegion, *it, startCycles, cycles) ||
                    cycles != std::set<int>{completionCycle.at(*it)})
                {
                    it = context->forwardedLoads.erase(it);
                    retryCapture = true;
                }
                else
                    ++it;
            }
            if (retryCapture)
                continue;

            const bool terminalPipelineExit = isTerminalPipelineExit(*context, block);
            int controlCycle = std::max(0, maximumCompletion - 1);
            std::function<void(const Value *)> requireReady = [&](const Value *value)
            {
                const auto *instruction = dyn_cast<Instruction>(value);
                const auto ready = completionCycle.find(instruction);
                if (ready != completionCycle.end())
                    controlCycle = std::max(controlCycle, ready->second);
                else if (instruction && context->combinationalValues.count(instruction))
                    for (const Use &operand : instruction->operands())
                        requireReady(operand.get());
            };
            for (const Use &operand : block.getTerminator()->operands())
                requireReady(operand.get());
            std::map<const PHINode *, const Instruction *> bypassCandidates;
            std::map<const Instruction *, double> bypassDelays;
            const Loop *loop = loops.getLoopFor(&block);
            const auto *selfBranch = dyn_cast<BranchInst>(block.getTerminator());
            if (loop && loop->getHeader() == &block && loop->getNumBlocks() == 1 &&
                std::none_of(context->pipelineRegions.begin(), context->pipelineRegions.end(),
                    [&](const LoopPipelineInfo &pipeline) {
                        return pipeline.header == &block ||
                            std::find(pipeline.blocks.begin(), pipeline.blocks.end(), &block) !=
                                pipeline.blocks.end();
                    }) &&
                selfBranch && selfBranch->isConditional() &&
                ((selfBranch->getSuccessor(0) == &block) !=
                 (selfBranch->getSuccessor(1) == &block)))
                for (const PHINode &phi : block.phis())
                {
                    if (phi.getNumIncomingValues() != 2 || phi.getBasicBlockIndex(&block) < 0)
                        continue;
                    const auto *update = dyn_cast<Instruction>(phi.getIncomingValueForBlock(&block));
                    if (update && update->getParent() == &block &&
                        !context->combinationalValues.count(update) &&
                        !context->constantValues.count(update) &&
                        completionCycle.count(update) && getLatency(*context, *update) == 1 &&
                        backedgeExpressionDelay(*context, *update, bypassDelays) <=
                            context->configuration.clockPeriodNs * 0.7)
                        bypassCandidates[&phi] = update;
                }
            std::vector<OrdinaryReadPhiBypass> readPhiCandidates;
            const bool ordinaryBlock = std::none_of(context->pipelineRegions.begin(),
                context->pipelineRegions.end(), [&](const LoopPipelineInfo &pipeline) {
                    return pipeline.header == &block ||
                        std::find(pipeline.blocks.begin(), pipeline.blocks.end(), &block) !=
                            pipeline.blocks.end();
                });
            for (const BasicBlock *successor : successors(&block))
            {
                if (context->ordinaryReturnExits.count(successor))
                    if (const Value *value = cast<ReturnInst>(successor->getTerminator())->getReturnValue())
                        requireReady(value);
                if (findPipeline(*context, successor))
                    continue;
                for (const PHINode &phi : successor->phis())
                    if (const Value *incoming = phi.getIncomingValueForBlock(&block))
                    {
                        const auto *load = dyn_cast<LoadInst>(incoming);
                        const auto access = context->memoryAccesses.find(load);
                        const MemoryObject *memory = access == context->memoryAccesses.end() ?
                            nullptr : findMemoryObject(*context, access->second.memory);
                        // The existing response capture is unchanged. Only the
                        // edge-local PHI samples Q directly on that same edge.
                        const bool ordinarySuccessor = std::none_of(context->pipelineRegions.begin(),
                            context->pipelineRegions.end(), [&](const LoopPipelineInfo &pipeline) {
                                return pipeline.header == successor ||
                                    std::find(pipeline.blocks.begin(), pipeline.blocks.end(), successor) !=
                                        pipeline.blocks.end();
                            });
                        if (ordinaryBlock && ordinarySuccessor && load && load->getParent() == &block &&
                            !context->forwardedLoads.count(load) && completionCycle.count(load) &&
                            memory && (memory->readLatency >= 1 || memory->fifoReadOnRequest) &&
                            (memory->banks == 1 || access->second.possibleBanks.size() == 1))
                        {
                            controlCycle = std::max(controlCycle, completionCycle.at(load) - 1);
                            if (std::none_of(readPhiCandidates.begin(), readPhiCandidates.end(),
                                    [&](const OrdinaryReadPhiBypass &item) { return item.phi == &phi; }))
                                readPhiCandidates.push_back({&block, &phi, load});
                            continue;
                        }
                        const auto bypass = bypassCandidates.find(&phi);
                        if (bypass != bypassCandidates.end())
                            controlCycle = std::max(controlCycle,
                                completionCycle.at(bypass->second) - 1);
                        else
                            requireReady(incoming);
                    }
            }
            // A shared operator drives the right expression only on its issue
            // edge. Later transitions must use the retained result register.
            for (const auto &bypass : bypassCandidates)
                if (controlCycle == completionCycle.at(bypass.second) - 1)
                    context->ordinaryBackedgeBypasses.insert(bypass);
            for (const OrdinaryReadPhiBypass &bypass : readPhiCandidates)
                if (controlCycle == completionCycle.at(bypass.load) - 1)
                    context->ordinaryReadPhiBypasses.push_back(bypass);
            // Side effects and the control transition may share an edge. Values
            // sampled by the branch, return, or successor PHI must already be ready.
            blockSchedule.stateCount = (terminalPipelineExit || context->ordinaryReturnExits.count(&block)) ?
                0 : controlCycle + 1;
            const auto *branch = dyn_cast<BranchInst>(block.getTerminator());
            if (&block == &top.getEntryBlock() && branch && branch->isUnconditional() &&
                !findPipeline(*context, branch->getSuccessor(0)) &&
                !context->ordinaryReturnExits.count(branch->getSuccessor(0)) &&
                std::all_of(block.begin(), block.end(), [&](const Instruction &instruction) {
                    return &instruction == branch || isa<AllocaInst>(instruction) ||
                           isIgnoredIntrinsic(instruction) || context->constantValues.count(&instruction) ||
                           ordinaryEntryValueDelay(*context, &instruction) <=
                               context->configuration.clockPeriodNs * 0.7;
                }))
            {
                const BasicBlock *successor = branch->getSuccessor(0);
                const bool initialPhisReady = std::all_of(successor->phis().begin(), successor->phis().end(),
                    [&](const PHINode &phi) {
                        const Value *initial = phi.getIncomingValueForBlock(&block);
                        return initial && ordinaryEntryValueDelay(*context, initial) <=
                            context->configuration.clockPeriodNs * 0.7;
                    });
                if (initialPhisReady)
                {
                    // Initial PHIs use a separate live-port expression on the
                    // accepted-start edge, not the argument registers being set.
                    context->ordinaryEntrySuccessor = successor;
                    blockSchedule.stateCount = 0;
                    for (InstructionSchedule &operation : blockSchedule.instructions)
                        if (operation.instruction->getType()->isIntegerTy())
                        {
                            context->combinationalValues.insert(operation.instruction);
                            operation.startCycle = 0;
                            operation.latency = 0;
                        }
                }
            }
            if (&block == &top.getEntryBlock() && branch && branch->isUnconditional() &&
                findPipeline(*context, branch->getSuccessor(0)) && block.phis().empty() &&
                std::all_of(blockSchedule.instructions.begin(), blockSchedule.instructions.end(),
                    [](const InstructionSchedule &operation) {
                        return operation.resultWidth == 0 && operation.instruction &&
                               !operation.instruction->mayReadOrWriteMemory() &&
                               !operation.instruction->mayHaveSideEffects();
                    }))
                blockSchedule.stateCount = 0;
            blockSchedule.executionLatency = blockSchedule.stateCount;
            break;
        }
        blockSchedule.firstState = nextState;
        nextState += blockSchedule.stateCount;
        context->schedule.blocks.push_back(std::move(blockSchedule));
        ++blockIndex;
    }

    std::map<const BasicBlock *, const BlockSchedule *> blockSchedules;
    for (BlockSchedule &block : context->schedule.blocks)
    {
        blockSchedules[block.block] = &block;
        for (InstructionSchedule &instruction : block.instructions)
            context->schedule.instructionMap[instruction.instruction] = &instruction;
    }

    for (LoopPipelineInfo &pipeline : context->pipelineRegions)
    {
        pipeline.initializedAtEntry = false;
        BlockSchedule *entry = &context->schedule.blocks.front();
        const auto *branch = dyn_cast<BranchInst>(entry->block->getTerminator());
        if (entry->stateCount != 0 || pipeline.preheader != entry->block ||
            !branch || !branch->isUnconditional() ||
            branch->getSuccessor(0) != pipeline.header)
            continue;
        for (BlockSchedule &scheduled : context->schedule.blocks)
            if (scheduled.block == pipeline.header)
            {
                entry->firstState = scheduled.firstState;
                pipeline.initializedAtEntry = std::all_of(pipeline.phis.begin(), pipeline.phis.end(),
                    [&](const PipelinePhiInfo &phi) {
                        return !pipeline.contextValues.count(phi.phi) ||
                               isa<ConstantInt>(phi.initialValue) ||
                               isa<Argument>(phi.initialValue) ||
                               context->constantValues.count(phi.initialValue);
                    });
                if (pipeline.initializedAtEntry)
                    --scheduled.executionLatency;
            }
    }
    if (context->ordinaryEntrySuccessor)
        context->schedule.blocks.front().firstState =
            blockSchedules.at(context->ordinaryEntrySuccessor)->firstState;
    context->schedule.stateCount = nextState;
    context->schedule.pipelines = context->pipelineRegions;
    std::map<const BasicBlock *, const BasicBlock *> pipelineExits;
    for (const LoopPipelineInfo &pipeline : context->pipelineRegions)
        pipelineExits[pipeline.header] = pipeline.exit;
    context->schedule.hasControlFlowCycle =
        detectControlFlowCycle(top, blockSchedules, pipelineExits);
    if (context->schedule.hasControlFlowCycle)
    {
        context->schedule.latencyCycles = -1;
        context->addWarning(
            "BACKEND_DYNAMIC_LATENCY",
            "The top function contains a control-flow cycle. The schedule is valid, "
            "but a single static latency cannot be reported without loop-trip analysis.");
    }
    else
    {
        std::map<const BasicBlock *, long long> latencyMemo;
        context->schedule.latencyCycles = getAcyclicLatency(
            &top.getEntryBlock(), blockSchedules, pipelineExits,
            latencyMemo);
    }
    context->schedule.achievedII = 0;
    for (const LoopPipelineInfo &pipeline : context->pipelineRegions)
        context->schedule.achievedII =
            std::max(context->schedule.achievedII, pipeline.achievedII);
    context->schedule.valid = true;
    return false;
}

void HI_OperationScheduling::getAnalysisUsage(AnalysisUsage &usage) const
{
    usage.addRequired<LoopInfoWrapperPass>();
    usage.addRequired<ScalarEvolutionWrapperPass>();
    usage.setPreservesAll();
}
