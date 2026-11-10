#include "HI_PipelineValueLifetime.h"

#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Instructions.h"

#include <vector>

using namespace llvm;

namespace light_hls::backend
{

bool pipelineSamplingCycles(const HLSBackendContext &context, const LoopPipelineInfo &pipeline,
                            const Value *value,
                            const std::map<const Instruction *, int> &startCycles,
                            std::set<int> &cycles, bool includeRetirement, bool *retirementOnly)
{
    if (retirementOnly)
        *retirementOnly = true;
    std::vector<const Value *> worklist{value};
    std::set<const Value *> visited;
    while (!worklist.empty())
    {
        const Value *current = worklist.back();
        worklist.pop_back();
        if (!visited.insert(current).second)
            continue;
        for (const User *user : current->users())
        {
            const auto *consumer = dyn_cast<Instruction>(user);
            if (consumer && isa<DbgInfoIntrinsic>(consumer))
                continue;
            if (!consumer)
                return false;
            if (consumer->getParent() != pipeline.header)
            {
                if (includeRetirement && (consumer->getParent() == pipeline.exit ||
                                          pipeline.liveOutValues.count(current)))
                {
                    cycles.insert(pipeline.pipelineLatency);
                    continue;
                }
                return false;
            }
            if (isa<PHINode>(consumer))
            {
                bool directFeedback = false;
                for (const ScalarRecurrenceDependence &dependence : pipeline.scalarRecurrences)
                    directFeedback |= dependence.exact && dependence.source == current &&
                                      dependence.phi == consumer;
                // Feedback is emitted from the producer expression on its
                // write edge, not by reading the producer's stored result.
                if (includeRetirement && directFeedback)
                    continue;
                return false;
            }
            if (consumer->isTerminator())
                return false;
            const auto start = startCycles.find(consumer);
            // Instructions outside the hardware slice have no sampling edge.
            if (start == startCycles.end())
                continue;
            if (context.combinationalValues.count(consumer) || consumer->getType()->isPointerTy())
            {
                worklist.push_back(consumer);
                continue;
            }
            cycles.insert(start->second);
            if (retirementOnly)
                *retirementOnly = false;
            // A dynamically banked load also uses its address at response
            // capture to select the correct bank's output.
            const auto access = context.memoryAccesses.find(consumer);
            if (isa<LoadInst>(consumer) && access != context.memoryAccesses.end())
            {
                for (const MemoryObject &memory : context.memoryObjects)
                    if (memory.name == access->second.memory && memory.banks > 1 &&
                        access->second.possibleBanks.size() != 1)
                        cycles.insert(start->second + memory.readLatency);
            }
        }
    }
    return !cycles.empty();
}

unsigned pipelineValueStorageSlots(const LoopPipelineInfo &pipeline, const Value *value)
{
    const auto found = pipeline.valueStorageSlots.find(value);
    return found == pipeline.valueStorageSlots.end() ? pipeline.contextSlots : found->second;
}

} // namespace light_hls::backend
