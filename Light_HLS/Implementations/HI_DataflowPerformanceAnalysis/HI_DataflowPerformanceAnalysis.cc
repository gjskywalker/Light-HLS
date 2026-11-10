#include "HI_DataflowPerformanceAnalysis.h"

#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/TargetParser/Triple.h"

#include <algorithm>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;
using namespace light_hls::backend;

namespace
{

const DataflowTaskImplementation *findImplementation(
    const HLSBackendContext &context, const std::string &task)
{
    for (const DataflowTaskImplementation &implementation :
         context.dataflowImplementations)
    {
        if (implementation.task == task)
            return &implementation;
    }
    return nullptr;
}

const DataflowTaskPortAccess *findPortAccess(
    const DataflowTaskImplementation &implementation, unsigned argumentIndex)
{
    for (const DataflowTaskPortAccess &port : implementation.portAccesses)
    {
        if (port.argumentIndex == argumentIndex)
            return &port;
    }
    return nullptr;
}

bool channelArgumentIndex(const DataflowTask &task, const Value *base,
                          unsigned &argumentIndex)
{
    if (!task.call)
        return false;
    for (unsigned index = 0; index < task.call->arg_size(); ++index)
    {
        if (getUnderlyingObject(task.call->getArgOperand(index)) == base)
        {
            argumentIndex = index;
            return true;
        }
    }
    return false;
}

unsigned long long dynamicExecutionCount(const Instruction &instruction,
                                         LoopInfo &loopInfo,
                                         ScalarEvolution &scalarEvolution,
                                         bool &known)
{
    unsigned long long count = 1;
    for (const Loop *loop = loopInfo.getLoopFor(instruction.getParent()); loop;
         loop = loop->getParentLoop())
    {
        unsigned tripCount = scalarEvolution.getSmallConstantTripCount(loop);
        if (tripCount == 0)
            tripCount = scalarEvolution.getSmallConstantMaxTripCount(loop);
        if (tripCount == 0 ||
            count > std::numeric_limits<unsigned long long>::max() /
                        tripCount)
        {
            known = false;
            return 0;
        }
        count *= tripCount;
    }
    return count;
}

bool isLoopTripBranch(const BranchInst &branch, const Instruction &instruction,
                      LoopInfo &loopInfo)
{
    if (!branch.isConditional())
        return false;
    Loop *loop = loopInfo.getLoopFor(branch.getParent());
    if (!loop || branch.getParent() != loop->getHeader() ||
        !loop->contains(instruction.getParent()))
    {
        return false;
    }
    const bool firstInside = loop->contains(branch.getSuccessor(0));
    const bool secondInside = loop->contains(branch.getSuccessor(1));
    return firstInside != secondInside;
}

bool isUnconditionallyExecuted(const Instruction &instruction,
                               LoopInfo &loopInfo,
                               DominatorTree &dominatorTree,
                               PostDominatorTree &postDominatorTree)
{
    const Function *function = instruction.getFunction();
    if (!function)
        return false;
    for (const BasicBlock &candidate : *function)
    {
        const Instruction *terminator = candidate.getTerminator();
        const auto *branch = dyn_cast<BranchInst>(terminator);
        const bool conditional =
            (branch && branch->isConditional()) || isa<SwitchInst>(terminator);
        if (!conditional ||
            !dominatorTree.dominates(&candidate, instruction.getParent()) ||
            postDominatorTree.dominates(instruction.getParent(), &candidate))
        {
            continue;
        }
        if (branch && isLoopTripBranch(*branch, instruction, loopInfo))
            continue;
        return false;
    }
    return true;
}

struct RateRange
{
    bool valid = true;
    bool exact = true;
    unsigned long long minimum = 0;
    unsigned long long maximum = 0;
};

struct RateAnalysis
{
    TargetLibraryInfoImpl libraryImpl;
    TargetLibraryInfo library;
    AssumptionCache assumptions;
    DominatorTree dominators;
    LoopInfo loops;
    PostDominatorTree postDominators;
    ScalarEvolution evolution;

    explicit RateAnalysis(Function &function)
        : libraryImpl(Triple(function.getParent()->getTargetTriple())),
          library(libraryImpl), assumptions(function), dominators(function),
          loops(dominators), postDominators(function),
          evolution(function, library, assumptions, dominators, loops)
    {
    }
};

using RateAnalyses = std::map<Function *, std::unique_ptr<RateAnalysis>>;

RateRange accessRate(const std::vector<const Instruction *> &instructions,
                     RateAnalyses &analyses)
{
    RateRange range;
    for (const Instruction *instruction : instructions)
    {
        if (!instruction || !instruction->getFunction())
        {
            range.valid = false;
            continue;
        }
        Function &function =
            *const_cast<Function *>(instruction->getFunction());
        // Own a coherent, immutable analysis snapshot per function. Nested
        // task pass managers must not invalidate borrowed legacy analyses.
        auto &owned = analyses[&function];
        if (!owned)
            owned = std::make_unique<RateAnalysis>(function);
        LoopInfo &loopInfo = owned->loops;
        ScalarEvolution &scalarEvolution = owned->evolution;
        DominatorTree &dominatorTree = owned->dominators;
        PostDominatorTree &postDominatorTree = owned->postDominators;
        const bool unconditional = isUnconditionallyExecuted(
            *instruction, loopInfo, dominatorTree, postDominatorTree);
        bool countKnown = true;
        const unsigned long long executions = dynamicExecutionCount(
            *instruction, loopInfo, scalarEvolution, countKnown);
        if (!countKnown ||
            range.maximum >
                std::numeric_limits<unsigned long long>::max() - executions ||
            (unconditional &&
             range.minimum >
                 std::numeric_limits<unsigned long long>::max() - executions))
        {
            range.valid = false;
            return range;
        }
        range.maximum += executions;
        if (unconditional)
            range.minimum += executions;
        else
            range.exact = false;
    }
    return range;
}

struct TokenEvent
{
    unsigned channel = 0;
    bool write = false;
    unsigned long long count = 0;
    long long firstCycle = -1;
    unsigned interval = 0;
};

struct TokenTaskState
{
    const DataflowTask *task = nullptr;
    long long localCycle = 1;
    bool done = false;
    bool terminal = false;
    std::vector<TokenEvent> events;
};

bool tokenEventActive(const TokenEvent &event, long long localCycle)
{
    if (event.count == 0 || event.firstCycle < 0 ||
        localCycle < event.firstCycle)
    {
        return false;
    }
    if (event.count == 1)
        return localCycle == event.firstCycle;
    if (event.interval == 0)
        return false;
    const unsigned long long elapsed =
        static_cast<unsigned long long>(localCycle - event.firstCycle);
    return elapsed % event.interval == 0 &&
           elapsed / event.interval < event.count;
}

bool appendEndpointEvent(const HLSBackendContext &context,
                         const DataflowTask &task,
                         const DataflowChannel &channel,
                         unsigned channelIndex, bool write,
                         TokenTaskState &state)
{
    const DataflowTaskImplementation *implementation =
        findImplementation(context, task.name);
    unsigned argumentIndex = 0;
    if (!implementation ||
        !channelArgumentIndex(task, channel.base, argumentIndex))
    {
        return false;
    }
    const DataflowTaskPortAccess *access =
        findPortAccess(*implementation, argumentIndex);
    if (!access)
        return false;

    TokenEvent event;
    event.channel = channelIndex;
    event.write = write;
    event.count =
        write ? channel.productionRate : channel.consumptionRate;
    event.firstCycle =
        write ? access->firstWriteCycle : access->firstReadCycle;
    event.interval =
        write ? access->writeInterval : access->readInterval;
    const bool exact =
        write ? access->writeTimingExact : access->readTimingExact;
    if (!exact || event.firstCycle <= 0 ||
        (event.count > 1 && event.interval == 0))
    {
        return false;
    }
    state.events.push_back(event);
    return true;
}

bool simulateFIFOTransaction(const HLSBackendContext &context,
                             const DataflowGraph &graph,
                             long long &latency)
{
    std::map<std::string, unsigned> taskIndices;
    const auto completionTasks = dataflowCompletionTasks(graph);
    std::vector<TokenTaskState> states(graph.tasks.size());
    unsigned long long maximumProgressCycles = 0;
    for (unsigned index = 0; index < graph.tasks.size(); ++index)
    {
        const DataflowTask &task = graph.tasks[index];
        if (!task.latencyExact || task.latencyCycles <= 0 ||
            maximumProgressCycles >
                std::numeric_limits<unsigned long long>::max() -
                    static_cast<unsigned long long>(task.latencyCycles))
        {
            return false;
        }
        taskIndices[task.name] = index;
        states[index].task = &task;
        maximumProgressCycles +=
            static_cast<unsigned long long>(task.latencyCycles);
    }

    std::vector<unsigned long long> fifoCounts;
    fifoCounts.reserve(graph.channels.size());
    for (unsigned index = 0; index < graph.channels.size(); ++index)
    {
        const DataflowChannel &channel = graph.channels[index];
        if (channel.type != "fifo" || channel.instances != 1 ||
            !channel.rateKnown ||
            channel.productionRate != channel.consumptionRate ||
            channel.depth == 0 || channel.initialTokens > channel.depth)
        {
            return false;
        }
        auto producer = taskIndices.find(channel.producer);
        auto consumer = taskIndices.find(channel.consumer);
        if (producer == taskIndices.end() || consumer == taskIndices.end() ||
            !appendEndpointEvent(context, graph.tasks[producer->second],
                                 channel, index, true,
                                 states[producer->second]) ||
            !appendEndpointEvent(context, graph.tasks[consumer->second],
                                 channel, index, false,
                                 states[consumer->second]))
        {
            return false;
        }
        fifoCounts.push_back(channel.initialTokens);
    }
    for (TokenTaskState &state : states)
    {
        state.terminal = completionTasks.count(state.task->name) != 0;
    }

    for (unsigned long long globalCycle = 1;
         globalCycle <= maximumProgressCycles; ++globalCycle)
    {
        std::vector<bool> enabled(states.size(), true);
        for (unsigned taskIndex = 0; taskIndex < states.size(); ++taskIndex)
        {
            const TokenTaskState &state = states[taskIndex];
            if (state.done)
                continue;
            for (const TokenEvent &event : state.events)
            {
                if (!tokenEventActive(event, state.localCycle))
                    continue;
                const DataflowChannel &channel =
                    graph.channels[event.channel];
                if ((event.write &&
                     fifoCounts[event.channel] == channel.depth) ||
                    (!event.write && fifoCounts[event.channel] == 0))
                {
                    enabled[taskIndex] = false;
                    break;
                }
            }
        }

        std::vector<long long> countChanges(graph.channels.size(), 0);
        bool madeProgress = false;
        for (unsigned taskIndex = 0; taskIndex < states.size(); ++taskIndex)
        {
            TokenTaskState &state = states[taskIndex];
            if (state.done || !enabled[taskIndex])
                continue;
            for (const TokenEvent &event : state.events)
            {
                if (tokenEventActive(event, state.localCycle))
                    countChanges[event.channel] += event.write ? 1 : -1;
            }
            madeProgress = true;
            if (state.localCycle == state.task->latencyCycles)
                state.done = true;
            else
                ++state.localCycle;
        }
        for (unsigned index = 0; index < fifoCounts.size(); ++index)
        {
            const long long nextCount =
                static_cast<long long>(fifoCounts[index]) +
                countChanges[index];
            if (nextCount < 0 ||
                nextCount >
                    static_cast<long long>(graph.channels[index].depth))
            {
                return false;
            }
            fifoCounts[index] = static_cast<unsigned long long>(nextCount);
        }

        const bool terminalsDone = std::all_of(
            states.begin(), states.end(), [](const TokenTaskState &state) {
                return !state.terminal || state.done;
            });
        if (terminalsDone)
        {
            if (globalCycle > static_cast<unsigned long long>(
                                  std::numeric_limits<long long>::max()) -
                                  graph.retirementCycles)
            {
                return false;
            }
            latency = static_cast<long long>(globalCycle) +
                      graph.retirementCycles;
            return true;
        }
        if (!madeProgress)
            return false;
    }
    return false;
}

} // namespace

char HI_DataflowPerformanceAnalysis::ID = 0;

HI_DataflowPerformanceAnalysis::HI_DataflowPerformanceAnalysis(
    HLSBackendContextPtr context)
    : ModulePass(ID), context(std::move(context))
{
}

bool HI_DataflowPerformanceAnalysis::runOnModule(Module &module)
{
    if (!context || context->hasErrors() || !context->rtl.dataflow.valid)
        return false;

    context->module = &module;
    DataflowGraph &graph = context->rtl.dataflow;
    std::map<std::string, DataflowTask *> tasks;
    unsigned graphII = 0;
    bool allTaskIntervalsKnown = true;
    for (DataflowTask &task : graph.tasks)
    {
        tasks[task.name] = &task;
        const DataflowTaskImplementation *implementation =
            findImplementation(*context, task.name);
        if (!implementation)
        {
            allTaskIntervalsKnown = false;
            continue;
        }
        task.latencyCycles = implementation->latencyCycles;
        task.latencySource = implementation->latencySource;
        task.latencyExact = implementation->latencyExact;
        task.initiationInterval = implementation->initiationInterval;
        task.pipelineInitiationInterval =
            implementation->pipelineInitiationInterval;
        task.pipelineCount = implementation->pipelineCount;
        if (task.initiationInterval == 0 && task.latencyCycles > 0)
            task.initiationInterval =
                static_cast<unsigned>(task.latencyCycles);
        if (task.initiationInterval == 0)
            allTaskIntervalsKnown = false;
        else
            graphII = std::max(graphII, task.initiationInterval);
    }
    if (!allTaskIntervalsKnown && context->estimate.initiationInterval > 0)
    {
        graphII = static_cast<unsigned>(context->estimate.initiationInterval);
        graph.graphIISource = "top-level evaluator fallback";
    }
    else
    {
        graph.graphIISource =
            "maximum generated task reacceptance interval";
    }
    graph.graphInitiationInterval = graphII;
    graph.executionModel = "persistent bounded task queue";
    graph.maximumOutstandingTransactions = 2;
    graph.minimumAcceptanceInterval = 1;
    graph.pipoHandoffCycles = 1;
    // Child done is registered after its effects commit. The wrapper exposes
    // the terminal completion predicate without a second output register.
    graph.retirementCycles = 0;

    bool allRatesKnown = true;
    RateAnalyses rateAnalyses;
    bool ratesBalanced = true;
    bool capacitiesSafe = true;
    for (DataflowChannel &channel : graph.channels)
    {
        DataflowTask *producer = tasks[channel.producer];
        DataflowTask *consumer = tasks[channel.consumer];
        const DataflowTaskImplementation *producerImplementation =
            producer ? findImplementation(*context, producer->name) : nullptr;
        const DataflowTaskImplementation *consumerImplementation =
            consumer ? findImplementation(*context, consumer->name) : nullptr;
        unsigned producerArgument = 0;
        unsigned consumerArgument = 0;
        const bool endpointsKnown =
            producer && consumer && producerImplementation &&
            consumerImplementation &&
            channelArgumentIndex(*producer, channel.base, producerArgument) &&
            channelArgumentIndex(*consumer, channel.base, consumerArgument);
        const DataflowTaskPortAccess *producerAccess =
            endpointsKnown
                ? findPortAccess(*producerImplementation, producerArgument)
                : nullptr;
        const DataflowTaskPortAccess *consumerAccess =
            endpointsKnown
                ? findPortAccess(*consumerImplementation, consumerArgument)
                : nullptr;
        channel.producerFirstAccessCycle =
            producerAccess ? producerAccess->firstWriteCycle : -1;
        channel.consumerFirstAccessCycle =
            consumerAccess ? consumerAccess->firstReadCycle : -1;
        channel.producerAccessInterval =
            producerAccess ? producerAccess->writeInterval : 0;
        channel.consumerAccessInterval =
            consumerAccess ? consumerAccess->readInterval : 0;
        channel.producerAccessTimingExact =
            producerAccess && producerAccess->writeTimingExact;
        channel.consumerAccessTimingExact =
            consumerAccess && consumerAccess->readTimingExact;
        const RateRange production =
            producerAccess ? accessRate(producerAccess->writes, rateAnalyses)
                           : RateRange{false, false, 0, 0};
        const RateRange consumption =
            consumerAccess ? accessRate(consumerAccess->reads, rateAnalyses)
                           : RateRange{false, false, 0, 0};
        channel.minimumProductionRate = production.minimum;
        channel.maximumProductionRate = production.maximum;
        channel.minimumConsumptionRate = consumption.minimum;
        channel.maximumConsumptionRate = consumption.maximum;
        channel.productionRate = production.maximum;
        channel.consumptionRate = consumption.maximum;
        channel.rateBoundsKnown = production.valid && consumption.valid &&
                                  production.maximum > 0 &&
                                  consumption.maximum > 0;
        channel.rateKnown = channel.rateBoundsKnown && production.exact &&
                            consumption.exact;
        channel.runtimeRateCheckRequired =
            channel.rateBoundsKnown && !channel.rateKnown;
        allRatesKnown &= channel.rateKnown;
        channel.bufferCount = channel.type == "pipo" ? 2 : 1;
        channel.minimumDepth = channel.type == "pipo"
                                   ? (channel.rateBoundsKnown
                                          ? static_cast<unsigned>(
                                                (channel.maximumProductionRate +
                                                 channel.instances - 1) /
                                                channel.instances)
                                          : channel.depth)
                                   : 1;
        if (!channel.rateKnown)
        {
            ratesBalanced = false;
            if (channel.type == "pipo")
            {
                context->addError(
                    "BACKEND_DATAFLOW_PIPO_RATE_UNKNOWN",
                    "PIPO channel '" + channel.name +
                        "' requires exact static producer and consumer rates; conditional transfers must use a FIFO channel.",
                    channel.name);
            }
            else
            {
                context->addWarning(
                    "BACKEND_DATAFLOW_RATE_UNKNOWN",
                    "Cannot prove the transaction rate of FIFO channel '" +
                        channel.name +
                        "'; RTL backpressure remains functional but graph-II validation requires a runtime rate contract.",
                    channel.name);
            }
        }
        else if (channel.productionRate != channel.consumptionRate)
        {
            ratesBalanced = false;
            context->addError(
                "BACKEND_DATAFLOW_RATE_MISMATCH",
                "Channel '" + channel.name + "' produces " +
                    std::to_string(channel.productionRate) +
                    " item(s) but consumes " +
                    std::to_string(channel.consumptionRate) +
                    " item(s) per top-level transaction.",
                channel.name);
        }
        if (channel.type == "pipo")
        {
            const unsigned long long volume =
                static_cast<unsigned long long>(channel.depth) *
                channel.instances;
            channel.capacitySafe =
                channel.rateKnown &&
                channel.productionRate == channel.consumptionRate &&
                channel.productionRate <= volume;
            if (channel.rateKnown && !channel.capacitySafe)
            {
                context->addError(
                    "BACKEND_DATAFLOW_PIPO_VOLUME_MISMATCH",
                    "PIPO channel '" + channel.name + "' has a " +
                        std::to_string(volume) +
                        "-element capacity but its task transaction transfers " +
                        std::to_string(channel.productionRate) + " producer and " +
                        std::to_string(channel.consumptionRate) +
                        " consumer element(s); matching partial transfers up to the capacity are supported.",
                    channel.name);
            }
        }
        else
        {
            channel.capacitySafe = channel.depth >= channel.minimumDepth;
        }
        capacitiesSafe &= channel.capacitySafe;
    }

    std::map<std::string, unsigned> taskIndex;
    for (unsigned index = 0; index < graph.tasks.size(); ++index)
        taskIndex[graph.tasks[index].name] = index;
    auto topologicalOrder = [&](bool ignoreInitializedEdges) {
        std::vector<std::vector<unsigned>> successors(graph.tasks.size());
        std::vector<unsigned> indegree(graph.tasks.size(), 0);
        for (const DataflowChannel &channel : graph.channels)
        {
            if (ignoreInitializedEdges && channel.initialTokens != 0)
                continue;
            const unsigned producer = taskIndex[channel.producer];
            const unsigned consumer = taskIndex[channel.consumer];
            successors[producer].push_back(consumer);
            ++indegree[consumer];
        }
        std::queue<unsigned> ready;
        for (unsigned index = 0; index < indegree.size(); ++index)
        {
            if (indegree[index] == 0)
                ready.push(index);
        }
        std::vector<std::string> order;
        while (!ready.empty())
        {
            const unsigned task = ready.front();
            ready.pop();
            order.push_back(graph.tasks[task].name);
            for (unsigned successor : successors[task])
            {
                if (--indegree[successor] == 0)
                    ready.push(successor);
            }
        }
        return order;
    };
    const std::vector<std::string> fullOrder = topologicalOrder(false);
    graph.cyclic = fullOrder.size() != graph.tasks.size();
    graph.topologicalOrder = topologicalOrder(true);
    graph.zeroTokenCycle =
        graph.topologicalOrder.size() != graph.tasks.size();
    if (graph.zeroTokenCycle)
    {
        context->addError(
            "BACKEND_DATAFLOW_ZERO_TOKEN_CYCLE",
            "The DATAFLOW graph contains a directed cycle whose edges all have zero initial tokens and can deadlock at startup.",
            context->configuration.topFunction);
    }
    graph.ratesBalanced = allRatesKnown && ratesBalanced;
    graph.deadlockFree = !graph.zeroTokenCycle && ratesBalanced && capacitiesSafe;

    bool allTaskLatenciesKnown = true;
    bool allTaskLatenciesExact = true;
    bool allChannelsPIPO = true;
    bool externalStreamBackpressure = false;
    for (const Argument &argument : context->top->args())
    {
        if (!argument.hasName())
            continue;
        for (const StreamDirective &stream : context->configuration.streams)
        {
            if (stream.scope == context->configuration.topFunction &&
                stream.variable == argument.getName().str())
            {
                externalStreamBackpressure = true;
                break;
            }
        }
    }
    unsigned long long serialLatency = graph.retirementCycles;
    long long maximumTaskLatency = 0;
    const auto completionTasks = dataflowCompletionTasks(graph);
    for (const DataflowTask &task : graph.tasks)
    {
        if (task.latencyCycles <= 0)
        {
            allTaskLatenciesKnown = false;
            continue;
        }
        allTaskLatenciesExact &= task.latencyExact;
        maximumTaskLatency =
            std::max(maximumTaskLatency, task.latencyCycles);
        if (serialLatency >
            std::numeric_limits<unsigned long long>::max() -
                static_cast<unsigned long long>(task.latencyCycles))
        {
            allTaskLatenciesKnown = false;
            continue;
        }
        serialLatency += static_cast<unsigned long long>(task.latencyCycles);
    }
    for (const DataflowChannel &channel : graph.channels)
    {
        allChannelsPIPO &= channel.type == "pipo";
        if (channel.type == "pipo")
        {
            if (serialLatency >
                std::numeric_limits<unsigned long long>::max() -
                    graph.pipoHandoffCycles)
            {
                allTaskLatenciesKnown = false;
            }
            else
            {
                serialLatency += graph.pipoHandoffCycles;
            }
        }
    }

    long long tokenSimulatedLatency = -1;
    const bool tokenSimulationExact =
        !externalStreamBackpressure &&
        simulateFIFOTransaction(*context, graph, tokenSimulatedLatency);
    if (tokenSimulationExact)
    {
        graph.firstTransactionLatencyLowerBound = tokenSimulatedLatency;
        graph.firstTransactionLatencyUpperBound = tokenSimulatedLatency;
        graph.firstTransactionLatencyExact = true;
        graph.firstTransactionLatencySource =
            "generated task FSM access cycles plus bounded FIFO token simulation and retirement";
    }
    else if (allTaskLatenciesKnown && !graph.cyclic)
    {
        std::map<std::string, long long> pipoFinish;
        for (const std::string &taskName : graph.topologicalOrder)
        {
            long long start = 0;
            for (const DataflowChannel &channel : graph.channels)
            {
                if (channel.consumer != taskName || channel.type != "pipo")
                    continue;
                auto predecessor = pipoFinish.find(channel.producer);
                if (predecessor != pipoFinish.end())
                {
                    start = std::max(
                        start,
                        predecessor->second +
                            static_cast<long long>(graph.pipoHandoffCycles));
                }
            }
            pipoFinish[taskName] =
                start + tasks.at(taskName)->latencyCycles;
        }
        long long constrainedLatency = maximumTaskLatency;
        for (const auto &finish : pipoFinish)
        {
            if (completionTasks.count(finish.first))
                constrainedLatency =
                    std::max(constrainedLatency, finish.second);
        }
        graph.firstTransactionLatencyLowerBound =
            constrainedLatency + graph.retirementCycles;
        if (allChannelsPIPO && allTaskLatenciesExact)
        {
            graph.firstTransactionLatencyUpperBound =
                graph.firstTransactionLatencyLowerBound;
            graph.firstTransactionLatencyExact = true;
            graph.firstTransactionLatencySource =
                "generated task FSMs plus PIPO handoff and retirement";
        }
        else if (!externalStreamBackpressure &&
                 serialLatency <= static_cast<unsigned long long>(
                                      std::numeric_limits<long long>::max()))
        {
            graph.firstTransactionLatencyUpperBound =
                static_cast<long long>(serialLatency);
            graph.firstTransactionLatencySource =
                "controller-aware structural bounds; FIFO overlap is not cycle-exact and the upper bound assumes no external stalls";
        }
        else if (externalStreamBackpressure)
        {
            graph.firstTransactionLatencySource =
                "structural lower bound only; public ap_fifo backpressure is externally controlled";
        }
    }
    else
    {
        graph.firstTransactionLatencySource =
            graph.cyclic
                ? "cyclic graph requires token-level cycle simulation"
                : "one or more generated task latencies are unknown";
    }
    graph.performanceValid = graph.ratesBalanced && graph.deadlockFree &&
                             graph.graphInitiationInterval > 0;
    return false;
}

void HI_DataflowPerformanceAnalysis::getAnalysisUsage(
    AnalysisUsage &usage) const
{
    usage.setPreservesAll();
}
