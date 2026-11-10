#include "HI_DataflowAnalysis.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Instructions.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>

using namespace llvm;
using namespace light_hls::backend;

namespace
{

bool isIgnoredCall(const CallBase &call)
{
    const Function *callee = call.getCalledFunction();
    if (!callee)
        return true;
    if (callee->isIntrinsic())
        return true;
    const StringRef name = callee->getName();
    return name.starts_with("_ZN3hls6stream") ||
           name.contains(".arrayctor.") ||
           name.contains(".arraydestroy.");
}

std::string shortFunctionName(const Function &function)
{
    std::string name = demangle(function.getName().str());
    const std::size_t parameters = name.find('(');
    if (parameters != std::string::npos)
        name.erase(parameters);
    const std::size_t scope = name.rfind("::");
    if (scope != std::string::npos)
        name.erase(0, scope + 2);
    return sanitizeVerilogIdentifier(name, "task");
}

bool referencesArgumentAddress(const Value *pointer, const Argument &argument)
{
    // Strength reduction represents stream-array addresses as integer PHIs.
    // Volatile endpoints have no readonly/writeonly attributes to fall back on.
    // Trace only address-producing operations, never a loaded payload.
    std::vector<const Value *> pending{pointer};
    std::set<const Value *> visited;
    while (!pending.empty())
    {
        const Value *value = pending.back();
        pending.pop_back();
        if (value == &argument)
            return true;
        if (!visited.insert(value).second)
            continue;
        if (const auto *gep = dyn_cast<GetElementPtrInst>(value))
            pending.push_back(gep->getPointerOperand());
        else if (const auto *cast = dyn_cast<CastInst>(value))
            pending.push_back(cast->getOperand(0));
        else if (const auto *phi = dyn_cast<PHINode>(value))
            for (const Value *incoming : phi->incoming_values())
                pending.push_back(incoming);
        else if (const auto *select = dyn_cast<SelectInst>(value))
        {
            pending.push_back(select->getTrueValue());
            pending.push_back(select->getFalseValue());
        }
        else if (const auto *binary = dyn_cast<BinaryOperator>(value))
        {
            if (binary->getOpcode() == Instruction::Add ||
                binary->getOpcode() == Instruction::Sub)
            {
                pending.push_back(binary->getOperand(0));
                pending.push_back(binary->getOperand(1));
            }
        }
    }
    return false;
}

std::pair<bool, bool> argumentAccess(const Function &function,
                                     const Argument &argument)
{
    bool read = argument.hasAttribute(Attribute::ReadOnly);
    bool write = argument.hasAttribute(Attribute::WriteOnly);
    for (const BasicBlock &block : function)
    {
        for (const Instruction &instruction : block)
        {
            if (const auto *load = dyn_cast<LoadInst>(&instruction))
            {
                read |= referencesArgumentAddress(load->getPointerOperand(), argument);
            }
            else if (const auto *store = dyn_cast<StoreInst>(&instruction))
            {
                write |= referencesArgumentAddress(store->getPointerOperand(), argument);
            }
        }
    }
    return {read, write};
}

unsigned scalarWidth(const Type *type)
{
    if (const auto *integer = dyn_cast<IntegerType>(type))
        return integer->getBitWidth();
    if (const auto *array = dyn_cast<ArrayType>(type))
        return scalarWidth(array->getElementType());
    if (const auto *structure = dyn_cast<StructType>(type))
    {
        for (Type *element : structure->elements())
        {
            const unsigned width = scalarWidth(element);
            if (width)
                return width;
        }
    }
    return 0;
}

unsigned long long scalarCount(const Type *type)
{
    if (const auto *array = dyn_cast<ArrayType>(type))
    {
        const unsigned long long nested = scalarCount(array->getElementType());
        if (nested == 0 ||
            array->getNumElements() >
                std::numeric_limits<unsigned long long>::max() / nested)
        {
            return 0;
        }
        return array->getNumElements() * nested;
    }
    return 1;
}

const StreamDirective *findStream(const HLSBackendContext &context,
                                  const std::string &name)
{
    for (const StreamDirective &stream : context.configuration.streams)
    {
        if (stream.scope == context.configuration.topFunction &&
            stream.variable == name)
        {
            return &stream;
        }
    }
    return nullptr;
}

} // namespace

char HI_DataflowAnalysis::ID = 0;

HI_DataflowAnalysis::HI_DataflowAnalysis(HLSBackendContextPtr context)
    : ModulePass(ID), context(std::move(context))
{
}

bool HI_DataflowAnalysis::runOnModule(Module &module)
{
    if (!context || context->hasErrors() || !context->top)
        return false;

    context->module = &module;
    context->rtl.dataflow = DataflowGraph();
    DataflowGraph &graph = context->rtl.dataflow;
    DominatorTree dominators(*context->top);
    LoopInfo loops(dominators);
    PostDominatorTree postDominators(*context->top);
    std::map<const CallBase *, std::string> taskNames;
    unsigned taskIndex = 0;
    for (const BasicBlock &block : *context->top)
    {
        for (const Instruction &instruction : block)
        {
            const auto *call = dyn_cast<CallBase>(&instruction);
            if (!call || isIgnoredCall(*call))
                continue;
            const Function *callee = call->getCalledFunction();
            if (!callee || callee->isDeclaration())
            {
                context->addError(
                    "BACKEND_DATAFLOW_INDIRECT_TASK",
                    "DATAFLOW tasks must be direct calls to defined functions.",
                    block.getName().str());
                continue;
            }
            if (!postDominators.dominates(
                    call->getParent(), &context->top->getEntryBlock()) ||
                loops.getLoopFor(call->getParent()))
            {
                context->addError(
                    "BACKEND_DATAFLOW_CONDITIONAL_TASK",
                    "DATAFLOW task calls must execute unconditionally once "
                    "per top-level invocation.",
                    block.getName().str());
                continue;
            }
            if (!callee->getReturnType()->isVoidTy())
            {
                context->addError(
                    "BACKEND_DATAFLOW_TASK_RETURN",
                    "DATAFLOW task functions must return void; scalar task "
                    "return channels are not implemented.",
                    callee->getName().str());
                continue;
            }
            DataflowTask task;
            task.name = shortFunctionName(*callee) + "_" +
                        std::to_string(taskIndex++);
            task.function = callee->getName().str();
            task.functionValue = callee;
            task.call = call;
            taskNames[call] = task.name;
            graph.tasks.push_back(std::move(task));
        }
    }
    if (graph.tasks.size() < 2)
    {
        context->addError(
            "BACKEND_DATAFLOW_TASK_COUNT",
            "A DATAFLOW region requires at least two top-level task calls.",
            context->configuration.topFunction);
        return false;
    }

    std::map<const Value *, std::vector<std::pair<const DataflowTask *, unsigned>>>
        channelUsers;
    for (const DataflowTask &task : graph.tasks)
    {
        const Function *callee = task.functionValue;
        if (!callee || !task.call ||
            task.call->arg_size() != callee->arg_size())
        {
            context->addError(
                "BACKEND_DATAFLOW_TASK_SIGNATURE",
                "DATAFLOW task '" + task.name +
                    "' has an unsupported call signature.",
                task.name);
            continue;
        }
        for (unsigned index = 0; index < task.call->arg_size(); ++index)
        {
            const Value *base = getUnderlyingObject(
                task.call->getArgOperand(index));
            if (isa<AllocaInst>(base))
                channelUsers[base].push_back({&task, index});
        }
    }

    for (const auto &entry : channelUsers)
    {
        const auto *allocation = dyn_cast<AllocaInst>(entry.first);
        if (!allocation || entry.second.size() < 2)
            continue;
        DataflowChannel channel;
        channel.name = allocation->hasName()
                           ? allocation->getName().str()
                           : "channel_" +
                                 std::to_string(graph.channels.size());
        channel.base = allocation;
        const StreamDirective *stream = findStream(*context, channel.name);
        if (stream)
        {
            channel.type = stream->type;
            channel.width = stream->bitWidth;
            channel.depth = stream->depth;
            channel.instances = stream->instances;
            channel.initialTokens = stream->initialTokens;
        }
        else
        {
            channel.type = "pipo";
            channel.width = scalarWidth(allocation->getAllocatedType());
            const unsigned long long count =
                scalarCount(allocation->getAllocatedType());
            channel.depth = count > std::numeric_limits<unsigned>::max()
                                ? 0
                                : static_cast<unsigned>(count);
        }
        if (channel.width == 0 || channel.depth == 0 ||
            channel.instances == 0)
        {
            context->addError(
                "BACKEND_DATAFLOW_CHANNEL_SHAPE",
                "Cannot determine a positive width, depth, and instance "
                "count for DATAFLOW channel '" + channel.name + "'.",
                channel.name);
            continue;
        }
        if (channel.type != "fifo" && channel.initialTokens != 0)
        {
            context->addError(
                "BACKEND_DATAFLOW_INITIAL_TOKEN_PROTOCOL",
                "Initial tokens are supported only for FIFO DATAFLOW channels.",
                channel.name);
        }

        std::vector<const DataflowTask *> producers;
        std::vector<const DataflowTask *> consumers;
        for (const auto &user : entry.second)
        {
            const DataflowTask *task = user.first;
            const Argument *argument =
                task->functionValue->getArg(user.second);
            const auto access = argumentAccess(*task->functionValue, *argument);
            if (access.second)
            {
                if (std::find(producers.begin(), producers.end(), task) ==
                    producers.end())
                    producers.push_back(task);
            }
            if (access.first)
            {
                if (std::find(consumers.begin(), consumers.end(), task) ==
                    consumers.end())
                    consumers.push_back(task);
            }
        }
        if (producers.size() > 1)
        {
            context->addError(
                "BACKEND_DATAFLOW_MULTIPLE_PRODUCERS",
                "DATAFLOW channel '" + channel.name +
                    "' has more than one producer; multi-writer fan-in has no deterministic Vitis-compatible ordering.",
                channel.name);
            continue;
        }
        if (producers.empty() || consumers.empty())
        {
            context->addError(
                "BACKEND_DATAFLOW_CHANNEL_ENDPOINTS",
                "DATAFLOW channel '" + channel.name +
                    "' requires one producer and at least one consumer.",
                channel.name);
            continue;
        }
        channel.producer = producers.front()->name;
        channel.broadcast = consumers.size() > 1;
        for (const DataflowTask *consumer : consumers)
        {
            if (consumer == producers.front())
            {
                context->addError(
                    "BACKEND_DATAFLOW_CHANNEL_ENDPOINTS",
                    "DATAFLOW channel '" + channel.name +
                        "' cannot use the same task as producer and consumer.",
                    channel.name);
                continue;
            }
            DataflowChannel edge = channel;
            edge.consumer = consumer->name;
            if (channel.broadcast)
            {
                edge.name = channel.name + "__to__" + consumer->name;
            }
            graph.channels.push_back(std::move(edge));
        }
    }

    // Channel discovery visits a pointer-keyed map. Stable source names keep
    // RTL declaration order and graph reports independent of allocator layout.
    std::sort(graph.channels.begin(), graph.channels.end(),
              [](const DataflowChannel &left, const DataflowChannel &right) {
                  return left.name < right.name;
              });
    graph.valid = !context->hasErrors() && !graph.channels.empty();
    if (!context->hasErrors() && graph.channels.empty())
    {
        context->addError(
            "BACKEND_DATAFLOW_CHANNEL_COUNT",
            "The DATAFLOW task graph does not contain an internal channel.",
            context->configuration.topFunction);
    }
    return false;
}

void HI_DataflowAnalysis::getAnalysisUsage(AnalysisUsage &usage) const
{
    usage.setPreservesAll();
}
