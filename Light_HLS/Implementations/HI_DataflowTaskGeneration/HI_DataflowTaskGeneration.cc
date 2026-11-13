#include "HI_DataflowTaskGeneration.h"

#include "HI_ArrayPartitionAnalysis.h"
#include "HI_BackendDependenceAnalysis.h"
#include "HI_BackendValidation.h"
#include "HI_LoopPipelineAnalysis.h"
#include "HI_MemoryObjectAnalysis.h"
#include "HI_OperationScheduling.h"
#include "HI_PipelineStorageAnalysis.h"
#include "HI_RTLGeneration.h"
#include "HI_ResourceBinding.h"
#include "HI_StorageBindingAnalysis.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LegacyPassManager.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;
using namespace light_hls::backend;

namespace
{

const DataflowChannel *findChannel(const DataflowGraph &graph,
                                   const Value *base)
{
    for (const DataflowChannel &channel : graph.channels)
    {
        if (channel.base == base)
            return &channel;
    }
    return nullptr;
}

unsigned long long externalDepth(const HLSBackendContext &context,
                                 const Value *base)
{
    const auto *argument = dyn_cast<Argument>(base);
    if (!argument)
        return 0;
    const std::string name = argument->hasName()
                                 ? argument->getName().str()
                                 : "arg" + std::to_string(argument->getArgNo());
    auto frontend = context.configuration.frontendMemoryDepths.find(name);
    if (frontend != context.configuration.frontendMemoryDepths.end())
        return frontend->second;
    auto configured = context.configuration.configuredMemoryDepths.find(name);
    return configured == context.configuration.configuredMemoryDepths.end()
               ? 0
               : configured->second;
}

std::string sourceFunctionName(const DataflowTask &task)
{
    const std::size_t suffix = task.name.rfind('_');
    if (suffix == std::string::npos)
        return task.name;
    return task.name.substr(0, suffix);
}

bool multiplyChecked(unsigned long long &value, unsigned long long factor)
{
    if (factor != 0 &&
        value > std::numeric_limits<unsigned long long>::max() / factor)
    {
        return false;
    }
    value *= factor;
    return true;
}

bool addChecked(unsigned long long &value, unsigned long long increment)
{
    if (value > std::numeric_limits<unsigned long long>::max() - increment)
        return false;
    value += increment;
    return true;
}

bool isStaticLoopExit(const BranchInst &branch, LoopInfo &loopInfo)
{
    if (!branch.isConditional())
        return false;
    Loop *loop = loopInfo.getLoopFor(branch.getParent());
    if (!loop)
        return false;
    const bool firstInside = loop->contains(branch.getSuccessor(0));
    const bool secondInside = loop->contains(branch.getSuccessor(1));
    return firstInside != secondInside;
}

const BlockSchedule *findBlockSchedule(const ScheduleResult &schedule,
                                       const BasicBlock *block)
{
    for (const BlockSchedule &candidate : schedule.blocks)
    {
        if (candidate.block == block)
            return &candidate;
    }
    return nullptr;
}

bool uniquePrefixLatency(const Function &function,
                         const BasicBlock *destination,
                         const ScheduleResult &schedule,
                         unsigned long long &latency)
{
    latency = 0;
    const BasicBlock *block = &function.getEntryBlock();
    std::set<const BasicBlock *> visited;
    while (block != destination)
    {
        if (!visited.insert(block).second)
            return false;
        const BlockSchedule *blockSchedule =
            findBlockSchedule(schedule, block);
        if (!blockSchedule || blockSchedule->pipelined)
            return false;
        const unsigned long long blockLatency =
            blockSchedule->executionLatency > 0
                ? static_cast<unsigned long long>(
                      blockSchedule->executionLatency)
                : blockSchedule->stateCount;
        if (!addChecked(latency, blockLatency))
            return false;
        const auto *branch = dyn_cast<BranchInst>(block->getTerminator());
        if (!branch || !branch->isUnconditional())
            return false;
        block = branch->getSuccessor(0);
    }
    return true;
}

bool regularAccessTiming(const Instruction &instruction,
                         const ScheduleResult &schedule,
                         LoopInfo &loopInfo,
                         ScalarEvolution &scalarEvolution,
                         long long &firstCycle, unsigned &interval)
{
    const BlockSchedule *block =
        findBlockSchedule(schedule, instruction.getParent());
    auto scheduled = schedule.instructionMap.find(&instruction);
    if (!block || scheduled == schedule.instructionMap.end() ||
        !scheduled->second || scheduled->second->startCycle < 0)
    {
        return false;
    }

    Loop *loop = loopInfo.getLoopFor(instruction.getParent());
    if (loop)
    {
        const auto pipeline = std::find_if(
            schedule.pipelines.begin(), schedule.pipelines.end(),
            [&](const LoopPipelineInfo &candidate) {
                return candidate.header == instruction.getParent();
            });
        const unsigned foldedLevels = pipeline == schedule.pipelines.end()
                                          ? 1 : pipeline->flattenedLoopCount;
        const Loop *hardwareParent = loop;
        for (unsigned level = 0; level < foldedLevels && hardwareParent; ++level)
            hardwareParent = hardwareParent->getParentLoop();
        if (hardwareParent || loop->getHeader() != instruction.getParent() ||
            scalarEvolution.getSmallConstantTripCount(loop) == 0)
        {
            return false;
        }
        if (block->pipelined)
        {
            if (pipeline == schedule.pipelines.end() ||
                pipeline->achievedII <= 0)
            {
                return false;
            }
            interval = static_cast<unsigned>(pipeline->achievedII);
        }
        else
        {
            if (block->executionLatency <= 0 ||
                block->executionLatency >
                    static_cast<long long>(
                        std::numeric_limits<unsigned>::max()))
            {
                return false;
            }
            interval = static_cast<unsigned>(block->executionLatency);
        }
    }
    else
    {
        interval = 0;
    }

    unsigned long long prefix = 0;
    if (!uniquePrefixLatency(*instruction.getFunction(),
                             instruction.getParent(), schedule, prefix))
    {
        return false;
    }
    unsigned long long pipelineLaunchCycle = block->pipelined ? 2 : 1;
    for (const LoopPipelineInfo &pipeline : schedule.pipelines)
        if (pipeline.header == block->block && pipeline.initializedAtEntry)
            pipelineLaunchCycle = 1;
    if (!addChecked(
            prefix,
            static_cast<unsigned long long>(scheduled->second->startCycle)) ||
        !addChecked(prefix, pipelineLaunchCycle) ||
        prefix > static_cast<unsigned long long>(
                     std::numeric_limits<long long>::max()))
    {
        return false;
    }
    firstCycle = static_cast<long long>(prefix);
    return true;
}

void populateAccessTiming(DataflowTaskPortAccess &port,
                          const ScheduleResult &schedule,
                          LoopInfo &loopInfo,
                          ScalarEvolution &scalarEvolution)
{
    if (port.reads.size() == 1 && port.reads.front())
    {
        port.readTimingExact = regularAccessTiming(
            *port.reads.front(), schedule, loopInfo, scalarEvolution,
            port.firstReadCycle, port.readInterval);
    }
    if (port.writes.size() == 1 && port.writes.front())
    {
        port.writeTimingExact = regularAccessTiming(
            *port.writes.front(), schedule, loopInfo, scalarEvolution,
            port.firstWriteCycle, port.writeInterval);
    }
}

long long structuralTransactionLatency(const ScheduleResult &schedule,
                                       LoopInfo &loopInfo,
                                       ScalarEvolution &scalarEvolution,
                                       bool &exact)
{
    exact = true;
    unsigned long long latency = 0;
    std::map<const BasicBlock *, const BasicBlock *> hardwareExits;
    std::set<const Loop *> foldedLoops;
    for (const LoopPipelineInfo &pipeline : schedule.pipelines)
    {
        hardwareExits[pipeline.header] = pipeline.exit;
        const Loop *loop = loopInfo.getLoopFor(pipeline.header);
        for (unsigned level = 1; level < pipeline.flattenedLoopCount && loop; ++level)
        {
            loop = loop->getParentLoop();
            if (loop)
                foldedLoops.insert(loop);
        }
    }
    std::set<const BasicBlock *> reachable;
    std::vector<const BasicBlock *> worklist;
    if (!schedule.blocks.empty())
        worklist.push_back(schedule.blocks.front().block);
    while (!worklist.empty())
    {
        const BasicBlock *block = worklist.back();
        worklist.pop_back();
        if (!block || !reachable.insert(block).second)
            continue;
        const auto pipeline = hardwareExits.find(block);
        if (pipeline != hardwareExits.end())
            worklist.push_back(pipeline->second);
        else
            for (const BasicBlock *successor : successors(block))
                worklist.push_back(successor);
    }
    for (const BlockSchedule &block : schedule.blocks)
    {
        const unsigned long long blockLatency =
            block.executionLatency > 0
                ? static_cast<unsigned long long>(block.executionLatency)
                : block.stateCount;
        if (blockLatency == 0 || !block.block || !reachable.count(block.block))
            continue;

        unsigned long long executions = 1;
        Loop *loop = loopInfo.getLoopFor(block.block);
        if (block.pipelined && loop && loop->getHeader() == block.block)
            loop = loop->getParentLoop();
        for (; loop; loop = loop->getParentLoop())
        {
            // A folded outer loop contributes one hardware traversal. Its
            // original backedge and trip multiplier no longer describe RTL.
            if (foldedLoops.count(loop))
                continue;
            unsigned tripCount =
                scalarEvolution.getSmallConstantTripCount(loop);
            if (tripCount == 0)
            {
                tripCount =
                    scalarEvolution.getSmallConstantMaxTripCount(loop);
                exact = false;
            }
            if (tripCount == 0 || !multiplyChecked(executions, tripCount))
                return -1;
        }

        unsigned long long contribution = blockLatency;
        if (!multiplyChecked(contribution, executions) ||
            !addChecked(latency, contribution))
        {
            return -1;
        }

        const Instruction *terminator = block.block->getTerminator();
        if (const auto *branch = dyn_cast<BranchInst>(terminator))
        {
            if (branch->isConditional() &&
                !isStaticLoopExit(*branch, loopInfo))
                exact = false;
        }
        else if (isa<SwitchInst>(terminator))
        {
            exact = false;
        }
    }
    if (latency > static_cast<unsigned long long>(
                      std::numeric_limits<long long>::max()))
    {
        return -1;
    }
    return static_cast<long long>(latency);
}

void selectTaskPipelineDirectives(const DataflowTask &task,
                                  BackendConfiguration &configuration)
{
    const std::string labelPrefix =
        "Loop_" + sourceFunctionName(task) + "_";
    std::vector<std::pair<std::string, int>> selected;
    for (const auto &directive : configuration.pragmas.pipeline)
    {
        if (directive.first.compare(0, labelPrefix.size(), labelPrefix) == 0)
            selected.push_back(directive);
    }
    configuration.pragmas.pipeline = std::move(selected);
}

std::string valueName(const Value *value, const std::string &fallback)
{
    return value && value->hasName() ? value->getName().str() : fallback;
}

std::vector<unsigned long long> allocationDimensions(const Value *value)
{
    const auto *allocation = dyn_cast_or_null<AllocaInst>(value);
    if (!allocation)
        return {};
    const auto *arraySize = dyn_cast<ConstantInt>(allocation->getArraySize());
    if (!arraySize || arraySize->isZero())
        return {};
    std::vector<unsigned long long> dimensions;
    if (!arraySize->isOne())
        dimensions.push_back(arraySize->getZExtValue());
    const Type *type = allocation->getAllocatedType();
    while (const auto *array = dyn_cast<ArrayType>(type))
    {
        dimensions.push_back(array->getNumElements());
        type = array->getElementType();
    }
    if (dimensions.empty())
        dimensions.push_back(1);
    return dimensions;
}

void selectTaskMemoryDirectives(const HLSBackendContext &parent,
                                const DataflowTask &task,
                                unsigned argumentIndex, const Value *rootBase,
                                const std::string &argumentName,
                                BackendConfiguration &configuration)
{
    const std::string rootName =
        valueName(rootBase, "memory" + std::to_string(argumentIndex));
    const std::string taskScope = sourceFunctionName(task);
    for (const ArrayPartitionDirective &directive :
         parent.configuration.arrayPartitions)
    {
        const bool parentDirective =
            directive.scope == parent.configuration.topFunction &&
            directive.memory == rootName;
        const bool taskDirective = directive.scope == taskScope &&
                                   directive.memory == argumentName;
        if (!parentDirective && !taskDirective)
            continue;
        ArrayPartitionDirective selected = directive;
        selected.scope = configuration.topFunction;
        selected.memory = argumentName;
        configuration.arrayPartitions.push_back(std::move(selected));
    }
}

bool isAcyclicTaskGraph(const DataflowGraph &graph)
{
    // Performance analysis runs after child generation, so graph.cyclic is not
    // populated yet. Check all edges, including initialized FIFO edges, here.
    std::map<std::string, unsigned> degree;
    for (const DataflowTask &task : graph.tasks)
        degree.emplace(task.name, 0);
    for (const DataflowChannel &channel : graph.channels)
    {
        if (!degree.count(channel.producer) || !degree.count(channel.consumer))
            return false;
        ++degree[channel.consumer];
    }
    std::vector<std::string> ready;
    for (const auto &entry : degree)
        if (entry.second == 0)
            ready.push_back(entry.first);
    unsigned visited = 0;
    while (!ready.empty())
    {
        const std::string task = ready.back();
        ready.pop_back();
        ++visited;
        for (const DataflowChannel &channel : graph.channels)
            if (channel.producer == task && --degree[channel.consumer] == 0)
                ready.push_back(channel.consumer);
    }
    return visited == graph.tasks.size();
}

} // namespace

char HI_DataflowTaskGeneration::ID = 0;

HI_DataflowTaskGeneration::HI_DataflowTaskGeneration(
    HLSBackendContextPtr context)
    : ModulePass(ID), context(std::move(context))
{
}

bool HI_DataflowTaskGeneration::runOnModule(Module &module)
{
    if (!context || context->hasErrors() ||
        !context->rtl.dataflow.valid)
    {
        return false;
    }

    context->module = &module;
    context->dataflowImplementations.clear();
    const bool acyclic = isAcyclicTaskGraph(context->rtl.dataflow);
    for (const DataflowTask &task : context->rtl.dataflow.tasks)
    {
        if (!task.functionValue || !task.call)
            continue;
        BackendConfiguration configuration = context->configuration;
        configuration.design = context->configuration.design + "_" + task.name;
        configuration.topFunction = task.function;
        configuration.rtlModuleName =
            context->configuration.topFunction + "__" + task.name;
        configuration.rtlClockEnable = true;
        configuration.outputDirectory.clear();
        configuration.runSynthesis = false;
        configuration.pragmas.dataflowCount = 0;
        configuration.pragmas.streamCount = 0;
        selectTaskPipelineDirectives(task, configuration);
        configuration.streams.clear();
        configuration.fifoArguments.clear();
        configuration.fifoReadOnRequestArguments.clear();
        configuration.internalPIPOWriteArguments.clear();
        configuration.storageBindings.clear();
        configuration.arrayPartitions.clear();
        configuration.pragmas.arrayPartitionCount = 0;
        configuration.frontendMemoryDepths.clear();
        configuration.frontendMemoryDimensions.clear();
        configuration.configuredMemoryDepths.clear();
        configuration.configuredMemoryPorts.clear();
        configuration.configuredMemoryReadLatencies.clear();

        for (unsigned index = 0; index < task.functionValue->arg_size(); ++index)
        {
            const Argument *argument = task.functionValue->getArg(index);
            if (!argument->getType()->isPointerTy())
                continue;
            const std::string argumentName =
                argument->hasName()
                    ? argument->getName().str()
                    : "memory" + std::to_string(index);
            const Value *base = getUnderlyingObject(
                task.call->getArgOperand(index));
            selectTaskMemoryDirectives(*context, task, index, base,
                                       argumentName, configuration);
            if (const DataflowChannel *channel =
                    findChannel(context->rtl.dataflow, base))
            {
                if (channel->type == "fifo")
                {
                    configuration.fifoArguments.insert(index);
                    // Ordinary consumers sample the stable FIFO head on pop.
                    // Keep pipeline, public, and cyclic endpoints registered.
                    if (channel->consumer == task.name &&
                        configuration.pragmas.pipeline.empty() && acyclic)
                        configuration.fifoReadOnRequestArguments.insert(index);
                }
                else if (channel->type == "pipo" && channel->producer == task.name)
                    configuration.internalPIPOWriteArguments.insert(index);
                configuration.configuredMemoryDepths[argumentName] =
                    channel->type == "fifo" ? channel->instances
                                            : channel->depth;
                configuration.configuredMemoryPorts[argumentName] = 1;
                configuration.configuredMemoryReadLatencies[argumentName] = 1;
                std::vector<unsigned long long> dimensions =
                    allocationDimensions(base);
                if (!dimensions.empty())
                {
                    configuration.frontendMemoryDimensions[argumentName] =
                        std::move(dimensions);
                }
                continue;
            }
            const unsigned long long depth = externalDepth(*context, base);
            if (depth)
                configuration.configuredMemoryDepths[argumentName] = depth;
            if (const auto *topArgument = dyn_cast<Argument>(base))
            {
                const std::string topName =
                    topArgument->hasName()
                        ? topArgument->getName().str()
                        : "arg" + std::to_string(topArgument->getArgNo());
                for (const StreamDirective &stream : context->configuration.streams)
                {
                    if (stream.scope == context->configuration.topFunction &&
                        stream.variable == topName && stream.type == "fifo")
                        configuration.fifoArguments.insert(index);
                }
                auto dimensions =
                    context->configuration.frontendMemoryDimensions.find(
                        topName);
                if (dimensions !=
                    context->configuration.frontendMemoryDimensions.end())
                {
                    configuration.frontendMemoryDimensions[argumentName] =
                        dimensions->second;
                }
            }
        }

        HLSBackendContextPtr child = createBackendContext(module, configuration);
        child->latencyModel = context->latencyModel;
        child->delayModel = context->delayModel;
        legacy::PassManager manager;
        manager.add(new HI_PredicatedLoopLowering(child));
        manager.add(new HI_BackendValidation(child, ValidationStage::PreBackend));
        manager.add(new HI_MemoryObjectAnalysis(child));
        manager.add(new HI_LoopPipelineAnalysis(child));
        manager.add(new HI_ArrayPartitionAnalysis(child));
        manager.add(new HI_StorageBindingAnalysis(child));
        manager.add(new HI_BackendDependenceAnalysis(child));
        manager.add(new HI_OperationScheduling(child));
        manager.add(new HI_PipelineStorageAnalysis(child));
        manager.add(new HI_ResourceBinding(child));
        manager.add(new HI_RTLGeneration(child));
        manager.add(new HI_BackendValidation(child, ValidationStage::PostRTL));
        manager.run(module);

        if (child->hasErrors())
        {
            for (const BackendDiagnostic &diagnostic : child->diagnostics)
            {
                if (diagnostic.severity !=
                    light_hls::backend::DiagnosticSeverity::Error)
                    continue;
                context->addError(
                    "BACKEND_DATAFLOW_TASK_" + diagnostic.code,
                    "Task '" + task.name + "': " + diagnostic.message,
                    diagnostic.location.empty() ? task.name
                                                : diagnostic.location);
            }
            continue;
        }

        DataflowTaskImplementation implementation;
        implementation.task = task.name;
        implementation.moduleName = child->rtl.moduleName;
        implementation.verilog = child->rtl.verilog;
        implementation.floatingResourceEstimate = floatingResourceEstimateJSON(*child);
        implementation.memories = child->memoryObjects;
        implementation.pipelineCount =
            static_cast<unsigned>(child->schedule.pipelines.size());
        for (const LoopPipelineInfo &pipeline : child->schedule.pipelines)
        {
            if (pipeline.achievedII > 0)
            {
                implementation.pipelineInitiationInterval = std::max(
                    implementation.pipelineInitiationInterval,
                    static_cast<unsigned>(pipeline.achievedII));
            }
        }
        Function &taskFunction =
            *const_cast<Function *>(task.functionValue);
        LoopInfo &loopInfo =
            getAnalysis<LoopInfoWrapperPass>(taskFunction).getLoopInfo();
        ScalarEvolution &scalarEvolution =
            getAnalysis<ScalarEvolutionWrapperPass>(taskFunction).getSE();
        bool structuralLatencyExact = false;
        const long long structuralLatency = structuralTransactionLatency(
            child->schedule, loopInfo, scalarEvolution,
            structuralLatencyExact);
        if (structuralLatency > 0)
        {
            implementation.latencyCycles = structuralLatency;
            implementation.latencyExact = structuralLatencyExact;
            implementation.latencySource =
                structuralLatencyExact
                    ? "generated RTL FSM with static loop trips"
                    : "generated RTL FSM conservative control-flow bound";
            const unsigned long long reacceptanceInterval =
                static_cast<unsigned long long>(structuralLatency) + 1;
            implementation.initiationInterval = static_cast<unsigned>(
                std::min<unsigned long long>(
                    reacceptanceInterval,
                    std::numeric_limits<unsigned>::max()));
        }
        auto latency = context->configuration.dataflowTaskLatencies.find(
            task.function);
        if (implementation.latencyCycles <= 0 &&
            latency !=
            context->configuration.dataflowTaskLatencies.end())
        {
            implementation.latencyCycles = latency->second;
            implementation.latencySource = "pre-backend evaluator fallback";
            implementation.latencyExact = false;
        }
        auto interval =
            context->configuration.dataflowTaskInitiationIntervals.find(
                task.function);
        if (implementation.initiationInterval == 0 &&
            interval !=
            context->configuration.dataflowTaskInitiationIntervals.end())
        {
            implementation.initiationInterval = interval->second;
        }
        std::map<unsigned, DataflowTaskPortAccess> accessesByArgument;
        for (const auto &accessEntry : child->memoryAccesses)
        {
            const MemoryObject *accessMemory = nullptr;
            for (const MemoryObject &memory : child->memoryObjects)
            {
                if (memory.base == accessEntry.second.memoryBase)
                {
                    accessMemory = &memory;
                    break;
                }
            }
            if (!accessMemory || !accessMemory->argument)
                continue;
            const unsigned argumentIndex =
                accessMemory->argument->getArgNo();
            DataflowTaskPortAccess &port =
                accessesByArgument[argumentIndex];
            port.argumentIndex = argumentIndex;
            if (accessEntry.second.isWrite)
                port.writes.push_back(accessEntry.first);
            else
                port.reads.push_back(accessEntry.first);
        }
        for (auto &port : accessesByArgument)
        {
            populateAccessTiming(port.second, child->schedule, loopInfo,
                                 scalarEvolution);
            implementation.portAccesses.push_back(std::move(port.second));
        }
        context->dataflowImplementations.push_back(std::move(implementation));
    }
    if (!context->hasErrors() &&
        context->dataflowImplementations.size() !=
            context->rtl.dataflow.tasks.size())
    {
        context->addError(
            "BACKEND_DATAFLOW_TASK_RTL_MISSING",
            "Not every DATAFLOW task produced an RTL implementation.",
            context->configuration.topFunction);
    }
    return false;
}

void HI_DataflowTaskGeneration::getAnalysisUsage(AnalysisUsage &usage) const
{
    usage.addRequired<LoopInfoWrapperPass>();
    usage.addRequired<ScalarEvolutionWrapperPass>();
    usage.setPreservesAll();
}
