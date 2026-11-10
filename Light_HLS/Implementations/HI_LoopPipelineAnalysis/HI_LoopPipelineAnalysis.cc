#include "HI_LoopPipelineAnalysis.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"

#include <algorithm>
#include <map>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;
using namespace light_hls::backend;

namespace
{

std::string loopName(const Loop &loop)
{
    return loop.getHeader()->getParent()->getName().str() + "-" +
           loop.getHeader()->getName().str();
}

void collectLoops(Loop *loop, std::vector<Loop *> &loops)
{
    loops.push_back(loop);
    for (Loop *child : loop->getSubLoops())
        collectLoops(child, loops);
}

bool affinePhiStep(const PHINode &phi, const Value *recurrence,
                   long long &step)
{
    const auto *binary = dyn_cast_or_null<BinaryOperator>(recurrence);
    if (!binary || (binary->getOpcode() != Instruction::Add &&
                    binary->getOpcode() != Instruction::Sub))
    {
        return false;
    }

    const ConstantInt *constant = nullptr;
    if (binary->getOperand(0) == &phi)
        constant = dyn_cast<ConstantInt>(binary->getOperand(1));
    else if (binary->getOpcode() == Instruction::Add &&
             binary->getOperand(1) == &phi)
        constant = dyn_cast<ConstantInt>(binary->getOperand(0));
    if (!constant || constant->getBitWidth() > 64)
        return false;

    step = constant->getSExtValue();
    if (binary->getOpcode() == Instruction::Sub)
        step = -step;
    return true;
}

bool isCanonicalSingleBlockLoop(const Loop &loop)
{
    if (loop.getBlocks().size() != 1 || loop.getHeader() != loop.getLoopLatch())
        return false;
    const auto *branch = dyn_cast<BranchInst>(loop.getHeader()->getTerminator());
    if (!branch || !branch->isConditional())
        return false;
    const BasicBlock *exit = loop.getUniqueExitBlock();
    if (!exit)
        return false;
    return (branch->getSuccessor(0) == loop.getHeader() &&
            branch->getSuccessor(1) == exit) ||
           (branch->getSuccessor(1) == loop.getHeader() &&
            branch->getSuccessor(0) == exit);
}

void flattenContinuousNest(const HLSBackendContext &context, const Loop &loop,
                           LoopPipelineInfo &pipeline)
{
    pipeline.sourceTripCount = pipeline.tripCount;
    pipeline.flatteningReason = "not a proven continuous perfect nest";
    const Loop *outer = loop.getParentLoop();
    if (!outer || outer->getSubLoops().size() != 1 ||
        outer->getNumBlocks() != 3 ||
        outer->getHeader() != loop.getLoopPreheader() ||
        outer->getLoopLatch() != loop.getUniqueExitBlock() ||
        !outer->getLoopPreheader() || !outer->getUniqueExitBlock())
        return;
    const auto count = context.configuration.irLoopTripCounts.find(loopName(*outer));
    if (count == context.configuration.irLoopTripCounts.end() || count->second <= 0 ||
        static_cast<unsigned long long>(count->second) * pipeline.tripCount >
            std::numeric_limits<unsigned>::max())
        return;
    const auto *entryBranch = dyn_cast<BranchInst>(outer->getHeader()->getTerminator());
    const auto *exitBranch = dyn_cast<BranchInst>(outer->getLoopLatch()->getTerminator());
    if (!entryBranch || !entryBranch->isUnconditional() ||
        entryBranch->getSuccessor(0) != loop.getHeader() || !exitBranch ||
        !exitBranch->isConditional() ||
        !((exitBranch->getSuccessor(0) == outer->getHeader() &&
           exitBranch->getSuccessor(1) == outer->getUniqueExitBlock()) ||
          (exitBranch->getSuccessor(1) == outer->getHeader() &&
           exitBranch->getSuccessor(0) == outer->getUniqueExitBlock())))
        return;

    // Only loop-control arithmetic may be bypassed. No row-level effects or
    // escaping values are allowed, including PHIs in the outer exit block.
    for (const BasicBlock *block : outer->blocks())
        for (const Instruction &instruction : *block)
        {
            if (isa<DbgInfoIntrinsic>(instruction))
                continue;
            if (block != loop.getHeader() &&
                (instruction.mayHaveSideEffects() || instruction.mayReadFromMemory()) &&
                !isa<BranchInst>(instruction))
                return;
            for (const User *user : instruction.users())
            {
                const auto *consumer = dyn_cast<Instruction>(user);
                if (!consumer || isa<DbgInfoIntrinsic>(consumer))
                    continue;
                if (!outer->contains(consumer) ||
                    (block == loop.getHeader() && !loop.contains(consumer)))
                    return;
            }
        }

    std::set<const Value *> needed;
    std::vector<const Value *> worklist;
    for (const Instruction &instruction : *loop.getHeader())
        if (instruction.mayReadOrWriteMemory())
            worklist.push_back(&instruction);
    while (!worklist.empty())
    {
        const Value *value = worklist.back();
        worklist.pop_back();
        if (!needed.insert(value).second)
            continue;
        const auto *instruction = dyn_cast<Instruction>(value);
        if (!instruction || isa<PHINode>(instruction))
            continue;
        if (outer->contains(instruction) && !loop.contains(instruction))
            return;
        for (const Value *operand : instruction->operand_values())
            worklist.push_back(operand);
    }
    for (const PipelinePhiInfo &phi : pipeline.phis)
    {
        if (!needed.count(phi.phi))
            continue;
        const auto *outerPhi = dyn_cast<PHINode>(phi.initialValue);
        long long outerStep = 0;
        if (!phi.affine || !outerPhi || outerPhi->getParent() != outer->getHeader() ||
            !affinePhiStep(*outerPhi,
                outerPhi->getIncomingValueForBlock(outer->getLoopLatch()), outerStep) ||
            static_cast<__int128>(phi.affineStep) * pipeline.tripCount != outerStep)
            return;
    }
    // Direct uses of outer counters would require quotient/remainder index
    // reconstruction. Keep those nests in their original, drained form.
    for (const Value *value : needed)
        if (const auto *instruction = dyn_cast<Instruction>(value))
            if (outer->contains(instruction) && !loop.contains(instruction))
                return;
    pipeline.tripCount *= static_cast<unsigned>(count->second);
    pipeline.exit = outer->getUniqueExitBlock();
    pipeline.flattenedLoopCount = 2;
    pipeline.flatteningReason = "continuous affine addresses across a perfect two-loop nest";
}

} // namespace

char HI_LoopPipelineAnalysis::ID = 0;

HI_LoopPipelineAnalysis::HI_LoopPipelineAnalysis(
    HLSBackendContextPtr context)
    : ModulePass(ID), context(std::move(context))
{
}

bool HI_LoopPipelineAnalysis::runOnModule(Module &module)
{
    if (!context || context->hasErrors() || !context->top)
        return false;

    context->module = &module;
    context->pipelineRegions.clear();
    if (context->configuration.pragmas.pipeline.empty())
        return false;

    Function &top = *context->top;
    DominatorTree dominatorTree(top);
    LoopInfo loopInfo(dominatorTree);
    std::vector<Loop *> loops;
    for (Loop *loop : loopInfo)
        collectLoops(loop, loops);

    std::map<std::string, Loop *> labelToLoop;
    std::map<Loop *, std::string> loopToLabel;
    for (Loop *loop : loops)
    {
        const std::string irName = loopName(*loop);
        const auto label = context->configuration.irLoopLabels.find(irName);
        if (label == context->configuration.irLoopLabels.end())
            continue;
        labelToLoop[label->second] = loop;
        loopToLabel[loop] = label->second;
    }

    std::set<std::string> requestedLabels;
    for (const auto &directive : context->configuration.pragmas.pipeline)
        requestedLabels.insert(directive.first);

    for (const auto &directive : context->configuration.pragmas.pipeline)
    {
        const auto target = labelToLoop.find(directive.first);
        if (target == labelToLoop.end())
        {
            context->addError(
                "BACKEND_PIPELINE_LOOP_NOT_FOUND",
                "The pipelined source loop was not found in the final LLVM IR.",
                directive.first);
            continue;
        }

        Loop *loop = target->second;
        for (Loop *ancestor = loop->getParentLoop(); ancestor;
             ancestor = ancestor->getParentLoop())
        {
            const auto ancestorLabel = loopToLabel.find(ancestor);
            if (ancestorLabel != loopToLabel.end() &&
                requestedLabels.count(ancestorLabel->second))
            {
                context->addError(
                    "BACKEND_UNSUPPORTED_NESTED_PIPELINES",
                    "Nested PIPELINE directives are unsupported. Pipeline the "
                    "innermost loop, or pipeline only the outer loop so all "
                    "descendant loops can be fully unrolled.",
                    ancestorLabel->second + " -> " + directive.first);
            }
        }
        if (context->hasErrors())
            continue;

        // Adjacent loops may share the preceding loop's conditional latch as
        // their unique entry predecessor. Initialization still happens only
        // after that predecessor transfers control to this pipeline state.
        if (!isCanonicalSingleBlockLoop(*loop) || !loop->getLoopPredecessor())
        {
            context->addError(
                "BACKEND_UNSUPPORTED_PIPELINE_LOOP_SHAPE",
                "The initial pipeline backend requires a canonical single-block "
                "loop with one entry predecessor, one latch, and one exit.",
                directive.first);
            continue;
        }

        const std::string irName = loopName(*loop);
        const auto tripCount =
            context->configuration.irLoopTripCounts.find(irName);
        if (tripCount == context->configuration.irLoopTripCounts.end() ||
            tripCount->second <= 0)
        {
            context->addError(
                "BACKEND_PIPELINE_TRIP_COUNT_UNKNOWN",
                "The initial pipeline backend requires a positive compile-time "
                "constant trip count.",
                directive.first);
            continue;
        }

        LoopPipelineInfo pipeline;
        pipeline.header = loop->getHeader();
        pipeline.preheader = loop->getLoopPredecessor();
        pipeline.latch = loop->getLoopLatch();
        pipeline.exit = loop->getUniqueExitBlock();
        pipeline.irLoopName = irName;
        pipeline.label = directive.first;
        pipeline.requestedII = directive.second > 0 ? directive.second : 1;
        pipeline.tripCount = static_cast<unsigned>(tripCount->second);
        pipeline.blocks.push_back(loop->getHeader());

        for (const Instruction &instruction : *loop->getHeader())
        {
            const auto *phi = dyn_cast<PHINode>(&instruction);
            if (!phi)
                break;
            const Value *initial =
                phi->getIncomingValueForBlock(pipeline.preheader);
            const Value *recurrence =
                phi->getIncomingValueForBlock(loop->getLoopLatch());
            if (!initial || !recurrence)
            {
                context->addError(
                    "BACKEND_PIPELINE_PHI_INCOMPLETE",
                    "A pipeline loop PHI does not have both preheader and latch "
                    "incoming values.",
                    directive.first);
                continue;
            }
            PipelinePhiInfo phiInfo;
            phiInfo.phi = phi;
            phiInfo.initialValue = initial;
            phiInfo.recurrenceValue = recurrence;
            phiInfo.affine =
                affinePhiStep(*phi, recurrence, phiInfo.affineStep);
            pipeline.phis.push_back(phiInfo);
        }
        if (!context->hasErrors())
        {
            flattenContinuousNest(*context, *loop, pipeline);
            pipeline.valid = true;
            context->pipelineRegions.push_back(std::move(pipeline));
        }
    }
    return false;
}

void HI_LoopPipelineAnalysis::getAnalysisUsage(AnalysisUsage &usage) const
{
    usage.setPreservesAll();
}
