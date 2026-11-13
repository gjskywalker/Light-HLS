#include "HI_DataflowRTLGeneration.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;
using namespace light_hls::backend;

namespace
{

struct TaskEndpoint
{
    const DataflowTask *task = nullptr;
    const DataflowTaskImplementation *implementation = nullptr;
    const MemoryObject *memory = nullptr;
    const Value *rootBase = nullptr;
    unsigned argumentIndex = 0;
};

unsigned widthForMaximum(unsigned maximum)
{
    unsigned width = 0;
    do
    {
        ++width;
        maximum >>= 1;
    } while (maximum);
    return width;
}

std::string rangeForWidth(unsigned width)
{
    return width <= 1 ? "" : "[" + std::to_string(width - 1) + ":0] ";
}

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

const MemoryObject *findArgumentMemory(
    const DataflowTaskImplementation &implementation, unsigned argumentIndex)
{
    for (const MemoryObject &memory : implementation.memories)
    {
        if (memory.argument && memory.argument->getArgNo() == argumentIndex)
            return &memory;
    }
    return nullptr;
}

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

const StreamDirective *findTopStream(const HLSBackendContext &context,
                                     const Argument &argument)
{
    const std::string name = argument.hasName()
                                 ? argument.getName().str()
                                 : "arg" + std::to_string(argument.getArgNo());
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

std::string endpointPrefix(const TaskEndpoint &endpoint)
{
    return sanitizeVerilogIdentifier(
               endpoint.task->name + "__" + endpoint.memory->portName,
               "task_memory") +
           "_";
}

std::string endpointBase(const TaskEndpoint &endpoint)
{
    return sanitizeVerilogIdentifier(
        endpoint.task->name + "__" + endpoint.memory->portName,
        "task_memory");
}

const MemoryPortUsage *bankPortUsage(const MemoryObject &memory,
                                     unsigned bank, unsigned port)
{
    if (bank >= memory.bankPortUsage.size() ||
        port >= memory.bankPortUsage[bank].size())
    {
        return nullptr;
    }
    return &memory.bankPortUsage[bank][port];
}

bool bankPortReads(const MemoryObject &memory, unsigned bank, unsigned port)
{
    const MemoryPortUsage *usage = bankPortUsage(memory, bank, port);
    return usage && usage->hasRead;
}

bool bankPortWrites(const MemoryObject &memory, unsigned bank, unsigned port)
{
    const MemoryPortUsage *usage = bankPortUsage(memory, bank, port);
    return usage && usage->hasWrite;
}

bool bankPortUsed(const MemoryObject &memory, unsigned bank, unsigned port)
{
    return bankPortReads(memory, bank, port) ||
           bankPortWrites(memory, bank, port);
}

bool usesScalarBanks(const MemoryObject &memory)
{
    return memory.storage == MemoryStorageKind::ExternalAPMemory &&
           memory.banks > 1 && memory.bankDepth == 1 &&
           memory.banks == memory.depth;
}

bool scalarBankReads(const MemoryObject &memory, unsigned bank)
{
    for (unsigned port = 0; port < memory.ports; ++port)
    {
        if (bankPortReads(memory, bank, port))
            return true;
    }
    return false;
}

bool scalarBankWrites(const MemoryObject &memory, unsigned bank)
{
    for (unsigned port = 0; port < memory.ports; ++port)
    {
        if (bankPortWrites(memory, bank, port))
            return true;
    }
    return false;
}

int channelPort(const MemoryObject &memory, unsigned bank, bool write)
{
    for (unsigned port = 0; port < memory.ports; ++port)
    {
        if (write ? bankPortWrites(memory, bank, port)
                  : bankPortReads(memory, bank, port))
        {
            return static_cast<int>(port);
        }
    }
    return -1;
}

std::string memoryBankPrefix(const MemoryObject &memory, unsigned bank)
{
    if (memory.banks <= 1)
        return memory.portName + "_";
    return memory.portName + "_" + std::to_string(bank) + "_";
}

std::string memoryScalarName(const MemoryObject &memory, unsigned bank)
{
    return memory.portName + "_" + std::to_string(bank);
}

std::string endpointBankPrefix(const TaskEndpoint &endpoint, unsigned bank)
{
    const std::string base = endpointBase(endpoint);
    if (endpoint.memory->banks <= 1)
        return base + "_";
    return base + "_" + std::to_string(bank) + "_";
}

std::string endpointScalarName(const TaskEndpoint &endpoint, unsigned bank)
{
    return endpointBase(endpoint) + "_" + std::to_string(bank);
}

std::string topMemoryBase(const Argument &argument)
{
    const std::string name = argument.hasName()
                                 ? argument.getName().str()
                                 : "memory" +
                                       std::to_string(argument.getArgNo());
    return sanitizeVerilogPortIdentifier(name, "memory");
}

std::string topMemoryPrefix(const Argument &argument)
{
    return topMemoryBase(argument) + "_";
}

std::string topMemoryBankPrefix(const Argument &argument,
                                const MemoryObject &memory, unsigned bank)
{
    const std::string base = topMemoryBase(argument);
    if (memory.banks <= 1)
        return base + "_";
    return base + "_" + std::to_string(bank) + "_";
}

std::string topMemoryScalarName(const Argument &argument, unsigned bank)
{
    return topMemoryBase(argument) + "_" + std::to_string(bank);
}

bool memoryReads(const MemoryObject &memory)
{
    return memory.accessMode == MemoryAccessMode::ReadOnly ||
           memory.accessMode == MemoryAccessMode::ReadWrite;
}

bool memoryWrites(const MemoryObject &memory)
{
    return memory.accessMode == MemoryAccessMode::WriteOnly ||
           memory.accessMode == MemoryAccessMode::ReadWrite;
}

std::string addressSelect(const std::string &address, unsigned instance,
                          unsigned instances)
{
    if (instances == 1)
        return "1'b1";
    return "(" + address + " == " + std::to_string(instance) + ")";
}

std::string fifoMux(const std::string &address, const std::string &channel,
                    unsigned instances)
{
    if (instances == 1)
        return channel + "_0_dout";
    std::string expression = channel + "_" +
                             std::to_string(instances - 1) + "_dout";
    for (unsigned reverse = instances - 1; reverse > 0; --reverse)
    {
        const unsigned instance = reverse - 1;
        expression = "(" + address + " == " + std::to_string(instance) +
                     ") ? " + channel + "_" +
                     std::to_string(instance) + "_dout : (" + expression +
                     ")";
    }
    return expression;
}

std::string channelBankName(const std::string &channel, unsigned bank,
                            unsigned banks)
{
    return banks <= 1 ? channel
                      : channel + "_bank_" + std::to_string(bank);
}

std::string constantConnection(const Value *value)
{
    if (const auto *floating = dyn_cast<ConstantFP>(value))
    {
        const APInt bits = floating->getValueAPF().bitcastToAPInt();
        return std::to_string(bits.getBitWidth()) + "'d" + std::to_string(bits.getZExtValue());
    }
    const auto *constant = dyn_cast<ConstantInt>(value);
    if (!constant)
        return "1'b0";
    return std::to_string(constant->getBitWidth()) + "'d" +
           std::to_string(constant->getZExtValue());
}

std::string fifoModule()
{
    return R"(
module light_hls_fifo #(
  parameter WIDTH = 32,
  parameter DEPTH = 2,
  parameter ADDRESS_WIDTH = 1,
  parameter COUNT_WIDTH = 2,
  parameter INITIAL_TOKENS = 0
) (
  input wire clk,
  input wire rst,
  input wire write_enable,
  input wire read_enable,
  input wire [WIDTH-1:0] data_in,
  output wire [WIDTH-1:0] data_out,
  output wire full,
  output wire empty
);
  reg [ADDRESS_WIDTH-1:0] write_pointer;
  reg [ADDRESS_WIDTH-1:0] read_pointer;
  reg [COUNT_WIDTH-1:0] count;

  assign full = (count == DEPTH);
  assign empty = (count == 0);

  generate
    if (INITIAL_TOKENS == 0) begin : generate_reset_free_payload
      (* ram_style = "auto" *) reg [WIDTH-1:0] storage [0:DEPTH-1];

      assign data_out = storage[read_pointer];

      always @(posedge clk) begin
        if (!rst && write_enable && !full)
          storage[write_pointer] <= data_in;
      end
    end else begin : generate_initialized_payload
      reg [WIDTH-1:0] storage [0:DEPTH-1];
      integer initialize_index;

      assign data_out = storage[read_pointer];

      always @(posedge clk) begin
        if (rst) begin
          for (initialize_index = 0; initialize_index < DEPTH;
               initialize_index = initialize_index + 1)
            storage[initialize_index] <= 0;
        end else if (write_enable && !full) begin
          storage[write_pointer] <= data_in;
        end
      end
    end
  endgenerate

  always @(posedge clk) begin
    if (rst) begin
      write_pointer <= (INITIAL_TOKENS == DEPTH) ? 0 : INITIAL_TOKENS;
      read_pointer <= 0;
      count <= INITIAL_TOKENS;
    end else begin
      if (write_enable && !full) begin
        write_pointer <= (write_pointer == DEPTH - 1) ? 0 : write_pointer + 1'b1;
      end
      if (read_enable && !empty)
        read_pointer <= (read_pointer == DEPTH - 1) ? 0 : read_pointer + 1'b1;
      case ({write_enable && !full, read_enable && !empty})
        2'b10: count <= count + 1'b1;
        2'b01: count <= count - 1'b1;
        default: count <= count;
      endcase
    end
  end
endmodule

)";
}

std::string pipoModule()
{
    return R"(
module light_hls_pipo_bank #(
  parameter WIDTH = 32,
  parameter DEPTH = 2,
  parameter ADDRESS_WIDTH = 1,
  parameter PEEK_ENABLED = 0
) (
  input wire clk,
  input wire write_bank,
  input wire read_bank,
  input wire write_enable,
  input wire [ADDRESS_WIDTH-1:0] write_address,
  input wire [WIDTH-1:0] data_in,
  input wire read_enable,
  input wire [ADDRESS_WIDTH-1:0] read_address,
  output reg [WIDTH-1:0] data_out,
  output wire [WIDTH-1:0] peek_data
);
  // Payload storage intentionally has no reset. The synchronous-only RAM
  // path lets Vivado select LUTRAM or BRAM instead of expanding every word
  // into resettable flip-flops.
  // Bank ownership becomes an address bit (or offset for non-power-of-two
  // depths), allowing both transaction buffers to share RAM primitive capacity.
  (* ram_style = "auto" *) reg [WIDTH-1:0] storage [0:2*DEPTH-1];
  wire [ADDRESS_WIDTH:0] physical_write_address = write_address + (write_bank ? DEPTH : 0);
  wire [ADDRESS_WIDTH:0] physical_read_address = read_address + (read_bank ? DEPTH : 0);
  generate
    if (PEEK_ENABLED != 0) begin : generate_scalar_peek
      // Complete-partition scalar banks require a combinational value. This
      // branch is removed for ordinary array banks so it cannot block RAM
      // inference on their payload storage.
      assign peek_data = storage[read_bank ? DEPTH : 0];
    end else begin : generate_no_scalar_peek
      assign peek_data = {WIDTH{1'b0}};
    end
  endgenerate

  always @(posedge clk) begin
    if (write_enable)
      storage[physical_write_address] <= data_in;
  end

  // Select the transaction bank on the request edge, then retain one response.
  // Bank ownership changes cannot switch an already accepted read response.
  always @(posedge clk) begin
    if (read_enable)
      data_out <= storage[physical_read_address];
  end
endmodule

)";
}

} // namespace

char HI_DataflowRTLGeneration::ID = 0;

HI_DataflowRTLGeneration::HI_DataflowRTLGeneration(
    HLSBackendContextPtr context)
    : ModulePass(ID), context(std::move(context))
{
}

bool HI_DataflowRTLGeneration::runOnModule(Module &module)
{
    if (!context || context->hasErrors() ||
        !context->rtl.dataflow.valid || !context->top)
    {
        return false;
    }

    context->module = &module;
    DataflowGraph &graph = context->rtl.dataflow;
    std::vector<TaskEndpoint> endpoints;
    for (const DataflowTask &task : graph.tasks)
    {
        const DataflowTaskImplementation *implementation =
            findImplementation(*context, task.name);
        if (!implementation || !task.call)
            continue;
        for (unsigned index = 0; index < task.call->arg_size(); ++index)
        {
            const MemoryObject *memory =
                findArgumentMemory(*implementation, index);
            if (!memory)
                continue;
            endpoints.push_back(
                {&task, implementation, memory,
                 getUnderlyingObject(task.call->getArgOperand(index)), index});
        }
    }

    std::map<const Value *, std::vector<const TaskEndpoint *>> byRootBase;
    std::map<std::string, const TaskEndpoint *> channelProducers;
    std::map<std::string, const TaskEndpoint *> channelConsumers;
    for (const TaskEndpoint &endpoint : endpoints)
    {
        byRootBase[endpoint.rootBase].push_back(&endpoint);
        for (const DataflowChannel &channel : graph.channels)
        {
            if (channel.base != endpoint.rootBase)
                continue;
            if (endpoint.task->name == channel.producer)
                channelProducers[channel.name] = &endpoint;
            if (endpoint.task->name == channel.consumer)
                channelConsumers[channel.name] = &endpoint;
        }
    }

    for (DataflowChannel &channel : graph.channels)
    {
        const auto producer = channelProducers.find(channel.name);
        if (producer == channelProducers.end() || !producer->second)
            continue;
        const MemoryObject &memory = *producer->second->memory;
        channel.partition = memory.partition;
        channel.banks = memory.banks;
        channel.bankDepth = memory.bankDepth;
        const auto consumer = channelConsumers.find(channel.name);
        channel.consumerReadOnRequest = consumer != channelConsumers.end() &&
            consumer->second && consumer->second->memory->fifoReadOnRequest;
    }

    for (const DataflowChannel &channel : graph.channels)
    {
        const TaskEndpoint *producer = channelProducers[channel.name];
        const TaskEndpoint *consumer = channelConsumers[channel.name];
        if (!producer || !consumer || !memoryWrites(*producer->memory) ||
            memoryReads(*producer->memory) || !memoryReads(*consumer->memory) ||
            memoryWrites(*consumer->memory))
        {
            context->addError(
                "BACKEND_DATAFLOW_CHANNEL_DIRECTION",
                "Channel '" + channel.name +
                    "' must connect a write-only producer to a read-only consumer.",
                channel.name);
        }
        if (channel.type == "pipo" && producer && consumer)
        {
            const MemoryObject &producerMemory = *producer->memory;
            const MemoryObject &consumerMemory = *consumer->memory;
            if (producerMemory.banks != consumerMemory.banks ||
                producerMemory.bankDepth != consumerMemory.bankDepth ||
                usesScalarBanks(producerMemory) !=
                    usesScalarBanks(consumerMemory))
            {
                context->addError(
                    "BACKEND_DATAFLOW_PARTITION_LAYOUT_MISMATCH",
                    "PIPO channel '" + channel.name +
                        "' must use the same ARRAY_PARTITION layout in its producer and consumer tasks.",
                    channel.name);
            }
            if (!usesScalarBanks(producerMemory))
            {
                for (unsigned bank = 0; bank < producerMemory.banks; ++bank)
                {
                    unsigned producerPorts = 0;
                    unsigned consumerPorts = 0;
                    for (unsigned port = 0; port < producerMemory.ports; ++port)
                        producerPorts +=
                            bankPortWrites(producerMemory, bank, port) ? 1 : 0;
                    for (unsigned port = 0; port < consumerMemory.ports; ++port)
                        consumerPorts +=
                            bankPortReads(consumerMemory, bank, port) ? 1 : 0;
                    if (producerPorts > 1 || consumerPorts > 1)
                    {
                        context->addError(
                            "BACKEND_DATAFLOW_PARTITION_MULTI_PORT",
                            "Partitioned PIPO channel '" + channel.name +
                                "' currently permits one active memory port per bank and direction.",
                            channel.name);
                        break;
                    }
                }
            }
        }
        if (consumer && consumer->memory->fifoReadOnRequest &&
            (channel.type != "fifo" || graph.cyclic || consumer->implementation->pipelineCount != 0 ||
             consumer->memory->readLatency != 0))
            context->addError("BACKEND_DATAFLOW_FIFO_POP_CAPTURE_INVALID",
                "Direct FIFO-head wiring requires an ordinary acyclic internal consumer with request-edge sampling.",
                channel.name);
    }
    for (Argument &argument : context->top->args())
    {
        if (!argument.getType()->isPointerTy())
            continue;
        auto users = byRootBase.find(&argument);
        if (users == byRootBase.end() || users->second.size() != 1)
        {
            context->addError(
                "BACKEND_DATAFLOW_EXTERNAL_MEMORY_OWNERSHIP",
                "External memory '" + argument.getName().str() +
                    "' must be connected to exactly one DATAFLOW task.",
                argument.getName().str());
        }
    }
    if (context->hasErrors())
        return false;

    const auto completionTasks = dataflowCompletionTasks(graph);
    std::vector<const DataflowTask *> terminalTasks;
    for (const DataflowTask &task : graph.tasks)
    {
        if (completionTasks.count(task.name))
            terminalTasks.push_back(&task);
    }
    if (terminalTasks.empty())
    {
        for (const DataflowTask &task : graph.tasks)
            terminalTasks.push_back(&task);
    }
    const bool multipleTerminalTasks = terminalTasks.size() > 1;
    bool hasScalarTransactionArguments = false;
    for (const Argument &argument : context->top->args())
        hasScalarTransactionArguments |= scalarBitWidth(argument.getType()) != 0;
    std::set<std::string> scalarTransactionTasks;
    for (const DataflowTask &task : graph.tasks)
    {
        if (!task.call)
            continue;
        for (unsigned index = 0; index < task.call->arg_size(); ++index)
        {
            if (scalarBitWidth(task.call->getArgOperand(index)->getType()) &&
                isa<Argument>(task.call->getArgOperand(index)))
            {
                scalarTransactionTasks.insert(task.name);
                break;
            }
        }
    }
    std::set<std::string> pipoConsumerTasks;
    for (const DataflowChannel &channel : graph.channels)
    {
        if (channel.type == "pipo")
            pipoConsumerTasks.insert(channel.consumer);
    }

    std::vector<std::string> ports = {
        "input wire ap_clk", "input wire ap_rst", "input wire ap_start",
        "output wire ap_done", "output wire ap_idle", "output wire ap_ready"};
    for (const Argument &argument : context->top->args())
    {
        if (scalarBitWidth(argument.getType()))
        {
            ports.push_back(
                "input wire " +
                rangeForWidth(scalarBitWidth(argument.getType())) +
                sanitizeVerilogPortIdentifier(argument.getName().str(),
                                              "argument"));
            continue;
        }
        if (!argument.getType()->isPointerTy())
            continue;
        const TaskEndpoint &endpoint = *byRootBase.at(&argument).front();
        const MemoryObject &memory = *endpoint.memory;
        if (const StreamDirective *stream = findTopStream(*context, argument))
        {
            if (stream->instances != 1 || memory.banks != 1)
            {
                context->addError(
                    "BACKEND_DATAFLOW_TOP_STREAM_SHAPE",
                    "A public DATAFLOW stream currently requires one scalar FIFO instance without ARRAY_PARTITION.",
                    argument.getName().str());
                continue;
            }
            const std::string name = topMemoryBase(argument);
            if (memoryReads(memory))
            {
                ports.push_back("input wire " +
                                rangeForWidth(memory.elementWidth) + name +
                                "_dout");
                ports.push_back("input wire " + name + "_empty_n");
                ports.push_back("output wire " + name + "_read");
            }
            if (memoryWrites(memory))
            {
                ports.push_back("output wire " +
                                rangeForWidth(memory.elementWidth) + name +
                                "_din");
                ports.push_back("input wire " + name + "_full_n");
                ports.push_back("output wire " + name + "_write");
            }
            continue;
        }
        for (unsigned bank = 0; bank < memory.banks; ++bank)
        {
            if (usesScalarBanks(memory))
            {
                const bool reads = scalarBankReads(memory, bank);
                const bool writes = scalarBankWrites(memory, bank);
                const std::string scalar =
                    topMemoryScalarName(argument, bank);
                if (reads && !writes)
                {
                    ports.push_back("input wire " +
                                    rangeForWidth(memory.elementWidth) + scalar);
                }
                else if (writes && !reads)
                {
                    ports.push_back("output wire " +
                                    rangeForWidth(memory.elementWidth) + scalar);
                    ports.push_back("output wire " + scalar + "_ap_vld");
                }
                else if (reads && writes)
                {
                    ports.push_back("input wire " +
                                    rangeForWidth(memory.elementWidth) + scalar +
                                    "_i");
                    ports.push_back("output wire " +
                                    rangeForWidth(memory.elementWidth) + scalar +
                                    "_o");
                    ports.push_back("output wire " + scalar + "_o_ap_vld");
                }
                continue;
            }
            const std::string prefix =
                topMemoryBankPrefix(argument, memory, bank);
            for (unsigned port = 0; port < memory.ports; ++port)
            {
                if (!bankPortUsed(memory, bank, port))
                    continue;
                const std::string ordinal = std::to_string(port);
                ports.push_back(
                    "output wire " + rangeForWidth(memory.bankAddressWidth) +
                    prefix + "address" + ordinal);
                ports.push_back("output wire " + prefix + "ce" + ordinal);
                if (bankPortWrites(memory, bank, port))
                {
                    ports.push_back("output wire " + prefix + "we" + ordinal);
                    ports.push_back("output wire " +
                                    rangeForWidth(memory.elementWidth) + prefix +
                                    "d" + ordinal);
                }
                if (bankPortReads(memory, bank, port))
                {
                    ports.push_back("input wire " +
                                    rangeForWidth(memory.elementWidth) + prefix +
                                    "q" + ordinal);
                }
            }
        }
    }

    if (context->hasErrors())
        return false;

    std::ostringstream output;
    for (const DataflowTaskImplementation &implementation :
         context->dataflowImplementations)
    {
        output << implementation.verilog << '\n';
    }
    output << fifoModule() << pipoModule();
    context->rtl.moduleName = sanitizeVerilogIdentifier(
        context->configuration.topFunction, "light_hls_dataflow_top");
    output << "`timescale 1ns/1ps\n"
           << "// Generated by the Light-HLS DATAFLOW RTL backend.\n"
           << "module " << context->rtl.moduleName << " (\n";
    for (std::size_t index = 0; index < ports.size(); ++index)
    {
        output << "  " << ports[index]
               << (index + 1 == ports.size() ? "\n" : ",\n");
    }
    output << ");\n\n"
           << "  localparam integer MAX_OUTSTANDING = 2;\n"
           << "  reg [1:0] outstanding_transactions;\n"
           << "  wire retire_transaction;\n"
           << "  wire accept_transaction;\n"
           << "  assign ap_done = !ap_rst && retire_transaction;\n"
           << "  assign ap_idle = (outstanding_transactions == 0);\n"
           << "  assign ap_ready = "
              "(outstanding_transactions < MAX_OUTSTANDING) || "
              "retire_transaction;\n"
           << "  assign accept_transaction = ap_start && ap_ready;\n";

    if (hasScalarTransactionArguments)
        output << "  reg transaction_write_bank;\n";

    for (const Argument &argument : context->top->args())
    {
        if (!scalarBitWidth(argument.getType()))
            continue;
        const std::string name = sanitizeVerilogPortIdentifier(
            argument.getName().str(), "argument");
        output << "  reg "
               << rangeForWidth(scalarBitWidth(argument.getType()))
               << name << "_transaction_queue [0:1];\n";
    }

    for (const DataflowTask &task : graph.tasks)
    {
        const std::string prefix = sanitizeVerilogIdentifier(task.name, "task");
        std::string channelReady;
        for (const DataflowChannel &channel : graph.channels)
        {
            if (channel.type != "pipo" ||
                (channel.producer != task.name &&
                 channel.consumer != task.name))
            {
                continue;
            }
            const bool producer = channel.producer == task.name;
            const std::string name = sanitizeVerilogIdentifier(
                "channel_" + channel.name, "channel");
            if (!channelReady.empty())
                channelReady += " && ";
            channelReady +=
                name + (producer ? "_write_start_available"
                                 : "_read_start_available");
        }
        if (channelReady.empty())
            channelReady = "1'b1";
        const bool twoBitPending = pipoConsumerTasks.count(task.name) != 0;
        output << "  reg " << (twoBitPending ? "[1:0] " : "") << prefix
               << "_pending;\n";
        if (scalarTransactionTasks.count(task.name) != 0)
            output << "  reg " << prefix << "_read_bank;\n";
        output << "  wire " << prefix
               << "_start = ((" << prefix
               << "_pending != 0) || accept_transaction) && "
               << prefix << "_idle && ("
               << channelReady << ");\n"
               << "  wire " << prefix << "_done;\n"
               << "  wire " << prefix << "_idle;\n"
               << "  wire " << prefix << "_ready;\n";
    }
    if (multipleTerminalTasks)
    {
        for (const DataflowTask *task : terminalTasks)
        {
            const std::string prefix =
                sanitizeVerilogIdentifier(task->name, "task");
            output << "  reg [1:0] " << prefix
                   << "_completion_credit;\n";
        }
    }

    for (const TaskEndpoint &endpoint : endpoints)
    {
        const auto *topArgument = dyn_cast<Argument>(endpoint.rootBase);
        const bool publicStream =
            topArgument && findTopStream(*context, *topArgument);
        if (!findChannel(graph, endpoint.rootBase) && !publicStream)
            continue;
        const MemoryObject &memory = *endpoint.memory;
        for (unsigned bank = 0; bank < memory.banks; ++bank)
        {
            if (usesScalarBanks(memory))
            {
                const std::string scalar = endpointScalarName(endpoint, bank);
                if (scalarBankReads(memory, bank) &&
                    !scalarBankWrites(memory, bank))
                {
                    output << "  wire " << rangeForWidth(memory.elementWidth)
                           << scalar << ";\n";
                }
                else if (scalarBankWrites(memory, bank) &&
                         !scalarBankReads(memory, bank))
                {
                    output << "  wire " << rangeForWidth(memory.elementWidth)
                           << scalar << ";\n"
                           << "  wire " << scalar << "_ap_vld;\n";
                }
                continue;
            }
            const std::string prefix = endpointBankPrefix(endpoint, bank);
            for (unsigned port = 0; port < memory.ports; ++port)
            {
                if (!bankPortUsed(memory, bank, port))
                    continue;
                const std::string ordinal = std::to_string(port);
                output << "  wire " << rangeForWidth(memory.bankAddressWidth)
                       << prefix << "address" << ordinal << ";\n"
                       << "  wire " << prefix << "ce" << ordinal << ";\n";
                if (bankPortWrites(memory, bank, port))
                {
                    output << "  wire " << prefix << "we" << ordinal << ";\n"
                           << "  wire " << rangeForWidth(memory.elementWidth)
                           << prefix << "d" << ordinal << ";\n";
                }
                if (bankPortReads(memory, bank, port))
                {
                    output << "  wire " << rangeForWidth(memory.elementWidth)
                           << prefix << "q" << ordinal << ";\n";
                }
            }
        }
    }

    for (const DataflowChannel &channel : graph.channels)
    {
        const std::string name = sanitizeVerilogIdentifier(
            "channel_" + channel.name, "channel");
        if (channel.type == "pipo")
        {
            const TaskEndpoint &producer = *channelProducers.at(channel.name);
            const unsigned banks = producer.memory->banks;
            output << "  reg " << name << "_ready_0;\n"
                   << "  reg " << name << "_ready_1;\n"
                   << "  reg " << name << "_write_bank;\n"
                   << "  reg " << name << "_read_bank;\n"
                   << "  wire " << name << "_write_available = " << name
                   << "_write_bank ? !" << name << "_ready_1 : !" << name
                   << "_ready_0;\n"
                   << "  wire " << name << "_read_available = " << name
                   << "_read_bank ? " << name << "_ready_1 : " << name
                   << "_ready_0;\n"
                   << "  wire " << name << "_write_start_available = "
                   << name << "_write_available || ("
                   << sanitizeVerilogIdentifier(channel.consumer, "task")
                   << "_done && " << name << "_read_available);\n"
                   << "  wire " << name << "_read_start_available = "
                   << name << "_read_available || ("
                   << sanitizeVerilogIdentifier(channel.producer, "task")
                   << "_done && " << name << "_write_available);\n";
            for (unsigned bank = 0; bank < banks; ++bank)
            {
                const std::string bankName =
                    channelBankName(name, bank, banks);
                output << "  wire " << rangeForWidth(channel.width) << bankName
                       << "_dout;\n"
                       << "  wire " << rangeForWidth(channel.width) << bankName
                       << "_peek;\n"
                       << "  wire " << bankName << "_write;\n"
                       << "  wire " << bankName << "_read;\n";
            }
            continue;
        }
        for (unsigned instance = 0; instance < channel.instances; ++instance)
        {
            const std::string fifo = name + "_" + std::to_string(instance);
            output << "  wire " << rangeForWidth(channel.width) << fifo
                   << "_dout;\n"
                   << "  wire " << fifo << "_full;\n"
                   << "  wire " << fifo << "_empty;\n"
                   << "  wire " << fifo << "_write;\n"
                   << "  wire " << fifo << "_read;\n";
        }
    }

    std::map<std::string, std::string> taskClockEnable;
    for (const DataflowTask &task : graph.tasks)
    {
        std::vector<std::string> stalls;
        for (const DataflowChannel &channel : graph.channels)
        {
            const TaskEndpoint *endpoint = nullptr;
            bool producer = false;
            if (channel.producer == task.name)
            {
                endpoint = channelProducers[channel.name];
                producer = true;
            }
            else if (channel.consumer == task.name)
            {
                endpoint = channelConsumers[channel.name];
            }
            if (!endpoint)
                continue;
            const std::string endpointName = endpointPrefix(*endpoint);
            const std::string channelName = sanitizeVerilogIdentifier(
                "channel_" + channel.name, "channel");
            if (channel.type == "pipo")
            {
                const MemoryObject &memory = *endpoint->memory;
                for (unsigned bank = 0; bank < memory.banks; ++bank)
                {
                    if (usesScalarBanks(memory))
                    {
                        if (producer && scalarBankWrites(memory, bank))
                        {
                            const std::string scalar =
                                endpointScalarName(*endpoint, bank);
                            stalls.push_back("(" + scalar +
                                             "_ap_vld && !" + channelName +
                                             "_write_start_available)");
                        }
                        continue;
                    }
                    const int port = channelPort(memory, bank, producer);
                    if (port < 0)
                        continue;
                    const std::string signal =
                        endpointBankPrefix(*endpoint, bank);
                    const std::string ordinal = std::to_string(port);
                    stalls.push_back(
                        "(" + signal + "ce" + ordinal +
                        (producer ? " && " + signal + "we" + ordinal : "") +
                        " && !" + channelName +
                        (producer ? "_write_start_available)"
                                  : "_read_start_available)"));
                }
                continue;
            }
            std::vector<std::string> blockedInstances;
            for (unsigned instance = 0; instance < channel.instances; ++instance)
            {
                blockedInstances.push_back(
                    "(" + addressSelect(endpointName + "address0", instance,
                                         channel.instances) +
                    " && " + channelName + "_" +
                    std::to_string(instance) +
                    (producer ? "_full)" : "_empty)"));
            }
            std::string blocked = blockedInstances.front();
            for (std::size_t index = 1; index < blockedInstances.size(); ++index)
                blocked += " || " + blockedInstances[index];
            stalls.push_back("(" + endpointName + "ce0" +
                             (producer ? " && " + endpointName + "we0" : "") +
                             " && (" + blocked + "))");
        }
        for (const TaskEndpoint &endpoint : endpoints)
        {
            if (endpoint.task->name != task.name)
                continue;
            const auto *argument = dyn_cast<Argument>(endpoint.rootBase);
            if (!argument || !findTopStream(*context, *argument))
                continue;
            const std::string streamName = topMemoryBase(*argument);
            const MemoryObject &memory = *endpoint.memory;
            for (unsigned port = 0; port < memory.ports; ++port)
            {
                if (!bankPortUsed(memory, 0, port))
                    continue;
                const std::string prefix = endpointBankPrefix(endpoint, 0);
                const std::string ordinal = std::to_string(port);
                if (bankPortReads(memory, 0, port))
                {
                    stalls.push_back("(" + prefix + "ce" + ordinal +
                                     " && !" + streamName + "_empty_n)");
                }
                if (bankPortWrites(memory, 0, port))
                {
                    stalls.push_back("(" + prefix + "ce" + ordinal + " && " +
                                     prefix + "we" + ordinal + " && !" +
                                     streamName + "_full_n)");
                }
            }
        }
        std::string enable = "1'b1";
        if (!stalls.empty())
        {
            enable = "!(" + stalls.front();
            for (std::size_t index = 1; index < stalls.size(); ++index)
                enable += " || " + stalls[index];
            enable += ")";
        }
        const std::string prefix = sanitizeVerilogIdentifier(task.name, "task");
        taskClockEnable[task.name] = prefix + "_clock_enable";
        output << "  wire " << prefix << "_enable = " << enable << ";\n"
               << "  wire " << prefix
               << "_clock_enable = " << prefix << "_idle || " << prefix
               << "_enable;\n";
    }

    output << '\n';
    for (const TaskEndpoint &endpoint : endpoints)
    {
        const auto *argument = dyn_cast<Argument>(endpoint.rootBase);
        if (!argument || !findTopStream(*context, *argument))
            continue;
        const MemoryObject &memory = *endpoint.memory;
        const std::string streamName = topMemoryBase(*argument);
        const std::string prefix = endpointBankPrefix(endpoint, 0);
        const std::string enable = taskClockEnable.at(endpoint.task->name);
        if (memoryReads(memory))
        {
            output << "  reg " << rangeForWidth(memory.elementWidth) << prefix
                   << "q0_register;\n"
                   << "  assign " << prefix << "q0 = " << prefix
                   << "q0_register;\n"
                   << "  assign " << streamName << "_read = " << prefix
                   << "ce0 && " << enable << ";\n"
                   << "  always @(posedge ap_clk) begin\n"
                   << "    if (ap_rst) " << prefix << "q0_register <= 0;\n"
                   << "    else if (" << streamName << "_read) " << prefix
                   << "q0_register <= " << streamName << "_dout;\n"
                   << "  end\n";
        }
        if (memoryWrites(memory))
        {
            output << "  assign " << streamName << "_din = " << prefix
                   << "d0;\n"
                   << "  assign " << streamName << "_write = " << prefix
                   << "ce0 && " << prefix << "we0 && " << enable << ";\n";
        }
    }

    for (const DataflowChannel &channel : graph.channels)
    {
        const TaskEndpoint &producer = *channelProducers.at(channel.name);
        const TaskEndpoint &consumer = *channelConsumers.at(channel.name);
        const std::string producerPrefix = endpointPrefix(producer);
        const std::string consumerPrefix = endpointPrefix(consumer);
        const std::string name = sanitizeVerilogIdentifier(
            "channel_" + channel.name, "channel");
        if (channel.type == "pipo")
        {
            const MemoryObject &producerMemory = *producer.memory;
            const MemoryObject &consumerMemory = *consumer.memory;
            const bool scalar = usesScalarBanks(producerMemory);
            for (unsigned bank = 0; bank < producerMemory.banks; ++bank)
            {
                const std::string bankName =
                    channelBankName(name, bank, producerMemory.banks);
                std::string writeEnable = "1'b0";
                std::string writeAddress = "1'b0";
                std::string writeData =
                    std::to_string(channel.width) + "'d0";
                std::string readEnable = "1'b0";
                std::string readAddress = "1'b0";
                if (scalar)
                {
                    const std::string producerScalar =
                        endpointScalarName(producer, bank);
                    const std::string consumerScalar =
                        endpointScalarName(consumer, bank);
                    writeEnable = producerScalar + "_ap_vld && " +
                                  taskClockEnable.at(channel.producer);
                    writeData = producerScalar;
                    output << "  assign " << consumerScalar << " = "
                           << bankName << "_peek;\n";
                }
                else
                {
                    const int writePort =
                        channelPort(producerMemory, bank, true);
                    const int readPort =
                        channelPort(consumerMemory, bank, false);
                    if (writePort >= 0)
                    {
                        const std::string prefix =
                            endpointBankPrefix(producer, bank);
                        const std::string ordinal =
                            std::to_string(writePort);
                        writeEnable = prefix + "ce" + ordinal + " && " +
                                      prefix + "we" + ordinal + " && " +
                                      taskClockEnable.at(channel.producer);
                        writeAddress = prefix + "address" + ordinal;
                        writeData = prefix + "d" + ordinal;
                    }
                    if (readPort >= 0)
                    {
                        const std::string prefix =
                            endpointBankPrefix(consumer, bank);
                        const std::string ordinal = std::to_string(readPort);
                        readEnable = prefix + "ce" + ordinal + " && " +
                                     taskClockEnable.at(channel.consumer);
                        readAddress = prefix + "address" + ordinal;
                        output << "  assign " << prefix << "q" << ordinal
                               << " = " << bankName << "_dout;\n";
                    }
                }
                const unsigned long long depth =
                    std::max(1ULL, producerMemory.bankDepth);
                output << "  assign " << bankName << "_write = "
                       << writeEnable << ";\n"
                       << "  assign " << bankName << "_read = "
                       << readEnable << ";\n"
                       << "  light_hls_pipo_bank #(\n"
                       << "    .WIDTH(" << channel.width << "), .DEPTH("
                       << depth << "),\n"
                       << "    .ADDRESS_WIDTH("
                       << widthForMaximum(static_cast<unsigned>(depth - 1))
                       << "), .PEEK_ENABLED(" << (scalar ? 1 : 0) << ")\n"
                       << "  ) " << bankName << "_instance (\n"
                       << "    .clk(ap_clk), .write_bank(" << name
                       << "_write_bank), .read_bank(" << name
                       << "_read_bank),\n"
                       << "    .write_enable(" << bankName << "_write),\n"
                       << "    .write_address(" << writeAddress << "),\n"
                       << "    .data_in(" << writeData << "),\n"
                       << "    .read_enable(" << bankName << "_read),\n"
                       << "    .read_address(" << readAddress << "),\n"
                       << "    .data_out(" << bankName << "_dout),\n"
                       << "    .peek_data(" << bankName << "_peek)\n"
                       << "  );\n";
            }
            const std::string producerTask =
                sanitizeVerilogIdentifier(channel.producer, "task");
            const std::string consumerTask =
                sanitizeVerilogIdentifier(channel.consumer, "task");
            output << "  always @(posedge ap_clk) begin\n"
                   << "    if (ap_rst) begin\n"
                   << "      " << name << "_ready_0 <= 1'b0;\n"
                   << "      " << name << "_ready_1 <= 1'b0;\n"
                   << "      " << name << "_write_bank <= 1'b0;\n"
                   << "      " << name << "_read_bank <= 1'b0;\n"
                   << "    end else begin\n"
                   << "      if (" << producerTask << "_done && " << name
                   << "_write_available) begin\n"
                   << "        if (" << name << "_write_bank)\n"
                   << "          " << name << "_ready_1 <= 1'b1;\n"
                   << "        else\n"
                   << "          " << name << "_ready_0 <= 1'b1;\n"
                   << "        " << name << "_write_bank <= !" << name
                   << "_write_bank;\n"
                   << "      end\n"
                   << "      if (" << consumerTask << "_done && " << name
                   << "_read_available) begin\n"
                   << "        if (" << name << "_read_bank)\n"
                   << "          " << name << "_ready_1 <= 1'b0;\n"
                   << "        else\n"
                   << "          " << name << "_ready_0 <= 1'b0;\n"
                   << "        " << name << "_read_bank <= !" << name
                   << "_read_bank;\n"
                   << "      end\n"
                   << "    end\n"
                   << "  end\n";
            continue;
        }
        if (channel.consumerReadOnRequest)
            output << "  // The task samples the stable head on the accepted pop edge.\n"
                   << "  assign " << consumerPrefix << "q0 = "
                   << fifoMux(consumerPrefix + "address0", name, channel.instances) << ";\n";
        else
            output << "  reg " << rangeForWidth(channel.width) << consumerPrefix
                   << "q0_register;\n"
                   << "  assign " << consumerPrefix << "q0 = " << consumerPrefix
                   << "q0_register;\n";
        for (unsigned instance = 0; instance < channel.instances; ++instance)
        {
            const std::string fifo = name + "_" + std::to_string(instance);
            output << "  assign " << fifo << "_write = "
                   << producerPrefix << "ce0 && " << producerPrefix
                   << "we0 && " << taskClockEnable.at(channel.producer)
                   << " && "
                   << addressSelect(producerPrefix + "address0", instance,
                                    channel.instances)
                   << ";\n"
                   << "  assign " << fifo << "_read = " << consumerPrefix
                   << "ce0 && " << taskClockEnable.at(channel.consumer)
                   << " && "
                   << addressSelect(consumerPrefix + "address0", instance,
                                    channel.instances)
                   << ";\n"
                   << "  light_hls_fifo #(\n"
                   << "    .WIDTH(" << channel.width << "), .DEPTH("
                   << channel.depth << "),\n"
                   << "    .ADDRESS_WIDTH("
                   << widthForMaximum(channel.depth - 1)
                   << "), .COUNT_WIDTH(" << widthForMaximum(channel.depth)
                   << "), .INITIAL_TOKENS(" << channel.initialTokens << ")\n"
                   << "  ) " << fifo << "_instance (\n"
                   << "    .clk(ap_clk), .rst(ap_rst),\n"
                   << "    .write_enable(" << fifo << "_write),\n"
                   << "    .read_enable(" << fifo << "_read),\n"
                   << "    .data_in(" << producerPrefix << "d0),\n"
                   << "    .data_out(" << fifo << "_dout),\n"
                   << "    .full(" << fifo << "_full), .empty(" << fifo
                   << "_empty)\n  );\n";
        }
        std::string anyRead;
        for (unsigned instance = 0; instance < channel.instances; ++instance)
        {
            if (!anyRead.empty())
                anyRead += " || ";
            anyRead += name + "_" + std::to_string(instance) + "_read";
        }
        if (channel.consumerReadOnRequest)
            continue;
        output << "  always @(posedge ap_clk) begin\n"
               << "    if (ap_rst) " << consumerPrefix
               << "q0_register <= 0;\n"
               << "    else if (" << anyRead << ") " << consumerPrefix
               << "q0_register <= "
               << fifoMux(consumerPrefix + "address0", name,
                          channel.instances)
               << ";\n"
               << "  end\n";
    }

    for (const DataflowTask &task : graph.tasks)
    {
        const DataflowTaskImplementation &implementation =
            *findImplementation(*context, task.name);
        const std::string taskPrefix =
            sanitizeVerilogIdentifier(task.name, "task");
        std::vector<std::string> connections = {
            ".ap_clk(ap_clk)", ".ap_rst(ap_rst)",
            ".ap_ce(" + taskPrefix + "_clock_enable)",
            ".ap_start(" + taskPrefix + "_start)",
            ".ap_done(" + taskPrefix + "_done)",
            ".ap_idle(" + taskPrefix + "_idle)",
            ".ap_ready(" + taskPrefix + "_ready)"};
        for (unsigned index = 0; index < task.call->arg_size(); ++index)
        {
            const Argument *childArgument = task.functionValue->getArg(index);
            const Value *rootOperand = task.call->getArgOperand(index);
            if (scalarBitWidth(childArgument->getType()))
            {
                const std::string port = sanitizeVerilogPortIdentifier(
                    childArgument->getName().str(),
                    "arg" + std::to_string(index));
                const auto *rootArgument = dyn_cast<Argument>(rootOperand);
                connections.push_back(
                    "." + port + "(" +
                    (rootArgument
                         ? sanitizeVerilogPortIdentifier(
                               rootArgument->getName().str(), "argument") +
                               "_transaction_queue[" + taskPrefix +
                               "_read_bank]"
                         : constantConnection(rootOperand)) +
                    ")");
                if (rootArgument)
                {
                    const std::string argumentName =
                        sanitizeVerilogPortIdentifier(
                            rootArgument->getName().str(), "argument");
                    connections.back() =
                        "." + port + "(((" + taskPrefix +
                        "_pending == 0) && accept_transaction) ? " +
                        argumentName + " : " + argumentName +
                        "_transaction_queue[" + taskPrefix +
                        "_read_bank ^ " + taskPrefix + "_done])";
                }
                continue;
            }
            const MemoryObject *memory =
                findArgumentMemory(implementation, index);
            if (!memory)
                continue;
            const Value *base = getUnderlyingObject(rootOperand);
            const bool channel = findChannel(graph, base) != nullptr;
            const TaskEndpoint endpoint =
                {&task, &implementation, memory, base, index};
            const auto *topArgument = dyn_cast<Argument>(base);
            const bool publicStream =
                topArgument && findTopStream(*context, *topArgument);
            for (unsigned bank = 0; bank < memory->banks; ++bank)
            {
                if (usesScalarBanks(*memory))
                {
                    const bool reads = scalarBankReads(*memory, bank);
                    const bool writes = scalarBankWrites(*memory, bank);
                    const std::string childScalar =
                        memoryScalarName(*memory, bank);
                    const std::string signalScalar =
                        channel ? endpointScalarName(endpoint, bank)
                                : topMemoryScalarName(*topArgument, bank);
                    auto scalarValid = [&](const std::string &signal) {
                        if (channel)
                            return signal;
                        const std::string request = taskPrefix + "__" + signal + "_request";
                        output << "  wire " << request << ";\n"
                               << "  assign " << signal << " = " << request
                               << " && " << taskPrefix << "_clock_enable && !ap_rst;\n";
                        return request;
                    };
                    if (reads && !writes)
                    {
                        connections.push_back("." + childScalar + "(" +
                                              signalScalar + ")");
                    }
                    else if (writes && !reads)
                    {
                        connections.push_back("." + childScalar + "(" +
                                              signalScalar + ")");
                        connections.push_back("." + childScalar +
                                              "_ap_vld(" + scalarValid(signalScalar + "_ap_vld") + ")");
                    }
                    else if (reads && writes)
                    {
                        connections.push_back("." + childScalar + "_i(" +
                                              signalScalar + "_i)");
                        connections.push_back("." + childScalar + "_o(" +
                                              signalScalar + "_o)");
                        connections.push_back("." + childScalar +
                                              "_o_ap_vld(" + scalarValid(signalScalar + "_o_ap_vld") + ")");
                    }
                    continue;
                }
                const std::string childPrefix =
                    memoryBankPrefix(*memory, bank);
                const std::string signalPrefix =
                    (channel || publicStream)
                            ? endpointBankPrefix(endpoint, bank)
                            : topMemoryBankPrefix(*topArgument, *memory, bank);
                for (unsigned port = 0; port < memory->ports; ++port)
                {
                    if (!bankPortUsed(*memory, bank, port))
                        continue;
                    const std::string ordinal = std::to_string(port);
                    connections.push_back("." + childPrefix + "address" +
                                          ordinal + "(" + signalPrefix +
                                          "address" + ordinal + ")");
                    std::string enableSignal = signalPrefix + "ce" + ordinal;
                    if (!channel && !publicStream)
                    {
                        const std::string request = taskPrefix + "__" + childPrefix + "ce" + ordinal + "_request";
                        // Internal requests remain visible to stall analysis,
                        // but external RAM must advance only with its owner.
                        output << "  wire " << request << ";\n"
                               << "  assign " << enableSignal << " = " << request
                               << " && " << taskPrefix << "_clock_enable && !ap_rst;\n";
                        enableSignal = request;
                    }
                    connections.push_back("." + childPrefix + "ce" + ordinal +
                                          "(" + enableSignal + ")");
                    if (bankPortWrites(*memory, bank, port))
                    {
                        connections.push_back("." + childPrefix + "we" +
                                              ordinal + "(" + signalPrefix +
                                              "we" + ordinal + ")");
                        connections.push_back("." + childPrefix + "d" +
                                              ordinal + "(" + signalPrefix +
                                              "d" + ordinal + ")");
                    }
                    if (bankPortReads(*memory, bank, port))
                    {
                        connections.push_back("." + childPrefix + "q" +
                                              ordinal + "(" + signalPrefix +
                                              "q" + ordinal + ")");
                    }
                }
            }
        }
        output << "\n  " << implementation.moduleName << " " << taskPrefix
               << "_instance (\n";
        for (std::size_t index = 0; index < connections.size(); ++index)
            output << "    " << connections[index]
                   << (index + 1 == connections.size() ? "\n" : ",\n");
        output << "  );\n";
    }

    std::string allTerminalCredits;
    for (const DataflowTask *task : terminalTasks)
    {
        const std::string prefix =
            sanitizeVerilogIdentifier(task->name, "task");
        if (!allTerminalCredits.empty())
            allTerminalCredits += " && ";
        if (multipleTerminalTasks)
        {
            allTerminalCredits += "((" + prefix +
                                  "_completion_credit != 0) || " + prefix +
                                  "_done)";
        }
        else
        {
            allTerminalCredits += prefix + "_done";
        }
    }
    output << "\n  assign retire_transaction = "
           << "(outstanding_transactions != 0) && "
           << allTerminalCredits << ";\n"
           << "\n  always @(posedge ap_clk) begin\n"
           << "    if (ap_rst) begin\n"
           << "      outstanding_transactions <= 0;\n";
    if (hasScalarTransactionArguments)
        output << "      transaction_write_bank <= 1'b0;\n";
    for (const DataflowTask &task : graph.tasks)
    {
        const std::string prefix = sanitizeVerilogIdentifier(task.name, "task");
        output << "      " << prefix << "_pending <= 0;\n";
        if (scalarTransactionTasks.count(task.name) != 0)
            output << "      " << prefix << "_read_bank <= 1'b0;\n";
    }
    if (multipleTerminalTasks)
    {
        for (const DataflowTask *task : terminalTasks)
        {
            const std::string prefix =
                sanitizeVerilogIdentifier(task->name, "task");
            output << "      " << prefix << "_completion_credit <= 0;\n";
        }
    }
    output << "    end else begin\n"
           << "      case ({accept_transaction, retire_transaction})\n"
           << "        2'b10: outstanding_transactions <= "
              "outstanding_transactions + 1'b1;\n"
           << "        2'b01: outstanding_transactions <= "
              "outstanding_transactions - 1'b1;\n"
           << "        default: outstanding_transactions <= "
              "outstanding_transactions;\n"
           << "      endcase\n"
           << "      if (accept_transaction) begin\n";
    for (const Argument &argument : context->top->args())
    {
        if (!scalarBitWidth(argument.getType()))
            continue;
        const std::string name = sanitizeVerilogPortIdentifier(
            argument.getName().str(), "argument");
        output << "        " << name
               << "_transaction_queue[transaction_write_bank] <= " << name
               << ";\n";
    }
    if (hasScalarTransactionArguments)
        output << "        transaction_write_bank <= !transaction_write_bank;\n";
    output << "      end\n";
    for (const DataflowTask &task : graph.tasks)
    {
        const std::string prefix = sanitizeVerilogIdentifier(task.name, "task");
        output << "      case ({accept_transaction, " << prefix
               << "_start})\n"
               << "        2'b10: " << prefix << "_pending <= " << prefix
               << "_pending + 1'b1;\n"
               << "        2'b01: " << prefix << "_pending <= " << prefix
               << "_pending - 1'b1;\n"
               << "        default: " << prefix << "_pending <= " << prefix
               << "_pending;\n"
               << "      endcase\n";
        if (scalarTransactionTasks.count(task.name) != 0)
            output << "      if (" << prefix << "_done) " << prefix
                   << "_read_bank <= !" << prefix << "_read_bank;\n";
    }
    if (multipleTerminalTasks)
    {
        for (const DataflowTask *task : terminalTasks)
        {
            const std::string prefix =
                sanitizeVerilogIdentifier(task->name, "task");
            output << "      case ({" << prefix
                   << "_done, retire_transaction})\n"
                   << "        2'b10: " << prefix
                   << "_completion_credit <= " << prefix
                   << "_completion_credit + 1'b1;\n"
                   << "        2'b01: " << prefix
                   << "_completion_credit <= " << prefix
                   << "_completion_credit - 1'b1;\n"
                   << "        default: " << prefix
                   << "_completion_credit <= " << prefix
                   << "_completion_credit;\n"
                   << "      endcase\n";
        }
    }
    output << "    end\n"
           << "  end\nendmodule\n";

    context->schedule.valid = true;
    context->schedule.stateCount = 2;
    context->schedule.latencyCycles =
        graph.firstTransactionLatencyExact
            ? graph.firstTransactionLatencyLowerBound
            : -1;
    context->schedule.achievedII =
        graph.graphInitiationInterval > 0
            ? static_cast<long long>(graph.graphInitiationInterval)
            : context->estimate.initiationInterval;
    context->binding.valid = true;
    context->binding.registerCount = 1;
    context->binding.registerBits = 2;
    for (const DataflowTask &task : graph.tasks)
    {
        ++context->binding.registerCount;
        context->binding.registerBits +=
            pipoConsumerTasks.count(task.name) != 0 ? 2 : 1;
        if (scalarTransactionTasks.count(task.name) != 0)
        {
            ++context->binding.registerCount;
            ++context->binding.registerBits;
        }
    }
    if (hasScalarTransactionArguments)
    {
        ++context->binding.registerCount;
        ++context->binding.registerBits;
        for (const Argument &argument : context->top->args())
        {
            if (!scalarBitWidth(argument.getType()))
                continue;
            context->binding.registerCount += 2;
            context->binding.registerBits +=
                2 * scalarBitWidth(argument.getType());
        }
    }
    if (multipleTerminalTasks)
    {
        context->binding.registerCount += terminalTasks.size();
        context->binding.registerBits += 2 * terminalTasks.size();
    }
    for (const DataflowChannel &channel : graph.channels)
    {
        if (channel.type != "pipo")
            continue;
        context->binding.registerCount += 4;
        context->binding.registerBits += 4;
    }
    context->binding.stateRegisterBits = 1;
    context->rtl.valid = true;
    context->rtl.dataflow = graph;
    context->rtl.interfaces = {RTLInterfaceProtocol::APControlHS,
                               RTLInterfaceProtocol::APMemory,
                               RTLInterfaceProtocol::FIFO};
    context->rtl.verilog = output.str();
    return false;
}

void HI_DataflowRTLGeneration::getAnalysisUsage(AnalysisUsage &usage) const
{
    usage.setPreservesAll();
}
