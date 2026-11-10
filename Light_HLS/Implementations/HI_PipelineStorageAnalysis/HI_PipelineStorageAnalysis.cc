#include "HI_PipelineStorageAnalysis.h"
#include "HI_PipelineValueLifetime.h"

#include "llvm/IR/Instructions.h"

#include <algorithm>
#include <functional>
#include <limits>

using namespace llvm;
using namespace light_hls::backend;

char HI_PipelineStorageAnalysis::ID = 0;

HI_PipelineStorageAnalysis::HI_PipelineStorageAnalysis(HLSBackendContextPtr context)
    : ModulePass(ID), context(std::move(context))
{
}

bool HI_PipelineStorageAnalysis::runOnModule(Module &module)
{
    if (!context || context->hasErrors() || !context->schedule.valid)
        return false;
    for (LoopPipelineInfo &pipeline : context->pipelineRegions)
    {
        pipeline.valueStorageSlots.clear();
        pipeline.valueLiveIntervals.clear();
        pipeline.retirementOnlyValues.clear();
        pipeline.storageAliases.clear();
        for (const Value *value : pipeline.contextValues)
        {
            pipeline.valueStorageSlots[value] = pipeline.contextSlots;
            if (value == pipeline.forwardedReturnValue)
            {
                // Only exact recurrence feedback and the terminal return use
                // this producer. Both sample its expression on the write edge.
                pipeline.valueStorageSlots[value] = 1;
                pipeline.retirementOnlyValues.insert(value);
                continue;
            }
            const auto *instruction = dyn_cast<Instruction>(value);
            if (!instruction)
                continue;
            const Instruction *producer = instruction;
            int iterationShift = 0;
            if (isa<PHINode>(instruction))
            {
                producer = nullptr;
                for (const ScalarRecurrenceDependence &dependence : pipeline.scalarRecurrences)
                    if (dependence.phi == instruction && dependence.exact &&
                        dependence.distance == 1)
                    {
                        producer = dependence.source;
                        iterationShift = pipeline.achievedII;
                    }
                if (!producer)
                    continue;
            }
            const auto scheduled = context->schedule.instructionMap.find(producer);
            if (scheduled == context->schedule.instructionMap.end())
                continue;
            int writeCycle = scheduled->second->startCycle - iterationShift;
            if (isa<LoadInst>(producer))
                writeCycle += scheduled->second->latency - 1;
            std::set<int> samplingCycles;
            bool retirementOnly = false;
            if (!pipelineSamplingCycles(*context, pipeline, value, pipeline.operationStartCycle,
                                        samplingCycles, true, &retirementOnly) ||
                *samplingCycles.begin() <= writeCycle)
                continue;
            const int lastUse = *samplingCycles.rbegin();
            pipeline.valueLiveIntervals[value] = {writeCycle, lastUse};
            // A result observed only when the entire pipeline drains needs
            // just the latest iteration, not a separate copy of every value.
            if (retirementOnly && !isa<PHINode>(instruction))
            {
                pipeline.valueStorageSlots[value] = 1;
                pipeline.retirementOnlyValues.insert(value);
                continue;
            }
            // A consumer samples the old value on the same edge that the
            // next iteration overwrites it with a nonblocking assignment.
            const unsigned required =
                std::max(1, (lastUse - writeCycle + pipeline.achievedII - 1) / pipeline.achievedII);
            // A divisor preserves iteration identity across the controller
            // ring's wrap, without introducing a second allocation counter.
            for (unsigned slots = required; slots < pipeline.contextSlots; ++slots)
                if (pipeline.contextSlots % slots == 0)
                {
                    pipeline.valueStorageSlots[value] = slots;
                    break;
                }
        }
        unsigned alternativeRing = 1;
        while (alternativeRing < pipeline.contextSlots &&
               alternativeRing <= std::numeric_limits<unsigned>::max() / 2)
            alternativeRing *= 2;
        if (alternativeRing > pipeline.contextSlots)
        {
            std::map<const Value *, unsigned> alternativeSlots;
            long long payloadSavings = 0;
            for (const Value *value : pipeline.contextValues)
            {
                unsigned slots = alternativeRing;
                const auto interval = pipeline.valueLiveIntervals.find(value);
                if (pipeline.retirementOnlyValues.count(value))
                    slots = 1;
                else if (interval != pipeline.valueLiveIntervals.end())
                {
                    const unsigned required = std::max(1,
                        (interval->second.second - interval->second.first + pipeline.achievedII - 1) /
                        pipeline.achievedII);
                    for (unsigned candidate = required; candidate < alternativeRing; ++candidate)
                        if (alternativeRing % candidate == 0)
                        {
                            slots = candidate;
                            break;
                        }
                }
                alternativeSlots[value] = slots;
                payloadSavings += (static_cast<long long>(pipelineValueStorageSlots(pipeline, value)) -
                    slots) * value->getType()->getIntegerBitWidth();
            }
            unsigned iterationWidth = 1;
            for (unsigned long long maximum = pipeline.tripCount - 1; maximum > 1; maximum >>= 1)
                ++iterationWidth;
            // A power-of-two ring may allow smaller per-value divisors. Only
            // expand it when payload savings exceed the additional index bits.
            // This is structural accounting, not a prediction of LUTRAM/FF QoR.
            const long long extraIndexBits = pipeline.derivedValues.empty() ? 0 :
                static_cast<long long>(alternativeRing - pipeline.contextSlots) * iterationWidth;
            if (payloadSavings > extraIndexBits)
            {
                pipeline.contextSlots = alternativeRing;
                pipeline.valueStorageSlots = std::move(alternativeSlots);
            }
        }
        for (const ScalarRecurrenceDependence &dependence : pipeline.scalarRecurrences)
        {
            if (dependence.exact && dependence.distance == 1 && dependence.source && dependence.phi &&
                pipeline.retirementOnlyValues.count(dependence.source) &&
                pipeline.contextValues.count(dependence.phi) &&
                dependence.source->getType() == dependence.phi->getType() &&
                pipelineValueStorageSlots(pipeline, dependence.source) == 1 &&
                pipelineValueStorageSlots(pipeline, dependence.phi) == 1)
                // Both receive the same producer expression on the same edge.
                // Only the PHI needs initialization; exported results are read
                // after the final producer update, never during initialization.
                pipeline.storageAliases[dependence.source] = dependence.phi;
        }
        // With only scalar storage, iteration tags can shift through fixed
        // stages. This removes dynamic ring selection from the address path.
        pipeline.stageIndexedController =
            pipeline.achievedII >= 1 &&
            std::all_of(pipeline.contextValues.begin(), pipeline.contextValues.end(),
                        [&](const Value *value)
                        { return pipelineValueStorageSlots(pipeline, value) == 1; }) &&
            std::all_of(pipeline.scalarRecurrences.begin(), pipeline.scalarRecurrences.end(),
                        [](const ScalarRecurrenceDependence &dependence)
                        { return dependence.exact; });
        pipeline.lastTokenController = false;
        pipeline.iterationTagLastStage = static_cast<int>(pipeline.pipelineLatency);
        if (pipeline.stageIndexedController)
        {
            int lastUse = -1;
            std::function<void(const Value *, int, std::set<const Value *> &)> sample =
                [&](const Value *value, int cycle, std::set<const Value *> &visited) {
                    if (!visited.insert(value).second)
                        return;
                    if (pipeline.derivedValues.count(value))
                    {
                        lastUse = std::max(lastUse, cycle);
                        return;
                    }
                    const auto *instruction = dyn_cast<Instruction>(value);
                    if (instruction && instruction->getParent() == pipeline.header &&
                        (context->combinationalValues.count(instruction) ||
                         instruction->getType()->isPointerTy() ||
                         isa<PtrToIntInst>(instruction) || isa<IntToPtrInst>(instruction)))
                        for (const Use &operand : instruction->operands())
                            sample(operand.get(), cycle, visited);
                };
            auto sampleValue = [&](const Value *value, int cycle) {
                std::set<const Value *> visited;
                sample(value, cycle, visited);
            };
            for (const auto &operation : pipeline.operationStartCycle)
            {
                for (const Use &operand : operation.first->operands())
                    sampleValue(operand.get(), operation.second);
                const auto access = context->memoryAccesses.find(operation.first);
                if (isa<LoadInst>(operation.first) && access != context->memoryAccesses.end())
                    for (const MemoryObject &memory : context->memoryObjects)
                        if (memory.name == access->second.memory && memory.banks > 1 &&
                            access->second.possibleBanks.size() != 1)
                            sampleValue(access->second.pointerOperand, operation.second + memory.readLatency);
            }
            for (const Value *value : pipeline.liveOutValues)
                sampleValue(value, pipeline.pipelineLatency);
            unsigned width = 1;
            for (unsigned maximum = pipeline.tripCount - 1; maximum > 1; maximum >>= 1)
                ++width;
            // A last-token bit replaces wide retirement-only tags only when
            // its complete shift chain costs fewer bits. Preserve address tags
            // through every request/response sample, including dynamic banks.
            if (lastUse < static_cast<int>(pipeline.pipelineLatency) &&
                (pipeline.pipelineLatency - lastUse) * width > pipeline.pipelineLatency + 1)
            {
                pipeline.lastTokenController = true;
                pipeline.iterationTagLastStage = lastUse;
            }
        }
    }
    context->schedule.pipelines = context->pipelineRegions;
    return false;
}

void HI_PipelineStorageAnalysis::getAnalysisUsage(AnalysisUsage &usage) const
{
    usage.setPreservesAll();
}
