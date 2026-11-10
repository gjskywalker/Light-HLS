#include "HI_BackendContext.h"
#include "HI_PipelineValueLifetime.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Constants.h"

#include "llvm/Demangle/Demangle.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>

namespace light_hls
{
namespace backend
{

namespace
{

std::string jsonEscape(const std::string &value)
{
    std::ostringstream output;
    for (unsigned char character : value)
    {
        switch (character)
        {
        case '\\':
            output << "\\\\";
            break;
        case '"':
            output << "\\\"";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (character < 0x20)
            {
                output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<unsigned>(character) << std::dec;
            }
            else
            {
                output << static_cast<char>(character);
            }
        }
    }
    return output.str();
}

std::string jsonString(const std::string &value)
{
    return "\"" + jsonEscape(value) + "\"";
}

void writeStringArray(std::ostream &output,
                      const std::vector<std::string> &values)
{
    output << '[';
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        if (index)
            output << ',';
        output << jsonString(values[index]);
    }
    output << ']';
}

std::vector<std::string> memoryPortSignals(const MemoryObject &memory,
                                           unsigned port)
{
    if (port >= memory.portUsage.size())
        return {};
    const MemoryPortUsage &usage = memory.portUsage[port];
    if (memory.storage == MemoryStorageKind::LocalRAM)
        return {"address", "ce", "we", "d", "q"};
    if (memory.banks > 1 && memory.bankDepth == 1 &&
        memory.banks == memory.depth)
    {
        if (usage.hasRead && usage.hasWrite)
            return {"scalar_i", "scalar_o", "ap_vld"};
        if (usage.hasRead)
            return {"scalar_input"};
        if (usage.hasWrite)
            return {"scalar_output", "ap_vld"};
        return {};
    }
    if (!usage.hasRead && !usage.hasWrite)
        return {};
    std::vector<std::string> signals = {"address", "ce"};
    if (usage.hasWrite)
    {
        signals.push_back("we");
        signals.push_back("d");
    }
    if (usage.hasRead)
        signals.push_back("q");
    return signals;
}

MemoryAccessMode portAccessMode(const MemoryObject &memory, unsigned port)
{
    if (port >= memory.portUsage.size())
        return MemoryAccessMode::Unused;
    const MemoryPortUsage &usage = memory.portUsage[port];
    if (usage.hasRead && usage.hasWrite)
        return MemoryAccessMode::ReadWrite;
    if (usage.hasRead)
        return MemoryAccessMode::ReadOnly;
    if (usage.hasWrite)
        return MemoryAccessMode::WriteOnly;
    return MemoryAccessMode::Unused;
}

std::string memoryPortUsageSummary(const MemoryObject &memory)
{
    if (memory.portUsage.empty())
        return "-";
    std::ostringstream output;
    for (unsigned port = 0; port < memory.portUsage.size(); ++port)
    {
        if (port)
            output << ", ";
        output << 'P' << port << ':' << memoryAccessModeName(
            portAccessMode(memory, port));
    }
    return output.str();
}

MemoryAccessMode bankPortAccessMode(const MemoryObject &memory, unsigned bank,
                                    unsigned port)
{
    if (bank >= memory.bankPortUsage.size() ||
        port >= memory.bankPortUsage[bank].size())
    {
        return MemoryAccessMode::Unused;
    }
    const MemoryPortUsage &usage = memory.bankPortUsage[bank][port];
    if (usage.hasRead && usage.hasWrite)
        return MemoryAccessMode::ReadWrite;
    if (usage.hasRead)
        return MemoryAccessMode::ReadOnly;
    if (usage.hasWrite)
        return MemoryAccessMode::WriteOnly;
    return MemoryAccessMode::Unused;
}

std::string unsignedList(const std::vector<unsigned> &values,
                         const std::string &opening = "{",
                         const std::string &closing = "}")
{
    if (values.empty())
        return "-";
    std::ostringstream output;
    output << opening;
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        if (index)
            output << ',';
        output << values[index];
    }
    output << closing;
    return output.str();
}

std::string dimensionList(const std::vector<unsigned long long> &dimensions)
{
    if (dimensions.empty())
        return "unknown";
    std::ostringstream output;
    for (std::size_t index = 0; index < dimensions.size(); ++index)
    {
        if (index)
            output << " x ";
        output << dimensions[index];
    }
    return output.str();
}

std::string partitionLayoutSummary(const MemoryObject &memory)
{
    if (memory.partitionDimensions.empty())
        return "none";
    std::ostringstream output;
    for (std::size_t index = 0; index < memory.partitionDimensions.size();
         ++index)
    {
        if (index)
            output << ", ";
        const MemoryPartitionDimension &partition =
            memory.partitionDimensions[index];
        output << 'D' << (partition.dimension + 1) << ':'
               << partitionKindName(partition.kind) << " x"
               << partition.factor << " (" << partition.extent << "->"
               << partition.localExtent << ')';
    }
    return output.str();
}

std::string bankPortUsageSummary(const MemoryObject &memory)
{
    if (memory.bankPortUsage.empty())
        return "-";
    std::ostringstream output;
    bool first = true;
    for (unsigned bank = 0; bank < memory.bankPortUsage.size(); ++bank)
    {
        for (unsigned port = 0; port < memory.bankPortUsage[bank].size();
             ++port)
        {
            const MemoryAccessMode mode =
                bankPortAccessMode(memory, bank, port);
            if (mode == MemoryAccessMode::Unused)
                continue;
            if (!first)
                output << ", ";
            first = false;
            output << 'B' << bank << "/P" << port << ':'
                   << memoryAccessModeName(mode);
        }
    }
    return first ? "-" : output.str();
}

std::string memoryBankInterface(const MemoryObject &memory)
{
    if (memory.storage == MemoryStorageKind::LocalRAM)
        return "local_ram";
    if (memory.banks > 1 && memory.bankDepth == 1 &&
        memory.banks == memory.depth)
    {
        return "scalar";
    }
    return "ap_memory";
}

std::string scheduledOperationName(const HLSBackendContext &context,
                                   const llvm::Instruction *instruction)
{
    if (!instruction)
        return "-";
    const auto scheduled = context.schedule.instructionMap.find(instruction);
    if (scheduled != context.schedule.instructionMap.end() &&
        scheduled->second)
    {
        return scheduled->second->name;
    }
    const auto value = context.valueNames.find(instruction);
    if (value != context.valueNames.end())
        return value->second;
    return instruction->getOpcodeName();
}

std::string scalarRecurrenceSinkNames(
    const HLSBackendContext &context,
    const ScalarRecurrenceDependence &dependence)
{
    std::ostringstream output;
    for (std::size_t index = 0; index < dependence.sinks.size(); ++index)
    {
        if (index)
            output << ',';
        output << scheduledOperationName(context, dependence.sinks[index]);
    }
    return output.str().empty() ? "-" : output.str();
}

void writeCommonFields(std::ostream &output, const HLSBackendContext &context,
                       const std::string &stage, bool valid)
{
    output << "\"schema_version\":\"1.0\","
           << "\"stage\":" << jsonString(stage) << ','
           << "\"valid\":" << (valid ? "true" : "false") << ','
           << "\"design\":" << jsonString(context.configuration.design) << ','
           << "\"top_function\":"
           << jsonString(context.configuration.topFunction) << ','
           << "\"clock_period_ns\":" << context.configuration.clockPeriodNs << ','
           << "\"target_frequency_mhz\":";
    if (context.configuration.clockPeriodNs > 0.0)
        output << 1000.0 / context.configuration.clockPeriodNs;
    else
        output << "null";
    output << ','
           << "\"board\":" << jsonString(context.configuration.board) << ','
           << "\"source_hash\":" << jsonString(context.configuration.sourceHash)
           << ',';

    output << "\"pragma_configuration\":{";
    output << "\"unroll\":{";
    for (std::size_t index = 0; index < context.configuration.pragmas.unroll.size();
         ++index)
    {
        if (index)
            output << ',';
        output << jsonString(context.configuration.pragmas.unroll[index].first)
               << ':' << context.configuration.pragmas.unroll[index].second;
    }
    output << "},\"pipeline\":{";
    for (std::size_t index = 0;
         index < context.configuration.pragmas.pipeline.size(); ++index)
    {
        if (index)
            output << ',';
        output << jsonString(context.configuration.pragmas.pipeline[index].first)
               << ':' << context.configuration.pragmas.pipeline[index].second;
    }
    output << "},\"array_partition_count\":"
           << context.configuration.pragmas.arrayPartitionCount
           << ",\"array_partitions\":[";
    for (std::size_t index = 0;
         index < context.configuration.arrayPartitions.size(); ++index)
    {
        if (index)
            output << ',';
        const ArrayPartitionDirective &partition =
            context.configuration.arrayPartitions[index];
        output << "{\"scope\":" << jsonString(partition.scope)
               << ",\"memory\":" << jsonString(partition.memory)
               << ",\"kind\":"
               << jsonString(partitionKindName(partition.kind))
               << ",\"dimension\":" << partition.dimension
               << ",\"factor\":" << partition.factor << '}';
    }
    output << ']'
           << ",\"dataflow_count\":"
           << context.configuration.pragmas.dataflowCount
           << ",\"stream_count\":"
           << context.configuration.pragmas.streamCount << "},";

    output << "\"memory_depth_configuration\":{";
    bool firstDepth = true;
    for (const auto &depth : context.configuration.configuredMemoryDepths)
    {
        if (!firstDepth)
            output << ',';
        firstDepth = false;
        output << jsonString(depth.first) << ':' << depth.second;
    }
    output << "},";

    output << "\"memory_interface_configuration\":{";
    bool firstInterface = true;
    for (const auto &ports : context.configuration.configuredMemoryPorts)
    {
        if (!firstInterface)
            output << ',';
        firstInterface = false;
        const auto latency =
            context.configuration.configuredMemoryReadLatencies.find(ports.first);
        output << jsonString(ports.first) << ":{\"ports\":" << ports.second
               << ",\"read_latency\":"
               << (latency ==
                           context.configuration.configuredMemoryReadLatencies.end()
                       ? 1
                       : latency->second)
               << '}';
    }
    output << "},";

    output << "\"pass_sequence\":";
    writeStringArray(output, context.configuration.passSequence);
    output << ',';

    output << "\"warnings\":[";
    bool first = true;
    for (const BackendDiagnostic &diagnostic : context.diagnostics)
    {
        if (diagnostic.severity != DiagnosticSeverity::Warning)
            continue;
        if (!first)
            output << ',';
        first = false;
        output << "{\"code\":" << jsonString(diagnostic.code)
               << ",\"message\":" << jsonString(diagnostic.message)
               << ",\"location\":" << jsonString(diagnostic.location) << '}';
    }
    output << "],\"errors\":[";
    first = true;
    for (const BackendDiagnostic &diagnostic : context.diagnostics)
    {
        if (diagnostic.severity != DiagnosticSeverity::Error)
            continue;
        if (!first)
            output << ',';
        first = false;
        output << "{\"code\":" << jsonString(diagnostic.code)
               << ",\"message\":" << jsonString(diagnostic.message)
               << ",\"location\":" << jsonString(diagnostic.location) << '}';
    }
    output << ']';
}

bool writeFile(const std::filesystem::path &path, const std::string &content,
               std::string &errorMessage)
{
    std::ofstream output(path);
    if (!output)
    {
        errorMessage = "cannot open output file '" + path.string() + "'";
        return false;
    }
    output << content;
    if (!output)
    {
        errorMessage = "cannot write output file '" + path.string() + "'";
        return false;
    }
    return true;
}

using ReportRows = std::vector<std::vector<std::string>>;

std::string reportCell(std::string value, std::size_t width)
{
    if (value.size() <= width)
        return value;
    if (width <= 3)
        return value.substr(0, width);
    return value.substr(0, width - 3) + "...";
}

void writeReportTable(std::ostream &output,
                      const std::vector<std::string> &headings,
                      const std::vector<std::size_t> &widths,
                      const ReportRows &rows)
{
    auto border = [&]() {
        output << '+';
        for (std::size_t width : widths)
            output << std::string(width + 2, '-') << '+';
        output << '\n';
    };
    auto row = [&](const std::vector<std::string> &cells) {
        output << '|';
        for (std::size_t index = 0; index < widths.size(); ++index)
        {
            const std::string value =
                index < cells.size() ? reportCell(cells[index], widths[index])
                                     : "";
            output << ' ' << std::left << std::setw(widths[index]) << value
                   << " |";
        }
        output << '\n';
    };

    border();
    if (!headings.empty())
    {
        row(headings);
        border();
    }
    if (rows.empty())
        row({"(none)"});
    else
        for (const auto &entry : rows)
            row(entry);
    border();
}

void writeReportTitle(std::ostream &output, const std::string &title,
                      const HLSBackendContext &context)
{
    std::ostringstream clockPeriod;
    clockPeriod << std::fixed << std::setprecision(3)
                << context.configuration.clockPeriodNs << " ns";
    std::ostringstream targetFrequency;
    if (context.configuration.clockPeriodNs > 0.0)
    {
        targetFrequency << std::fixed << std::setprecision(3)
                        << 1000.0 / context.configuration.clockPeriodNs
                        << " MHz";
    }
    else
    {
        targetFrequency << '-';
    }
    output << "================================================================\n"
           << "== Light-HLS " << title << '\n'
           << "================================================================\n";
    writeReportTable(
        output, {"Field", "Value"}, {20, 55},
        {{"Design", context.configuration.design},
         {"Top Function", context.configuration.topFunction},
         {"Target Board", context.configuration.board},
         {"Target Frequency", targetFrequency.str()},
         {"Clock Period", clockPeriod.str()}});
    output << "Source: " << context.configuration.sourcePath << '\n';
}

void writeReportSection(std::ostream &output, const std::string &title)
{
    output << "\n+ " << title << ":\n\n";
}

std::string yesNo(bool value)
{
    return value ? "yes" : "no";
}

std::string integerText(long long value)
{
    return value < 0 ? "-" : std::to_string(value);
}

std::string doubleText(double value, unsigned precision = 3)
{
    std::ostringstream output;
    output << std::fixed << std::setprecision(precision) << value;
    return output.str();
}

void writeDiagnostics(std::ostream &output, const HLSBackendContext &context)
{
    writeReportSection(output, "Diagnostics");
    if (context.diagnostics.empty())
    {
        output << "No warnings or errors.\n";
        return;
    }
    for (const BackendDiagnostic &diagnostic : context.diagnostics)
    {
        output << (diagnostic.severity == DiagnosticSeverity::Error ? "ERROR"
                                                                     : "WARNING")
               << " [" << diagnostic.code << "] " << diagnostic.message << '\n';
        if (!diagnostic.location.empty())
            output << "  Location: " << diagnostic.location << '\n';
    }
}

std::string makeEstimateReport(const HLSBackendContext &context)
{
    std::ostringstream output;
    writeReportTitle(output, "Pre-Backend Estimate", context);
    writeReportSection(output, "Performance Estimates");
    writeReportTable(
        output, {"Metric", "Value", "Interpretation"}, {24, 14, 38},
        {{"Valid", yesNo(context.estimate.valid), "Heuristic model status"},
         {"Latency", integerText(context.estimate.latencyCycles), "cycles"},
         {"Initiation Interval",
          std::to_string(context.estimate.initiationInterval), "cycles"}});
    writeReportSection(output, "Resource Estimates");
    writeReportTable(
        output, {"Resource", "Estimate", "QoR Kind"}, {24, 14, 38},
        {{"DSP", integerText(context.estimate.dsp), "heuristic estimate"},
         {"FF", integerText(context.estimate.ff), "heuristic estimate"},
         {"LUT", integerText(context.estimate.lut), "heuristic estimate"},
         {"BRAM", integerText(context.estimate.bram), "heuristic estimate"}});
    writeDiagnostics(output, context);
    return output.str();
}

std::string makeScheduleReport(const HLSBackendContext &context)
{
    std::ostringstream output;
    writeReportTitle(output, "Operation Schedule", context);
    writeReportSection(output, "Schedule Summary");
    writeReportTable(
        output, {"Metric", "Value", "Description"}, {28, 14, 38},
        {{"Valid", yesNo(context.schedule.valid && !context.hasErrors()),
          "Scheduler status"},
         {"Latency", integerText(context.schedule.latencyCycles),
          context.schedule.hasControlFlowCycle ? "dynamic control-flow cycle"
                                               : "static acyclic schedule"},
         {"Achieved II", std::to_string(context.schedule.achievedII), "cycles"},
         {"FSM States", std::to_string(context.schedule.stateCount),
          "scheduled control states"},
         {"Memory Port Conflicts",
          std::to_string(context.schedule.memoryPortConflicts), "conflicts"},
         {"Serialization Cycles",
          std::to_string(context.schedule.memoryPortSerializationCycles),
          "cycles added for memory ports"},
         {"Memory Ordering Cycles", std::to_string(context.schedule.memoryOrderingCycles),
          "request-edge alias constraints"},
         {"Forwarded Block Loads", std::to_string(context.forwardedLoads.size()),
          "response-ready sampling without a capture register"},
         {"Hardware Constants", std::to_string(context.constantValues.size()),
          "object-relative constants without setup states or registers"},
         {"Combinational Values", std::to_string(context.combinationalValues.size()),
          "SSA values without dedicated registers"},
         {"Ordinary Feedback Wires", std::to_string(context.ordinaryFeedbackValues.size()),
          "constant-step updates sampled at the unique loop latch"},
         {"Backedge Bypasses", std::to_string(context.ordinaryBackedgeBypasses.size()),
          "same-edge operator-to-PHI paths; result values retained"},
         {"Shared Recurrence Registers", std::to_string(context.ordinaryRegisterAliases.size()),
          "PHI/result storage shared only without old-PHI live-outs"},
         {"Held Live-out PHIs", std::to_string(context.ordinaryLiveOutExpressionRoots.size()),
          "old recurrence values preserved for exit expressions"},
         {"Fused Ordinary Returns", std::to_string(context.ordinaryReturnExits.size()),
          "timed combinational exits completed on the incoming branch edge"},
         {"Fused Ordinary Entry", context.ordinaryEntrySuccessor ? "yes" : "no",
          "PHIs initialized from constants/live ports on accepted start"},
         {"Read-to-PHI Bypasses", std::to_string(context.ordinaryReadPhiBypasses.size()),
          "response capture and successor PHI share an edge; load registers retained"},
         {"Chain Delay Budget (ns)", std::to_string(context.configuration.clockPeriodNs * 0.7),
          "heuristic; verify post-synthesis timing"}});

    ReportRows feedbackRows;
    // Iterate in IR order for reproducible reports, not pointer-map order.
    if (context.top)
        for (const llvm::BasicBlock &block : *context.top)
            for (const llvm::Instruction &instruction : block)
            {
                const auto feedback = context.ordinaryFeedbackValues.find(&instruction);
                if (feedback != context.ordinaryFeedbackValues.end())
                    feedbackRows.push_back({context.valueNames.at(&instruction),
                        context.valueNames.at(feedback->second),
                        std::to_string(instruction.getType()->getIntegerBitWidth()),
                        feedback->second->getParent() == &block ?
                            "ordinary self-backedge" : "ordinary loop latch"});
            }
    writeReportSection(output, "Ordinary Feedback Wires");
    writeReportTable(output, {"Update", "Held PHI", "IR Bits", "Sampling Edge"},
                     {32, 32, 10, 25}, feedbackRows);

    ReportRows bypassRows;
    if (context.top)
        for (const llvm::BasicBlock &block : *context.top)
            for (const llvm::PHINode &phi : block.phis())
            {
                const auto bypass = context.ordinaryBackedgeBypasses.find(&phi);
                if (bypass != context.ordinaryBackedgeBypasses.end())
                {
                    const auto scheduled = context.schedule.instructionMap.find(bypass->second);
                    bypassRows.push_back({context.valueNames.at(&phi),
                        context.valueNames.at(bypass->second),
                        scheduled == context.schedule.instructionMap.end() ? "not scheduled" :
                            std::to_string(scheduled->second->startCycle),
                        context.ordinaryRegisterAliases.count(bypass->second) ? "shared with PHI" : "retained"});
                }
            }
    writeReportSection(output, "Ordinary Backedge Bypasses");
    writeReportTable(output, {"Held PHI", "Registered Update", "Issue/Sample Cycle", "Update Register"},
                     {32, 32, 18, 18}, bypassRows);

    ReportRows readPhiRows;
    for (const OrdinaryReadPhiBypass &bypass : context.ordinaryReadPhiBypasses)
    {
        const auto scheduled = context.schedule.instructionMap.find(bypass.load);
        readPhiRows.push_back({bypass.predecessor->getName().str(),
            context.valueNames.at(bypass.phi), context.valueNames.at(bypass.load),
            scheduled == context.schedule.instructionMap.end() ? "not scheduled" :
                std::to_string(scheduled->second->startCycle + scheduled->second->latency - 1),
            "retained"});
    }
    writeReportSection(output, "Ordinary Read-to-PHI Bypasses");
    writeReportTable(output, {"Predecessor", "Successor PHI", "Load", "Capture/Sample Cycle", "Load Register"},
                     {24, 32, 32, 20, 16}, readPhiRows);

    ReportRows blockRows;
    ReportRows instructionRows;
    for (const BlockSchedule &block : context.schedule.blocks)
    {
        blockRows.push_back({block.name, std::to_string(block.firstState),
                             std::to_string(block.stateCount),
                             std::to_string(block.instructions.size())});
        for (const InstructionSchedule &instruction : block.instructions)
        {
            instructionRows.push_back(
                {block.name, instruction.name, instruction.opcode,
                 std::to_string(instruction.startCycle),
                 std::to_string(instruction.latency),
                 std::to_string(instruction.resultWidth)});
        }
    }
    writeReportSection(output, "Basic-Block Schedule");
    writeReportTable(output, {"Block", "First State", "States", "Operations"},
                     {32, 12, 10, 12}, blockRows);
    writeReportSection(output, "Operation Schedule");
    writeReportTable(output,
                     {"Block", "Operation", "Opcode", "Start", "Latency",
                      "Width"},
                     {18, 24, 12, 8, 8, 8}, instructionRows);
    if (!context.schedule.pipelines.empty())
    {
        ReportRows pipelineRows;
        for (const LoopPipelineInfo &pipeline : context.schedule.pipelines)
        {
            pipelineRows.push_back(
                {pipeline.label,
                 std::to_string(pipeline.requestedII),
                 std::to_string(pipeline.achievedII),
                 std::to_string(pipeline.memoryMII),
                 std::to_string(pipeline.memoryDependenceMII),
                 std::to_string(pipeline.recurrenceMII),
                 std::to_string(pipeline.stageCount),
                 std::to_string(pipeline.contextValues.size()),
                 std::to_string(pipeline.tripCount),
                 pipeline.limitingFactor});
        }
        writeReportSection(output, "Pipeline Schedule");
        writeReportTable(
            output,
            {"Loop", "Requested", "Achieved", "MemMII", "DepMII",
             "RecMII", "Stages", "Contexts", "Trip Count",
             "Limiting Constraint"},
            {24, 10, 10, 8, 8, 8, 8, 10, 12, 38}, pipelineRows);
    }
    writeDiagnostics(output, context);
    return output.str();
}

std::string makePipelineScheduleReport(const HLSBackendContext &context)
{
    std::ostringstream output;
    writeReportTitle(output, "Loop Pipeline Schedule", context);
    writeReportSection(output, "Pipeline Summary");
    if (context.schedule.pipelines.empty())
    {
        output << "No pipelined loops.\n";
        writeDiagnostics(output, context);
        return output.str();
    }

    ReportRows rows;
    for (const LoopPipelineInfo &pipeline : context.schedule.pipelines)
    {
        rows.push_back(
            {pipeline.label,
             std::to_string(pipeline.requestedII),
             std::to_string(pipeline.achievedII),
             std::to_string(pipeline.memoryMII),
             std::to_string(pipeline.memoryDependenceMII),
             std::to_string(pipeline.recurrenceMII),
             std::to_string(pipeline.pipelineLatency),
             std::to_string(pipeline.stageCount),
             std::to_string(pipeline.contextValues.size()),
             std::to_string(pipeline.tripCount),
             pipeline.limitingFactor});
    }
    writeReportTable(
        output,
        {"Loop", "Req II", "Ach II", "MemMII", "DepMII", "RecMII",
         "Latency", "Stages", "Contexts", "Trips", "Limiting Constraint"},
        {24, 8, 8, 8, 8, 8, 9, 8, 10, 8, 36}, rows);

    for (const LoopPipelineInfo &pipeline : context.schedule.pipelines)
    {
        output << "\nLoop normalization: " << pipeline.label << "\n"
               << "  Source trips: " << pipeline.sourceTripCount
               << "; hardware trips: " << pipeline.tripCount
               << "; flattened loops: " << pipeline.flattenedLoopCount << "\n"
               << "  " << pipeline.flatteningReason << "\n";
        output << "  Forwarded memory responses: " << pipeline.forwardedLoads.size()
               << " (sampled directly on the response-ready edge)\n";
        output << "  Iteration controller: "
               << (pipeline.stageIndexedController ? "fixed stage tags" : "context ring") << "\n";
        output << "  Launch initialization: "
               << (pipeline.initializedAtEntry ? "ap_start acceptance edge" : "pipeline entry state")
               << "; retirement edge: " << pipeline.pipelineLatency << "\n";
        ReportRows storageRows;
        for (const llvm::Instruction &instruction : *pipeline.header)
        {
            if (!pipeline.contextValues.count(&instruction))
                continue;
            const auto interval = pipeline.valueLiveIntervals.find(&instruction);
            const auto alias = pipeline.storageAliases.find(&instruction);
            storageRows.push_back({scheduledOperationName(context, &instruction),
                alias != pipeline.storageAliases.end() ? "alias" :
                    std::to_string(pipelineValueStorageSlots(pipeline, &instruction)),
                std::to_string(pipeline.contextSlots),
                alias != pipeline.storageAliases.end() ? "shares " + scheduledOperationName(context,
                    llvm::cast<llvm::Instruction>(alias->second)) :
                pipeline.retirementOnlyValues.count(&instruction) ? "retirement only" :
                interval == pipeline.valueLiveIntervals.end() ? "fallback" :
                    std::to_string(interval->second.first) + " -> " +
                    std::to_string(interval->second.second)});
        }
        writeReportSection(output, "Value Storage: " + pipeline.label);
        writeReportTable(output, {"Value", "Allocated Slots", "Full Ring Slots", "Write -> Last Read"},
                         {26, 18, 18, 24}, storageRows);
        ReportRows operationRows;
        for (const auto &operation : pipeline.operationStage)
        {
            const auto offset = pipeline.iterationOffset.find(operation.first);
            operationRows.push_back(
                {operation.first, std::to_string(operation.second),
                 offset == pipeline.iterationOffset.end()
                     ? "-"
                     : std::to_string(offset->second)});
        }
        writeReportSection(output, "Operations: " + pipeline.label);
        writeReportTable(output, {"Operation", "Stage", "Modulo Slot"},
                         {42, 10, 14}, operationRows);

        ReportRows recurrenceRows;
        for (const ScalarRecurrenceDependence &dependence :
             pipeline.scalarRecurrences)
        {
            recurrenceRows.push_back(
                {scheduledOperationName(context, dependence.phi),
                 scheduledOperationName(context, dependence.source),
                 scalarRecurrenceSinkNames(context, dependence),
                 std::to_string(dependence.distance), yesNo(dependence.exact),
                 dependence.reason});
        }
        writeReportSection(output, "Scalar Recurrences: " + pipeline.label);
        writeReportTable(output,
                         {"PHI", "Source", "Sinks", "Distance", "Exact",
                          "Reason"},
                         {24, 24, 30, 10, 8, 38}, recurrenceRows);

        ReportRows accessRows;
        for (const AffineMemoryAccessPattern &pattern :
             pipeline.memoryAccessPatterns)
        {
            const std::string expression =
                pattern.affine
                    ? std::to_string(pattern.coefficient) + "*i + " +
                          std::to_string(pattern.constant) +
                          (pattern.invariantTerms.empty() ? "" : " + invariant")
                    : "unknown";
            const std::string range =
                pattern.affine
                    ? (pattern.invariantTerms.empty() ? "[" : "relative [") +
                          std::to_string(pattern.minimum) + "," +
                          std::to_string(pattern.maximum) + "]"
                    : "unknown";
            accessRows.push_back(
                {scheduledOperationName(context, pattern.instruction),
                 pattern.memory, pattern.isWrite ? "write" : "read",
                 expression, range, unsignedList(pattern.possibleBanks),
                 yesNo(pattern.bankMappingExact), yesNo(pattern.affine),
                 pattern.reason});
        }
        writeReportSection(output, "Affine Memory Accesses: " + pipeline.label);
        writeReportTable(output,
                         {"Operation", "Memory", "Mode", "Address", "Range",
                          "Banks", "Bank Exact", "Affine", "Reason"},
                         {24, 18, 8, 18, 18, 16, 10, 8, 38}, accessRows);

        ReportRows dependenceRows;
        for (const MemoryDependenceInfo &dependence :
             pipeline.memoryDependences)
        {
            dependenceRows.push_back(
                {dependenceKindName(dependence.kind), dependence.memory,
                 scheduledOperationName(context, dependence.source),
                 scheduledOperationName(context, dependence.sink),
                 memoryEventKindName(dependence.sourceEvent) + " -> " +
                     memoryEventKindName(dependence.sinkEvent),
                 dependence.exists ? std::to_string(dependence.distance) : "-",
                 std::to_string(dependence.minimumSeparation),
                 yesNo(dependence.exists), yesNo(dependence.exact),
                 dependence.reason});
        }
        writeReportSection(output, "Memory Dependences: " + pipeline.label);
        writeReportTable(output,
                         {"Kind", "Memory", "Source", "Sink", "Events",
                          "Distance", "Separation", "Exists", "Exact",
                          "Reason"},
                         {14, 16, 22, 22, 28, 10, 12, 8, 8, 40},
                         dependenceRows);
    }
    writeDiagnostics(output, context);
    return output.str();
}

std::pair<unsigned, unsigned> directWritePortCounts(const HLSBackendContext &context)
{
    std::pair<unsigned, unsigned> counts{0, 0};
    for (const MemoryAccessBinding &binding : context.binding.memoryAccesses)
        for (const MemoryObject &memory : context.memoryObjects)
            if (memory.name == binding.memory)
            {
                unsigned &count = memory.storage == MemoryStorageKind::LocalRAM ? counts.first : counts.second;
                count += binding.directWriteDataBanks.size();
                break;
            }
    return counts;
}

std::string makeBindingReport(const HLSBackendContext &context)
{
    std::ostringstream output;
    writeReportTitle(output, "Resource Binding", context);
    unsigned memoryPortCount = 0;
    for (const MemoryObject &memory : context.memoryObjects)
        memoryPortCount += memory.ports * memory.banks;
    const auto directPorts = directWritePortCounts(context);
    writeReportSection(output, "Structural Summary");
    writeReportTable(
        output, {"Metric", "Value", "Description"}, {28, 14, 38},
        {{"Valid", yesNo(context.binding.valid && !context.hasErrors()),
          "Binder status"},
         {"Operator Instances", std::to_string(context.binding.operators.size()),
          "shared structural operators"},
         {"Registers", std::to_string(context.binding.registerCount),
          "registered SSA values"},
         {"Register Bits", std::to_string(context.binding.registerBits),
          "structural count"},
         {"State Register Bits",
          std::to_string(context.binding.stateRegisterBits), "FSM encoding"},
         {"Muxes", std::to_string(context.binding.muxCount),
          "structural count"},
         {"Mux Inputs", std::to_string(context.binding.muxInputCount),
          "structural count"},
         {"Ordinal Mux Bit Cost", std::to_string(context.binding.ordinalMuxBitCost),
          "selector input bits; not LUTs"},
         {"Selected Mux Bit Cost", std::to_string(context.binding.selectedMuxBitCost),
          "cost-monotone FU assignment"},
         {"Affinity Rebound Groups", std::to_string(context.binding.affinityReboundGroups),
          "strictly cheaper than ordinal binding"},
         {"Memory Objects", std::to_string(context.memoryObjects.size()),
          "external and local memories"},
         {"Memory Ports", std::to_string(memoryPortCount), "bound ports"},
         {"Direct Local Write Ports", std::to_string(directPorts.first),
          "single-producer ungated payloads"},
         {"Direct PIPO Write Ports", std::to_string(directPorts.second),
          "internal-only payloads sampled on accepted writes"},
         {"Memory Accesses",
          std::to_string(context.binding.memoryAccesses.size()),
          "load/store operations"}});

    ReportRows operatorRows;
    for (const OperatorInstance &instance : context.binding.operators)
    {
        operatorRows.push_back(
            {instance.name, instance.opcode, std::to_string(instance.bitWidth),
             std::to_string(instance.operations.size())});
    }
    writeReportSection(output, "Operator Instances");
    writeReportTable(output, {"Instance", "Opcode", "Width", "Operations"},
                     {35, 18, 10, 12}, operatorRows);

    ReportRows memoryRows;
    for (const MemoryObject &memory : context.memoryObjects)
    {
        memoryRows.push_back(
            {memory.name,
             memory.storage == MemoryStorageKind::ExternalAPMemory
                 ? "ap_memory"
                 : "internal_ram",
             std::to_string(memory.elementWidth),
             memory.depth == 0 ? "unknown" : std::to_string(memory.depth),
             std::to_string(memory.addressWidth),
             std::to_string(memory.ports),
             memoryBindingTypeName(memory.bindingType),
             memoryImplementationName(memory.implementation),
             memoryAccessModeName(memory.accessMode),
             memoryPortUsageSummary(memory),
             std::to_string(memory.readLatency),
             memory.fifoReadOnRequest ? "FIFO pop" : "response",
             memoryDepthSourceName(memory.depthSource)});
    }
    writeReportSection(output, "Memory Objects");
    writeReportTable(output,
                     {"Name", "Protocol", "Bits", "Depth", "Addr", "Ports",
                      "Type", "Impl", "Access", "Port Usage", "Read Lat",
                      "Read Sample", "Depth Source"},
                     {18, 12, 6, 9, 6, 6, 8, 8, 11, 22, 8, 12, 13}, memoryRows);

    ReportRows partitionRows;
    for (const MemoryObject &memory : context.memoryObjects)
    {
        partitionRows.push_back(
            {memory.name, dimensionList(memory.dimensions),
             partitionLayoutSummary(memory), std::to_string(memory.banks),
             std::to_string(memory.bankDepth),
             std::to_string(memory.bankAddressWidth),
             memoryBankInterface(memory), bankPortUsageSummary(memory)});
    }
    writeReportSection(output, "Array Partition Layout");
    writeReportTable(
        output,
        {"Name", "Dimensions", "Partition", "Banks", "Bank Depth",
         "Bank Addr", "RTL Interface", "Bank Port Usage"},
        {18, 18, 36, 8, 11, 9, 14, 34}, partitionRows);

    ReportRows accessRows;
    for (const MemoryAccessBinding &binding : context.binding.memoryAccesses)
    {
        accessRows.push_back(
            {binding.operation, binding.memory,
             unsignedList(binding.possibleBanks),
             binding.bankDynamic ? "dynamic" : "static",
             std::to_string(binding.port)});
    }
    writeReportSection(output, "Memory Access Bindings");
    writeReportTable(output,
                     {"Operation", "Memory", "Possible Banks", "Mapping",
                      "Port"},
                     {38, 25, 22, 10, 8}, accessRows);
    ReportRows bankAnalysisRows;
    for (const MemoryAccessBinding &binding : context.binding.memoryAccesses)
    {
        const auto access = context.memoryAccesses.find(binding.instruction);
        if (access == context.memoryAccesses.end())
            continue;
        bankAnalysisRows.push_back({binding.operation, access->second.bankAnalysisSource,
            yesNo(access->second.bankMappingExact),
            std::to_string(access->second.bankAnalysisIterations), access->second.bankAnalysisReason});
    }
    writeReportSection(output, "Memory Bank Analysis");
    writeReportTable(output, {"Operation", "Analysis", "Proven", "Iterations", "Reason"},
                     {38, 20, 8, 12, 65}, bankAnalysisRows);
    ReportRows directWriteRows;
    for (const MemoryAccessBinding &binding : context.binding.memoryAccesses)
        for (unsigned bank : binding.directWriteDataBanks)
            directWriteRows.push_back({binding.operation, binding.memory,
                                      std::to_string(bank), std::to_string(binding.port)});
    writeReportSection(output, "Direct Local Write Data");
    writeReportTable(output, {"Producer", "Memory", "Bank", "Port"},
                     {38, 25, 8, 8}, directWriteRows);
    output << "\nNote: structural binding counts are not synthesized LUT/FF "
              "measurements.\n";
    writeDiagnostics(output, context);
    return output.str();
}

std::string makeRTLReport(const HLSBackendContext &context)
{
    std::ostringstream output;
    writeReportTitle(output, "RTL Generation", context);
    writeReportSection(output, "RTL Summary");
    writeReportTable(
        output, {"Field", "Value", "Description"}, {24, 25, 32},
        {{"Status", context.rtl.valid ? "generated" : "failed",
          "RTL generation result"},
         {"Module", context.rtl.moduleName, "Verilog top module"},
         {"Control Protocol", "ap_ctrl_hs", "start/done/idle/ready"},
         {"Memory Objects", std::to_string(context.rtl.memories.size()),
          "external and local memories"},
         {"DATAFLOW Tasks", std::to_string(context.rtl.dataflow.tasks.size()),
          "concurrent task modules"},
         {"DATAFLOW Channels",
          std::to_string(context.rtl.dataflow.channels.size()),
          "bounded FIFO or PIPO channels"},
         {"Verilog", context.rtl.valid ? "rtl/top.v" : "rtl/failure.json",
          "generated artifact"}});
    writeDiagnostics(output, context);
    return output.str();
}

std::string makeDataflowReport(const HLSBackendContext &context)
{
    const DataflowGraph &graph = context.rtl.dataflow;
    std::ostringstream output;
    writeReportTitle(output, "DATAFLOW Task Graph", context);
    writeReportSection(output, "Graph Summary");
    writeReportTable(
        output, {"Metric", "Value", "Description"}, {24, 16, 38},
        {{"Valid", yesNo(graph.valid), "task graph status"},
         {"Performance Valid", yesNo(graph.performanceValid),
          "rate, capacity, II, and deadlock checks"},
         {"Rates Balanced", yesNo(graph.ratesBalanced),
          "equal transaction production/consumption"},
         {"Deadlock Free", yesNo(graph.deadlockFree),
          "static zero-token graph check"},
         {"Cyclic", yesNo(graph.cyclic), "directed channel cycle"},
         {"Zero-token Cycle", yesNo(graph.zeroTokenCycle),
          "startup-deadlocking directed cycle"},
         {"Graph II", std::to_string(graph.graphInitiationInterval),
          graph.graphIISource},
         {"Execution Model", graph.executionModel,
          "generated DATAFLOW top controller"},
         {"Outstanding Capacity",
          std::to_string(graph.maximumOutstandingTransactions),
          "accepted top transactions"},
         {"Minimum Acceptance II",
          std::to_string(graph.minimumAcceptanceInterval),
          "burst-mode ap_start acceptance"},
         {"Measured Acceptance II",
          graph.measuredAcceptanceInterval < 0
              ? "not measured"
              : std::to_string(graph.measuredAcceptanceInterval),
          "RTL testbench measurement"},
         {"First-Transaction Latency",
          graph.firstTransactionLatencyLowerBound < 0
              ? "not available"
              : (graph.firstTransactionLatencyExact
                     ? std::to_string(
                           graph.firstTransactionLatencyLowerBound)
                     : std::to_string(
                           graph.firstTransactionLatencyLowerBound) +
                           ".." +
                           (graph.firstTransactionLatencyUpperBound < 0
                                ? "unknown"
                                : std::to_string(
                                      graph.firstTransactionLatencyUpperBound))),
          graph.firstTransactionLatencySource},
         {"PIPO Handoff", std::to_string(graph.pipoHandoffCycles) + " cycle",
          "commit/release look-ahead controller"},
         {"Retirement", std::to_string(graph.retirementCycles) + " cycle",
          "terminal done to top-level ap_done"},
         {"Tasks", std::to_string(graph.tasks.size()),
          "concurrent RTL modules"},
         {"Channels", std::to_string(graph.channels.size()),
          "bounded internal channels"}});
    ReportRows taskRows;
    for (const DataflowTask &task : graph.tasks)
        taskRows.push_back({task.name, task.function,
                            std::to_string(task.latencyCycles),
                            yesNo(task.latencyExact), task.latencySource,
                            std::to_string(task.initiationInterval),
                            std::to_string(task.pipelineCount),
                            task.pipelineInitiationInterval == 0
                                ? "-"
                                : std::to_string(
                                      task.pipelineInitiationInterval)});
    writeReportSection(output, "Tasks");
    writeReportTable(output,
                     {"Task", "LLVM Function", "Latency", "Exact", "Source",
                      "Txn II", "Pipelines", "Loop II"},
                     {24, 38, 9, 7, 44, 9, 10, 8}, taskRows);
    ReportRows channelRows;
    auto accessTiming = [](bool exact, long long firstCycle,
                           unsigned interval) {
        if (!exact || firstCycle < 0)
            return std::string("unknown");
        return std::to_string(firstCycle) + "/" +
               (interval == 0 ? std::string("single")
                              : std::to_string(interval));
    };
    for (const DataflowChannel &channel : graph.channels)
    {
        channelRows.push_back(
            {channel.name, channel.producer, channel.consumer, channel.type,
             std::to_string(channel.width), std::to_string(channel.depth),
             std::to_string(channel.instances),
             partitionKindName(channel.partition),
             std::to_string(channel.banks),
             std::to_string(channel.bankDepth),
             std::to_string(channel.initialTokens),
             channel.rateKnown
                 ? "exact"
                 : (channel.rateBoundsKnown ? "dynamic" : "unknown"),
             channel.rateBoundsKnown
                 ? std::to_string(channel.minimumProductionRate) + ".." +
                       std::to_string(channel.maximumProductionRate)
                 : "unknown",
             channel.rateBoundsKnown
                 ? std::to_string(channel.minimumConsumptionRate) + ".." +
                       std::to_string(channel.maximumConsumptionRate)
                 : "unknown",
             accessTiming(channel.producerAccessTimingExact,
                          channel.producerFirstAccessCycle,
                          channel.producerAccessInterval),
             accessTiming(channel.consumerAccessTimingExact,
                          channel.consumerFirstAccessCycle,
                          channel.consumerAccessInterval),
             std::to_string(channel.minimumDepth),
             std::to_string(channel.bufferCount),
             yesNo(channel.broadcast), yesNo(channel.capacitySafe),
             channel.consumerReadOnRequest ? "pop edge" : "registered"});
    }
    writeReportSection(output, "Channels");
    writeReportTable(output,
                     {"Channel", "Producer", "Consumer", "Type", "Bits",
                      "Depth", "Instances", "Partition", "Banks", "Bank Depth",
                      "Initial", "Rate", "Produce", "Consume", "Prod First/II",
                      "Cons First/II",
                      "Min Depth", "Buffers", "Broadcast", "Safe", "Consumer Read"},
                     {16, 18, 18, 8, 6, 7, 9, 10, 7, 10, 8, 8, 12, 12, 13,
                      13, 9, 7, 10, 6, 12},
                     channelRows);
    writeDiagnostics(output, context);
    return output.str();
}

std::string makeDataflowJSON(const HLSBackendContext &context)
{
    const DataflowGraph &graph = context.rtl.dataflow;
    std::ostringstream output;
    output << '{';
    writeCommonFields(output, context, "dataflow_task_graph",
                      context.rtl.dataflow.valid && !context.hasErrors());
    output << ",\"performance_valid\":"
           << (graph.performanceValid ? "true" : "false")
           << ",\"rates_balanced\":"
           << (graph.ratesBalanced ? "true" : "false")
           << ",\"deadlock_free\":"
           << (graph.deadlockFree ? "true" : "false")
           << ",\"cyclic\":" << (graph.cyclic ? "true" : "false")
           << ",\"zero_token_cycle\":"
           << (graph.zeroTokenCycle ? "true" : "false")
           << ",\"graph_ii\":" << graph.graphInitiationInterval
           << ",\"graph_ii_source\":" << jsonString(graph.graphIISource)
           << ",\"execution_model\":" << jsonString(graph.executionModel)
           << ",\"maximum_outstanding_transactions\":"
           << graph.maximumOutstandingTransactions
           << ",\"minimum_acceptance_interval\":"
           << graph.minimumAcceptanceInterval
           << ",\"measured_acceptance_interval\":";
    if (graph.measuredAcceptanceInterval < 0)
        output << "null";
    else
        output << graph.measuredAcceptanceInterval;
    output << ",\"first_transaction_latency_lower_bound\":";
    if (graph.firstTransactionLatencyLowerBound < 0)
        output << "null";
    else
        output << graph.firstTransactionLatencyLowerBound;
    output << ",\"first_transaction_latency_upper_bound\":";
    if (graph.firstTransactionLatencyUpperBound < 0)
        output << "null";
    else
        output << graph.firstTransactionLatencyUpperBound;
    output << ",\"first_transaction_latency_exact\":"
           << (graph.firstTransactionLatencyExact ? "true" : "false")
           << ",\"first_transaction_latency_source\":"
           << jsonString(graph.firstTransactionLatencySource)
           << ",\"pipo_handoff_cycles\":" << graph.pipoHandoffCycles
           << ",\"retirement_cycles\":" << graph.retirementCycles
           << ",\"completion_tasks\":";
    const auto completions = dataflowCompletionTasks(graph);
    writeStringArray(output, std::vector<std::string>(completions.begin(), completions.end()));
    output
           << ",\"topological_order\":[";
    for (std::size_t index = 0; index < graph.topologicalOrder.size(); ++index)
    {
        if (index)
            output << ',';
        output << jsonString(graph.topologicalOrder[index]);
    }
    output << "],\"tasks\":[";
    for (std::size_t index = 0; index < graph.tasks.size(); ++index)
    {
        if (index)
            output << ',';
        const DataflowTask &task = graph.tasks[index];
        output << "{\"name\":" << jsonString(task.name)
               << ",\"function\":" << jsonString(task.function)
               << ",\"latency_cycles\":" << task.latencyCycles
               << ",\"latency_exact\":"
               << (task.latencyExact ? "true" : "false")
               << ",\"latency_source\":"
               << jsonString(task.latencySource)
               << ",\"initiation_interval\":" << task.initiationInterval
               << ",\"pipeline_count\":" << task.pipelineCount
               << ",\"pipeline_initiation_interval\":";
        if (task.pipelineInitiationInterval == 0)
            output << "null";
        else
            output << task.pipelineInitiationInterval;
        output
               << '}';
    }
    output << "],\"channels\":[";
    for (std::size_t index = 0; index < graph.channels.size(); ++index)
    {
        if (index)
            output << ',';
        const DataflowChannel &channel = graph.channels[index];
        output << "{\"name\":" << jsonString(channel.name)
               << ",\"producer\":" << jsonString(channel.producer)
               << ",\"consumer\":" << jsonString(channel.consumer)
               << ",\"type\":" << jsonString(channel.type)
               << ",\"width\":" << channel.width
               << ",\"depth\":" << channel.depth
               << ",\"instances\":" << channel.instances
               << ",\"initial_tokens\":" << channel.initialTokens
               << ",\"partition\":"
               << jsonString(partitionKindName(channel.partition))
               << ",\"banks\":" << channel.banks
               << ",\"bank_depth\":" << channel.bankDepth
               << ",\"rate_known\":"
               << (channel.rateKnown ? "true" : "false")
               << ",\"rate_bounds_known\":"
               << (channel.rateBoundsKnown ? "true" : "false")
               << ",\"runtime_rate_check_required\":"
               << (channel.runtimeRateCheckRequired ? "true" : "false")
               << ",\"production_rate\":" << channel.productionRate
               << ",\"consumption_rate\":" << channel.consumptionRate
               << ",\"consumer_read_on_request\":" << (channel.consumerReadOnRequest ? "true" : "false")
               << ",\"minimum_production_rate\":"
               << channel.minimumProductionRate
               << ",\"maximum_production_rate\":"
               << channel.maximumProductionRate
               << ",\"minimum_consumption_rate\":"
               << channel.minimumConsumptionRate
               << ",\"maximum_consumption_rate\":"
               << channel.maximumConsumptionRate
               << ",\"producer_access_timing_exact\":"
               << (channel.producerAccessTimingExact ? "true" : "false")
               << ",\"producer_first_access_cycle\":";
        if (channel.producerFirstAccessCycle < 0)
            output << "null";
        else
            output << channel.producerFirstAccessCycle;
        output << ",\"producer_access_interval\":"
               << channel.producerAccessInterval
               << ",\"consumer_access_timing_exact\":"
               << (channel.consumerAccessTimingExact ? "true" : "false")
               << ",\"consumer_first_access_cycle\":";
        if (channel.consumerFirstAccessCycle < 0)
            output << "null";
        else
            output << channel.consumerFirstAccessCycle;
        output << ",\"consumer_access_interval\":"
               << channel.consumerAccessInterval
               << ",\"minimum_depth\":" << channel.minimumDepth
               << ",\"buffer_count\":" << channel.bufferCount
               << ",\"broadcast\":"
               << (channel.broadcast ? "true" : "false")
               << ",\"capacity_safe\":"
               << (channel.capacitySafe ? "true" : "false") << '}';
    }
    output << "]}\n";
    return output.str();
}

std::string synthesisStatus(const HLSBackendContext &context)
{
    if (context.synthesis.valid)
        return "succeeded";
    if (!context.configuration.runSynthesis)
        return "not requested";
    return context.synthesis.attempted ? "failed" : "pending";
}

std::string makeSynthesisReport(const HLSBackendContext &context)
{
    const RTLSynthesisResult &synthesis = context.synthesis;
    std::ostringstream output;
    writeReportTitle(output, "Vivado Post-RTL Synthesis", context);
    writeReportSection(output, "Synthesis Summary");
    writeReportTable(
        output, {"Field", "Value", "Description"}, {24, 25, 32},
        {{"Requested", yesNo(context.configuration.runSynthesis),
          "--synthesis enable"},
         {"Status", synthesisStatus(context), "Vivado execution status"},
         {"Tool", context.configuration.synthesisTool, "synthesis tool"},
         {"Tool Version", synthesis.toolVersion, "reported version"},
         {"FPGA Part", context.configuration.synthesisPart, "target device"},
         {"Synthesis Mode", "out_of_context", "kernel-level comparison"},
         {"Constraint Phase", "before synthesis", "clock and IO XDC"},
         {"IO Budget", "20% of clock period", "input/output max; min = 0"},
         {"Vivado Threads", "1", "stable U250 device loading"},
         {"CPU Affinity",
          synthesis.cpuAffinity >= 0
              ? "CPU " + std::to_string(synthesis.cpuAffinity)
              : "not pinned",
          "Vivado process placement"},
         {"Runtime", doubleText(synthesis.runtimeSeconds) + " s",
          "tool wall time"}});
    writeReportSection(output, "Post-Synthesis Resources");
    writeReportTable(
        output, {"Resource", "Used", "QoR Kind"}, {24, 18, 38},
        {{"LUT", synthesis.valid ? std::to_string(synthesis.lut) : "-",
          "measured post-RTL synthesis"},
         {"FF", synthesis.valid ? std::to_string(synthesis.ff) : "-",
          "measured post-RTL synthesis"},
         {"DSP", synthesis.valid ? std::to_string(synthesis.dsp) : "-",
          "measured post-RTL synthesis"},
         {"BRAM Tiles",
          synthesis.valid ? doubleText(synthesis.bramTiles, 1) : "-",
          "measured post-RTL synthesis"}});
    writeReportSection(output, "Timing");
    writeReportTable(
        output, {"Metric", "Value", "Requirement"}, {28, 18, 34},
        {{"Target Period", doubleText(context.configuration.clockPeriodNs) +
                               " ns",
          "selected Light-HLS clock"},
         {"Worst Slack",
          synthesis.hasTiming ? doubleText(synthesis.worstSlackNs) + " ns" : "-",
          "non-negative meets timing"},
         {"Period Minus WNS",
          synthesis.hasTiming
              ? doubleText(context.configuration.clockPeriodNs -
                           synthesis.worstSlackNs) +
                    " ns"
              : "-",
          "includes IO budget; not Fmax"}});
    writeDiagnostics(output, context);
    return output.str();
}

std::string makeComparisonReport(const HLSBackendContext &context)
{
    std::ostringstream output;
    writeReportTitle(output, "QoR Summary", context);
    writeReportSection(output, "Stage Comparison");
    ReportRows rows = {
        {"Pre-Backend Estimate", integerText(context.estimate.latencyCycles),
         integerText(context.estimate.lut), integerText(context.estimate.ff),
         integerText(context.estimate.dsp), integerText(context.estimate.bram),
         "heuristic"},
        {"Post-Scheduling", integerText(context.schedule.latencyCycles), "-", "-",
         "-", "-", "scheduled states"},
        {"Post-Binding", "-", "-", "-", "-", "-", "structural"},
        {"Post-RTL Synthesis", "-",
         context.synthesis.valid ? std::to_string(context.synthesis.lut) : "-",
         context.synthesis.valid ? std::to_string(context.synthesis.ff) : "-",
         context.synthesis.valid ? std::to_string(context.synthesis.dsp) : "-",
         context.synthesis.valid ? doubleText(context.synthesis.bramTiles, 1)
                                 : "-",
         synthesisStatus(context)}};
    writeReportTable(output,
                     {"Stage", "Latency", "LUT", "FF", "DSP", "BRAM",
                      "Meaning"},
                     {22, 9, 9, 9, 7, 8, 19}, rows);
    writeReportSection(output, "Binding Structure");
    writeReportTable(
        output, {"Metric", "Value"}, {32, 22},
        {{"FSM States", std::to_string(context.schedule.stateCount)},
         {"Registers", std::to_string(context.binding.registerCount)},
         {"Register Bits", std::to_string(context.binding.registerBits)},
         {"Operator Instances", std::to_string(context.binding.operators.size())},
         {"Memory Objects", std::to_string(context.memoryObjects.size())},
         {"Memory Accesses",
          std::to_string(context.binding.memoryAccesses.size())}});
    output << "\nPre-backend values are heuristic, post-binding values are "
              "structural, and post-RTL values are measured by Vivado.\n";
    writeDiagnostics(output, context);
    return output.str();
}

std::string makeReportIndex(const HLSBackendContext &context)
{
    std::ostringstream output;
    writeReportTitle(output, "Report Index", context);
    writeReportSection(output, "Human-Readable Reports");
    writeReportTable(
        output, {"Stage", "Report"}, {28, 48},
        {{"Pre-Backend Estimate", "estimate/pre_backend_estimate.rpt"},
         {"Operation Scheduling", "scheduling/schedule.rpt"},
         {"Loop Pipeline Scheduling", "scheduling/pipeline_schedule.rpt"},
         {"DATAFLOW Task Graph", "scheduling/dataflow_graph.rpt"},
         {"Resource Binding", "binding/binding.rpt"},
         {"RTL Generation", "rtl/rtl_generation.rpt"},
         {"Vivado Post-RTL Synthesis", "vivado_report/vivado_report.rpt"},
         {"QoR Summary", "summary/qor_summary.rpt"}});
    writeReportSection(output, "Machine-Readable Artifacts");
    writeReportTable(
        output, {"Artifact", "Path"}, {28, 48},
        {{"Manifest", "manifest.json"},
         {"Estimate JSON", "estimate/pre_backend_estimate.json"},
         {"Schedule JSON", "scheduling/schedule.json"},
         {"Pipeline JSON", "scheduling/pipeline_schedule.json"},
         {"DATAFLOW JSON", "scheduling/dataflow_graph.json"},
         {"Binding JSON", "binding/binding.json"},
         {"Generated RTL", context.rtl.valid ? "rtl/top.v" : "rtl/failure.json"},
         {"Vivado JSON", "vivado_report/vivado_report.json"},
         {"QoR JSON", "summary/qor_comparison.json"},
         {"Intermediate Files", ".lighthls/"}});
    writeDiagnostics(output, context);
    return output.str();
}

std::string makeEstimateJSON(const HLSBackendContext &context)
{
    std::ostringstream output;
    output << '{';
    writeCommonFields(output, context, "pre_backend_estimate",
                      context.estimate.valid);
    output << ",\"qor_kind\":\"heuristic_estimate\""
           << ",\"latency_cycles\":" << context.estimate.latencyCycles
           << ",\"initiation_interval\":"
           << context.estimate.initiationInterval << ",\"resources\":{"
           << "\"dsp\":" << context.estimate.dsp << ",\"ff\":"
           << context.estimate.ff << ",\"lut\":" << context.estimate.lut
           << ",\"bram\":" << context.estimate.bram << "}}\n";
    return output.str();
}

static std::string pipelineStorageJSONFields(const HLSBackendContext &context,
                                             const LoopPipelineInfo &pipeline)
{
    std::ostringstream output;
    output << ",\"forwarded_load_count\":" << pipeline.forwardedLoads.size()
           << ",\"stage_indexed_controller\":" << (pipeline.stageIndexedController ? "true" : "false")
           << ",\"last_token_controller\":" << (pipeline.lastTokenController ? "true" : "false")
           << ",\"iteration_tag_last_stage\":" << pipeline.iterationTagLastStage
           << ",\"return_forwarded\":" << (pipeline.forwardedReturnValue ? "true" : "false")
           << ",\"initialized_at_entry\":" << (pipeline.initializedAtEntry ? "true" : "false")
           << ",\"storage_alias_count\":" << pipeline.storageAliases.size()
           << ",\"value_storage\":[";
    bool first = true;
    unsigned long long allocatedBits = 0, uncompactedBits = 0;
    for (const llvm::Instruction &instruction : *pipeline.header)
    {
        if (!pipeline.contextValues.count(&instruction))
            continue;
        if (!first)
            output << ',';
        first = false;
        const unsigned slots = pipelineValueStorageSlots(pipeline, &instruction);
        const unsigned width = instruction.getType()->getIntegerBitWidth();
        const auto alias = pipeline.storageAliases.find(&instruction);
        if (alias == pipeline.storageAliases.end())
            allocatedBits += static_cast<unsigned long long>(slots) * width;
        uncompactedBits += static_cast<unsigned long long>(pipeline.contextSlots) * width;
        output << "{\"value\":" << jsonString(scheduledOperationName(context, &instruction))
               << ",\"slots\":" << slots << ",\"width\":" << width
               << ",\"retirement_only\":"
               << (pipeline.retirementOnlyValues.count(&instruction) ? "true" : "false");
        if (alias != pipeline.storageAliases.end())
            output << ",\"alias_of\":" << jsonString(scheduledOperationName(context,
                llvm::cast<llvm::Instruction>(alias->second)));
        const auto interval = pipeline.valueLiveIntervals.find(&instruction);
        if (interval != pipeline.valueLiveIntervals.end())
            output << ",\"write_cycle\":" << interval->second.first
                   << ",\"last_read_cycle\":" << interval->second.second;
        output << '}';
    }
    output << "],\"value_storage_bits\":" << allocatedBits
           << ",\"uncompacted_value_storage_bits\":" << uncompactedBits;
    return output.str();
}

std::string makeScheduleJSON(const HLSBackendContext &context, bool detailed)
{
    std::ostringstream output;
    output << '{';
    writeCommonFields(output, context,
                      detailed ? "operation_schedule" : "post_scheduling",
                      context.schedule.valid && !context.hasErrors());
    output << ",\"qor_kind\":\"scheduled_control_steps\""
           << ",\"latency_cycles\":";
    if (context.schedule.latencyCycles < 0)
        output << "null";
    else
        output << context.schedule.latencyCycles;
    output << ",\"latency_reason\":"
           << jsonString(context.schedule.hasControlFlowCycle
                             ? "dynamic control-flow cycle"
                             : "static acyclic schedule")
           << ",\"achieved_ii\":" << context.schedule.achievedII
           << ",\"state_count\":" << context.schedule.stateCount
           << ",\"memory_port_conflicts\":"
           << context.schedule.memoryPortConflicts
           << ",\"memory_port_serialization_cycles\":"
           << context.schedule.memoryPortSerializationCycles
           << ",\"memory_ordering_cycles\":" << context.schedule.memoryOrderingCycles
           << ",\"combinational_value_count\":" << context.combinationalValues.size()
           << ",\"ordinary_feedback_wire_count\":" << context.ordinaryFeedbackValues.size()
           << ",\"forwarded_block_load_count\":" << context.forwardedLoads.size()
           << ",\"hardware_constant_count\":" << context.constantValues.size()
           << ",\"combinational_delay_budget_ns\":" << context.configuration.clockPeriodNs * 0.7;
    output << ",\"ordinary_feedback_wires\":[";
    bool firstFeedback = true;
    if (context.top)
        for (const llvm::BasicBlock &block : *context.top)
            for (const llvm::Instruction &instruction : block)
            {
                const auto feedback = context.ordinaryFeedbackValues.find(&instruction);
                if (feedback == context.ordinaryFeedbackValues.end())
                    continue;
                if (!firstFeedback)
                    output << ',';
                firstFeedback = false;
                output << "{\"operation\":" << jsonString(context.valueNames.at(&instruction))
                       << ",\"phi\":" << jsonString(context.valueNames.at(feedback->second))
                       << ",\"bit_width\":" << instruction.getType()->getIntegerBitWidth()
                       << ",\"sample_edge\":" << jsonString(feedback->second->getParent() == &block ?
                            "ordinary_self_backedge" : "ordinary_loop_latch") << '}';
            }
    output << ']';
    output << ",\"ordinary_backedge_bypass_count\":" << context.ordinaryBackedgeBypasses.size()
           << ",\"ordinary_register_alias_count\":" << context.ordinaryRegisterAliases.size()
           << ",\"ordinary_held_liveout_phi_count\":" << context.ordinaryLiveOutExpressionRoots.size()
           << ",\"ordinary_fused_return_count\":" << context.ordinaryReturnExits.size()
           << ",\"ordinary_fused_void_return_count\":" << std::count_if(
                context.ordinaryReturnExits.begin(), context.ordinaryReturnExits.end(),
                [](const llvm::BasicBlock *block) {
                    return !llvm::cast<llvm::ReturnInst>(block->getTerminator())->getReturnValue();
                })
           << ",\"ordinary_entry_fused\":" << (context.ordinaryEntrySuccessor ? "true" : "false")
           << ",\"ordinary_backedge_bypasses\":[";
    bool firstBypass = true;
    if (context.top)
        for (const llvm::BasicBlock &block : *context.top)
            for (const llvm::PHINode &phi : block.phis())
            {
                const auto bypass = context.ordinaryBackedgeBypasses.find(&phi);
                if (bypass == context.ordinaryBackedgeBypasses.end())
                    continue;
                if (!firstBypass)
                    output << ',';
                firstBypass = false;
                const auto scheduled = context.schedule.instructionMap.find(bypass->second);
                output << "{\"phi\":" << jsonString(context.valueNames.at(&phi))
                       << ",\"operation\":" << jsonString(context.valueNames.at(bypass->second))
                       << ",\"sample_cycle\":" << (scheduled == context.schedule.instructionMap.end() ?
                            "null" : std::to_string(scheduled->second->startCycle))
                       << ",\"result_register_retained\":" << (context.ordinaryRegisterAliases.count(bypass->second) ? "false" : "true")
                       << ",\"result_value_retained\":true,\"storage_register\":"
                       << jsonString(context.valueNames.at(context.ordinaryRegisterAliases.count(bypass->second) ?
                            static_cast<const llvm::Value *>(&phi) : static_cast<const llvm::Value *>(bypass->second))) << '}';
            }
    output << ']';
    output << ",\"ordinary_read_phi_bypass_count\":" << context.ordinaryReadPhiBypasses.size()
           << ",\"ordinary_read_phi_bypasses\":[";
    for (std::size_t index = 0; index < context.ordinaryReadPhiBypasses.size(); ++index)
    {
        if (index)
            output << ',';
        const OrdinaryReadPhiBypass &bypass = context.ordinaryReadPhiBypasses[index];
        const auto scheduled = context.schedule.instructionMap.find(bypass.load);
        output << "{\"predecessor\":" << jsonString(bypass.predecessor->getName().str())
               << ",\"successor\":" << jsonString(bypass.phi->getParent()->getName().str())
               << ",\"phi\":" << jsonString(context.valueNames.at(bypass.phi))
               << ",\"load\":" << jsonString(context.valueNames.at(bypass.load))
               << ",\"sample_cycle\":" << (scheduled == context.schedule.instructionMap.end() ? "null" :
                    std::to_string(scheduled->second->startCycle + scheduled->second->latency - 1))
               << ",\"load_register_retained\":true}";
    }
    output << ']';
    output << ",\"pipelines\":[";
    for (std::size_t index = 0; index < context.schedule.pipelines.size();
         ++index)
    {
        if (index)
            output << ',';
        const LoopPipelineInfo &pipeline = context.schedule.pipelines[index];
        output << "{\"loop\":" << jsonString(pipeline.label)
               << ",\"ir_loop\":" << jsonString(pipeline.irLoopName)
               << ",\"requested_ii\":" << pipeline.requestedII
               << ",\"achieved_ii\":" << pipeline.achievedII
               << ",\"recurrence_mii\":" << pipeline.recurrenceMII
               << ",\"memory_mii\":" << pipeline.memoryMII
               << ",\"memory_dependence_mii\":"
               << pipeline.memoryDependenceMII
               << ",\"resource_mii\":" << pipeline.resourceMII
               << ",\"trip_count\":" << pipeline.tripCount
               << ",\"source_trip_count\":" << pipeline.sourceTripCount
               << ",\"flattened_loop_count\":" << pipeline.flattenedLoopCount
               << ",\"flattening_reason\":" << jsonString(pipeline.flatteningReason)
               << ",\"pipeline_latency\":" << pipeline.pipelineLatency
               << ",\"stage_count\":" << pipeline.stageCount
               << ",\"context_slots\":" << pipeline.contextSlots
               << ",\"context_value_count\":"
               << pipeline.contextValues.size()
               << ",\"derived_value_count\":"
               << pipeline.derivedValues.size()
               << ",\"live_out_value_count\":"
               << pipeline.liveOutValues.size()
               << ",\"conservative_memory_dependence\":"
               << (pipeline.conservativeMemoryDependence ? "true" : "false")
               << ",\"dependence_analysis_valid\":"
               << (pipeline.dependenceAnalysisValid ? "true" : "false")
               << ",\"limiting_factor\":"
               << jsonString(pipeline.limitingFactor)
               << pipelineStorageJSONFields(context, pipeline) << '}';
    }
    output << ']';
    if (detailed)
    {
        output << ",\"blocks\":[";
        for (std::size_t blockIndex = 0;
             blockIndex < context.schedule.blocks.size(); ++blockIndex)
        {
            if (blockIndex)
                output << ',';
            const BlockSchedule &block = context.schedule.blocks[blockIndex];
            output << "{\"name\":" << jsonString(block.name)
                   << ",\"first_state\":" << block.firstState
                   << ",\"state_count\":" << block.stateCount
                   << ",\"instructions\":[";
            for (std::size_t instructionIndex = 0;
                 instructionIndex < block.instructions.size(); ++instructionIndex)
            {
                if (instructionIndex)
                    output << ',';
                const InstructionSchedule &instruction =
                    block.instructions[instructionIndex];
                output << "{\"name\":" << jsonString(instruction.name)
                       << ",\"opcode\":" << jsonString(instruction.opcode)
                       << ",\"start_cycle\":" << instruction.startCycle
                       << ",\"latency\":" << instruction.latency
                       << ",\"result_width\":" << instruction.resultWidth << '}';
            }
            output << "]}";
        }
        output << ']';
    }
    output << "}\n";
    return output.str();
}

std::string makePipelineScheduleJSON(const HLSBackendContext &context)
{
    std::ostringstream output;
    output << '{';
    writeCommonFields(output, context, "loop_pipeline_schedule",
                      context.schedule.valid && !context.hasErrors());
    output << ",\"pipelines\":[";
    for (std::size_t index = 0; index < context.schedule.pipelines.size();
         ++index)
    {
        if (index)
            output << ',';
        const LoopPipelineInfo &pipeline = context.schedule.pipelines[index];
        output << "{\"loop\":" << jsonString(pipeline.label)
               << ",\"ir_loop\":" << jsonString(pipeline.irLoopName)
               << ",\"requested_ii\":" << pipeline.requestedII
               << ",\"achieved_ii\":" << pipeline.achievedII
               << ",\"memory_mii\":" << pipeline.memoryMII
               << ",\"memory_dependence_mii\":"
               << pipeline.memoryDependenceMII
               << ",\"recurrence_mii\":" << pipeline.recurrenceMII
               << ",\"resource_mii\":" << pipeline.resourceMII
               << ",\"trip_count\":" << pipeline.tripCount
               << ",\"source_trip_count\":" << pipeline.sourceTripCount
               << ",\"flattened_loop_count\":" << pipeline.flattenedLoopCount
               << ",\"flattening_reason\":" << jsonString(pipeline.flatteningReason)
               << ",\"pipeline_latency\":" << pipeline.pipelineLatency
               << ",\"stage_count\":" << pipeline.stageCount
               << ",\"context_slots\":" << pipeline.contextSlots
               << ",\"context_value_count\":"
               << pipeline.contextValues.size()
               << ",\"derived_value_count\":"
               << pipeline.derivedValues.size()
               << ",\"live_out_value_count\":"
               << pipeline.liveOutValues.size()
               << ",\"dependence_analysis_valid\":"
               << (pipeline.dependenceAnalysisValid ? "true" : "false")
               << ",\"conservative_memory_dependence\":"
               << (pipeline.conservativeMemoryDependence ? "true" : "false")
               << ",\"limiting_factor\":"
               << jsonString(pipeline.limitingFactor)
               << pipelineStorageJSONFields(context, pipeline)
               << ",\"operations\":[";
        bool firstOperation = true;
        for (const auto &operation : pipeline.operationStage)
        {
            if (!firstOperation)
                output << ',';
            firstOperation = false;
            const auto offset = pipeline.iterationOffset.find(operation.first);
            output << "{\"name\":" << jsonString(operation.first)
                   << ",\"stage\":" << operation.second
                   << ",\"modulo_slot\":"
                   << (offset == pipeline.iterationOffset.end()
                           ? -1
                           : offset->second)
                   << '}';
        }
        output << "],\"scalar_recurrences\":[";
        for (std::size_t recurrenceIndex = 0;
             recurrenceIndex < pipeline.scalarRecurrences.size();
             ++recurrenceIndex)
        {
            if (recurrenceIndex)
                output << ',';
            const ScalarRecurrenceDependence &dependence =
                pipeline.scalarRecurrences[recurrenceIndex];
            output << "{\"kind\":\"scalar_recurrence\""
                   << ",\"phi\":"
                   << jsonString(scheduledOperationName(context,
                                                        dependence.phi))
                   << ",\"source\":"
                   << jsonString(scheduledOperationName(context,
                                                        dependence.source))
                   << ",\"sinks\":[";
            for (std::size_t sinkIndex = 0;
                 sinkIndex < dependence.sinks.size(); ++sinkIndex)
            {
                if (sinkIndex)
                    output << ',';
                output << jsonString(scheduledOperationName(
                    context, dependence.sinks[sinkIndex]));
            }
            output << "]"
                   << ",\"distance\":" << dependence.distance
                   << ",\"exact\":"
                   << (dependence.exact ? "true" : "false")
                   << ",\"reason\":" << jsonString(dependence.reason)
                   << '}';
        }
        output << "],\"memory_access_patterns\":[";
        for (std::size_t accessIndex = 0;
             accessIndex < pipeline.memoryAccessPatterns.size(); ++accessIndex)
        {
            if (accessIndex)
                output << ',';
            const AffineMemoryAccessPattern &pattern =
                pipeline.memoryAccessPatterns[accessIndex];
            output << "{\"operation\":"
                   << jsonString(scheduledOperationName(context,
                                                        pattern.instruction))
                   << ",\"memory\":" << jsonString(pattern.memory)
                   << ",\"mode\":"
                   << jsonString(pattern.isWrite ? "write" : "read")
                   << ",\"affine\":"
                   << (pattern.affine ? "true" : "false")
                   << ",\"coefficient\":" << pattern.coefficient
                   << ",\"constant\":" << pattern.constant
                   << ",\"minimum\":" << pattern.minimum
                   << ",\"maximum\":" << pattern.maximum
                   << ",\"order\":" << pattern.order
                   << ",\"invariant_term_count\":"
                   << pattern.invariantTerms.size()
                   << ",\"bank_mapping_exact\":"
                   << (pattern.bankMappingExact ? "true" : "false")
                   << ",\"possible_banks\":[";
            for (std::size_t bankIndex = 0;
                 bankIndex < pattern.possibleBanks.size(); ++bankIndex)
            {
                if (bankIndex)
                    output << ',';
                output << pattern.possibleBanks[bankIndex];
            }
            output << ']'
                   << ",\"reason\":" << jsonString(pattern.reason) << '}';
        }
        output << "],\"memory_dependences\":[";
        for (std::size_t dependenceIndex = 0;
             dependenceIndex < pipeline.memoryDependences.size();
             ++dependenceIndex)
        {
            if (dependenceIndex)
                output << ',';
            const MemoryDependenceInfo &dependence =
                pipeline.memoryDependences[dependenceIndex];
            output << "{\"kind\":"
                   << jsonString(dependenceKindName(dependence.kind))
                   << ",\"memory\":" << jsonString(dependence.memory)
                   << ",\"source\":"
                   << jsonString(scheduledOperationName(context,
                                                        dependence.source))
                   << ",\"sink\":"
                   << jsonString(scheduledOperationName(context,
                                                        dependence.sink))
                   << ",\"source_event\":"
                   << jsonString(memoryEventKindName(dependence.sourceEvent))
                   << ",\"sink_event\":"
                   << jsonString(memoryEventKindName(dependence.sinkEvent))
                   << ",\"source_event_offset\":"
                   << dependence.sourceEventOffset
                   << ",\"sink_event_offset\":"
                   << dependence.sinkEventOffset
                   << ",\"minimum_separation\":"
                   << dependence.minimumSeparation
                   << ",\"distance\":" << dependence.distance
                   << ",\"loop_carried\":"
                   << (dependence.loopCarried ? "true" : "false")
                   << ",\"exists\":"
                   << (dependence.exists ? "true" : "false")
                   << ",\"exact\":"
                   << (dependence.exact ? "true" : "false")
                   << ",\"reason\":" << jsonString(dependence.reason)
                   << '}';
        }
        output << "]}";
    }
    output << "]}\n";
    return output.str();
}

std::string makeBindingJSON(const HLSBackendContext &context, bool detailed)
{
    std::ostringstream output;
    output << '{';
    writeCommonFields(output, context,
                      detailed ? "resource_binding" : "post_binding",
                      context.binding.valid && !context.hasErrors());
    output << ",\"qor_kind\":\"structural_post_binding\""
           << ",\"register_count\":" << context.binding.registerCount
           << ",\"register_bits\":" << context.binding.registerBits
           << ",\"state_register_bits\":"
           << context.binding.stateRegisterBits << ",\"mux_count\":"
           << context.binding.muxCount << ",\"mux_input_count\":"
           << context.binding.muxInputCount
           << ",\"operand_binding_cost_model\":\"selector_input_bits_v1\""
           << ",\"ordinal_mux_bit_cost\":" << context.binding.ordinalMuxBitCost
           << ",\"selected_mux_bit_cost\":" << context.binding.selectedMuxBitCost
           << ",\"affinity_rebound_groups\":" << context.binding.affinityReboundGroups
           << ",\"memory_object_count\":"
           << context.memoryObjects.size() << ",\"memory_port_count\":";
    unsigned memoryPortCount = 0;
    for (const MemoryObject &memory : context.memoryObjects)
        memoryPortCount += memory.ports * memory.banks;
    output << memoryPortCount << ",\"memory_access_count\":"
           << context.binding.memoryAccesses.size()
           << ",\"direct_local_write_port_count\":";
    const auto directPorts = directWritePortCounts(context);
    output << directPorts.first
           << ",\"direct_internal_pipo_write_port_count\":" << directPorts.second
           << ",\"note\":\"These are structural counts, not synthesized LUT/FF results.\"";
    if (detailed)
    {
        output << ",\"operator_instances\":[";
        for (std::size_t index = 0; index < context.binding.operators.size();
             ++index)
        {
            if (index)
                output << ',';
            const OperatorInstance &instance = context.binding.operators[index];
            output << "{\"name\":" << jsonString(instance.name)
                   << ",\"opcode\":" << jsonString(instance.opcode)
                   << ",\"bit_width\":" << instance.bitWidth
                   << ",\"invariant_operand_count\":" << instance.invariantOperands.size()
                   << ",\"operand_widths\":[";
            for (std::size_t operandIndex = 0;
                 operandIndex < instance.operandWidths.size(); ++operandIndex)
            {
                if (operandIndex)
                    output << ',';
                output << instance.operandWidths[operandIndex];
            }
            output << ']'
                   << ",\"operations\":";
            writeStringArray(output, instance.operations);
            output << '}';
        }
        output << "],\"operation_bindings\":[";
        for (std::size_t index = 0; index < context.binding.operations.size();
             ++index)
        {
            if (index)
                output << ',';
            const OperationBinding &binding = context.binding.operations[index];
            output << "{\"operation\":" << jsonString(binding.operation)
                   << ",\"instance\":" << jsonString(binding.instance) << '}';
        }
        output << "],\"memory_objects\":[";
        for (std::size_t index = 0; index < context.memoryObjects.size(); ++index)
        {
            if (index)
                output << ',';
            const MemoryObject &memory = context.memoryObjects[index];
            output << "{\"name\":" << jsonString(memory.name)
                   << ",\"protocol\":"
                   << jsonString(memory.storage ==
                                         MemoryStorageKind::ExternalAPMemory
                                     ? "ap_memory"
                                     : "internal_ram")
                   << ",\"storage\":"
                   << jsonString(memory.storage ==
                                         MemoryStorageKind::ExternalAPMemory
                                     ? "external"
                                     : "local")
                   << ",\"binding_type\":"
                   << jsonString(memoryBindingTypeName(memory.bindingType))
                   << ",\"implementation\":"
                   << jsonString(memoryImplementationName(
                          memory.implementation))
                   << ",\"storage_binding_explicit\":"
                   << (memory.storageBindingExplicit ? "true" : "false")
                   << ",\"port_name\":" << jsonString(memory.portName)
                   << ",\"element_width\":" << memory.elementWidth
                   << ",\"address_width\":" << memory.addressWidth
                   << ",\"depth\":";
            if (memory.depth == 0)
                output << "null";
            else
                output << memory.depth;
            output << ",\"read_latency\":" << memory.readLatency
                   << ",\"combinational_scalar_reads\":" << (memory.hasCombinationalScalarReads() ? "true" : "false")
                   << ",\"fifo_read_on_request\":" << (memory.fifoReadOnRequest ? "true" : "false")
                   << ",\"depth_source\":"
                   << jsonString(memoryDepthSourceName(memory.depthSource))
                   << ",\"depth_exact\":"
                   << (memory.depthExact ? "true" : "false")
                   << ",\"inferred_minimum_depth\":";
            if (memory.inferredMinimumDepth == 0)
                output << "null";
            else
                output << memory.inferredMinimumDepth;
            output
                   << ",\"partition_kind\":"
                   << jsonString(partitionKindName(memory.partition))
                   << ",\"dimensions\":[";
            for (std::size_t dimension = 0;
                 dimension < memory.dimensions.size(); ++dimension)
            {
                if (dimension)
                    output << ',';
                output << memory.dimensions[dimension];
            }
            output << "]"
                   << ",\"partition_dimensions\":[";
            for (std::size_t partitionIndex = 0;
                 partitionIndex < memory.partitionDimensions.size();
                 ++partitionIndex)
            {
                if (partitionIndex)
                    output << ',';
                const MemoryPartitionDimension &partition =
                    memory.partitionDimensions[partitionIndex];
                output << "{\"dimension\":" << (partition.dimension + 1)
                       << ",\"kind\":"
                       << jsonString(partitionKindName(partition.kind))
                       << ",\"extent\":" << partition.extent
                       << ",\"factor\":" << partition.factor
                       << ",\"local_extent\":" << partition.localExtent
                       << '}';
            }
            output << "]"
                   << ",\"banks\":" << memory.banks
                   << ",\"bank_depth\":" << memory.bankDepth
                   << ",\"bank_address_width\":"
                   << memory.bankAddressWidth
                   << ",\"bank_interface\":"
                   << jsonString(memoryBankInterface(memory))
                   << ",\"ports\":" << memory.ports
                   << ",\"access_mode\":"
                   << jsonString(memoryAccessModeName(memory.accessMode))
                   << ",\"port_usage\":[";
            for (unsigned port = 0; port < memory.portUsage.size(); ++port)
            {
                if (port)
                    output << ',';
                const MemoryPortUsage &usage = memory.portUsage[port];
                output << "{\"port\":" << port
                       << ",\"access_mode\":"
                       << jsonString(memoryAccessModeName(
                              portAccessMode(memory, port)))
                       << ",\"read\":"
                       << (usage.hasRead ? "true" : "false")
                       << ",\"write\":"
                       << (usage.hasWrite ? "true" : "false")
                       << ",\"rtl_signals\":";
                writeStringArray(output, memoryPortSignals(memory, port));
                output << '}';
            }
            output << "],\"bank_port_usage\":[";
            bool firstBankPort = true;
            for (unsigned bank = 0; bank < memory.bankPortUsage.size(); ++bank)
            {
                for (unsigned port = 0;
                     port < memory.bankPortUsage[bank].size(); ++port)
                {
                    const MemoryPortUsage &usage =
                        memory.bankPortUsage[bank][port];
                    if (!usage.hasRead && !usage.hasWrite)
                        continue;
                    if (!firstBankPort)
                        output << ',';
                    firstBankPort = false;
                    output << "{\"bank\":" << bank << ",\"port\":" << port
                           << ",\"access_mode\":"
                           << jsonString(memoryAccessModeName(
                                  bankPortAccessMode(memory, bank, port)))
                           << ",\"read\":"
                           << (usage.hasRead ? "true" : "false")
                           << ",\"write\":"
                           << (usage.hasWrite ? "true" : "false") << '}';
                }
            }
            output << "]}";
        }
        output << "],\"memory_access_bindings\":[";
        for (std::size_t index = 0;
             index < context.binding.memoryAccesses.size(); ++index)
        {
            if (index)
                output << ',';
            const MemoryAccessBinding &binding =
                context.binding.memoryAccesses[index];
            output << "{\"operation\":" << jsonString(binding.operation)
                   << ",\"memory\":" << jsonString(binding.memory)
                   << ",\"bank\":" << binding.bank
                   << ",\"bank_dynamic\":"
                   << (binding.bankDynamic ? "true" : "false")
                   << ",\"possible_banks\":[";
            for (std::size_t bankIndex = 0;
                 bankIndex < binding.possibleBanks.size(); ++bankIndex)
            {
                if (bankIndex)
                    output << ',';
                output << binding.possibleBanks[bankIndex];
            }
            output << "]"
                   << ",\"port\":" << binding.port
                   << ",\"direct_write_data_banks\":[";
            for (std::size_t bank = 0; bank < binding.directWriteDataBanks.size(); ++bank)
            {
                if (bank)
                    output << ',';
                output << binding.directWriteDataBanks[bank];
            }
            output << ']';
            const auto access = context.memoryAccesses.find(binding.instruction);
            if (access != context.memoryAccesses.end())
                output << ",\"is_write\":" << (access->second.isWrite ? "true" : "false")
                       << ",\"bank_mapping_exact\":" << (access->second.bankMappingExact ? "true" : "false")
                       << ",\"bank_analysis_source\":" << jsonString(access->second.bankAnalysisSource)
                       << ",\"bank_analysis_iterations\":" << access->second.bankAnalysisIterations
                       << ",\"relational_bank_trace_length\":" << access->second.iterationBanks.size()
                       << ",\"bank_analysis_reason\":" << jsonString(access->second.bankAnalysisReason);
            output << '}';
        }
        output << ']';
    }
    output << "}\n";
    return output.str();
}

std::string makeSynthesisJSON(const HLSBackendContext &context)
{
    const RTLSynthesisResult &synthesis = context.synthesis;
    std::ostringstream output;
    output << '{';
    writeCommonFields(output, context, "post_rtl_synthesis",
                      synthesis.valid && !context.hasErrors());
    output << ",\"qor_kind\":\"measured_post_rtl_synthesis\""
           << ",\"requested\":"
           << (context.configuration.runSynthesis ? "true" : "false")
           << ",\"attempted\":" << (synthesis.attempted ? "true" : "false")
           << ",\"status\":"
           << jsonString(synthesis.valid
                             ? "succeeded"
                             : (context.configuration.runSynthesis
                                    ? (synthesis.attempted ? "failed" : "pending")
                                    : "not_requested"))
           << ",\"tool\":" << jsonString(context.configuration.synthesisTool)
           << ",\"tool_version\":" << jsonString(synthesis.toolVersion)
           << ",\"part\":" << jsonString(context.configuration.synthesisPart)
           << ",\"synthesis_mode\":\"out_of_context\""
           << ",\"vivado_max_threads\":1"
           << ",\"vivado_cpu_affinity\":";
    if (synthesis.cpuAffinity < 0)
        output << "null";
    else
        output << synthesis.cpuAffinity;
    output << ",\"exit_code\":";
    if (!synthesis.attempted)
        output << "null";
    else
        output << synthesis.exitCode;
    output << ",\"runtime_seconds\":" << synthesis.runtimeSeconds
           << ",\"resources\":{";
    if (!synthesis.valid)
    {
        output << "\"lut\":null,\"ff\":null,\"dsp\":null,\"bram_tiles\":null";
    }
    else
    {
        output << "\"lut\":" << synthesis.lut << ",\"ff\":" << synthesis.ff
               << ",\"dsp\":" << synthesis.dsp << ",\"bram_tiles\":"
               << synthesis.bramTiles;
    }
    output << "},\"timing\":{";
    if (!synthesis.hasTiming)
    {
        output << "\"worst_slack_ns\":null,\"critical_path_delay_ns\":null";
    }
    else
    {
        output << "\"worst_slack_ns\":" << synthesis.worstSlackNs
               << ",\"critical_path_delay_ns\":"
               << context.configuration.clockPeriodNs - synthesis.worstSlackNs;
    }
    output << "},\"flow_id\":\"vivado_ooc_native_v3_pre_synth_io20\","
           << "\"io_budget_fraction\":0.2,"
           << "\"timing_note\":\"Period minus WNS includes external IO budgets; this is not a routed Fmax measurement.\","
           << "\"artifacts\":{"
           << "\"constraints\":\"vivado_report/constraints.xdc\","
           << "\"utilization_report\":\"vivado_report/utilization.rpt\","
           << "\"timing_report\":\"vivado_report/timing_summary.rpt\","
           << "\"vivado_log\":\"vivado_report/vivado.log\"}}\n";
    return output.str();
}

std::string makeManifestJSON(const HLSBackendContext &context)
{
    std::ostringstream output;
    output << '{';
    writeCommonFields(output, context, "manifest", !context.hasErrors());
    output << ",\"capabilities\":{"
           << "\"sequential_control_flow\":"
           << (context.capabilities.sequentialControlFlow ? "true" : "false")
           << ",\"integer_arithmetic\":"
           << (context.capabilities.integerArithmetic ? "true" : "false")
           << ",\"loop_pipeline\":"
           << (context.capabilities.loopPipeline ? "true" : "false")
           << ",\"array_partition\":"
           << (context.capabilities.arrayPartition ? "true" : "false")
           << ",\"dataflow\":"
           << (context.capabilities.dataflow ? "true" : "false")
           << ",\"floating_point\":"
           << (context.capabilities.floatingPoint ? "true" : "false")
           << ",\"memory_interface\":"
           << (context.capabilities.memoryInterface ? "true" : "false")
           << ",\"stream_interface\":"
           << (context.capabilities.streamInterface ? "true" : "false")
           << "},\"artifacts\":{"
           << "\"human_report_index\":\"report.rpt\","
           << "\"estimate\":\"estimate/pre_backend_estimate.json\","
           << "\"estimate_report\":\"estimate/pre_backend_estimate.rpt\","
           << "\"schedule\":\"scheduling/schedule.json\","
           << "\"schedule_report\":\"scheduling/schedule.rpt\","
           << "\"pipeline_schedule\":\"scheduling/pipeline_schedule.json\","
           << "\"pipeline_schedule_report\":\"scheduling/pipeline_schedule.rpt\","
           << "\"binding\":\"binding/binding.json\","
           << "\"binding_report\":\"binding/binding.rpt\","
           << "\"rtl\":"
           << jsonString(context.rtl.valid ? "rtl/top.v" : "rtl/failure.json")
           << ','
           << "\"rtl_report\":\"rtl/rtl_generation.rpt\","
           << "\"synthesis\":\"vivado_report/vivado_report.json\","
           << "\"synthesis_report\":\"vivado_report/vivado_report.rpt\","
           << "\"comparison\":\"summary/qor_comparison.json\","
           << "\"comparison_report\":\"summary/qor_summary.rpt\","
           << "\"intermediate_directory\":\".lighthls\"}}\n";
    return output.str();
}

std::string makeComparisonJSON(const HLSBackendContext &context)
{
    std::ostringstream output;
    output << '{';
    writeCommonFields(output, context, "qor_comparison", !context.hasErrors());
    output << ",\"pre_backend_estimate\":{";
    output << "\"latency_cycles\":" << context.estimate.latencyCycles
           << ",\"dsp\":" << context.estimate.dsp << ",\"ff\":"
           << context.estimate.ff << ",\"lut\":" << context.estimate.lut
           << ",\"bram\":" << context.estimate.bram << "},"
           << "\"post_scheduling\":{";
    output << "\"latency_cycles\":";
    if (context.schedule.latencyCycles < 0)
        output << "null";
    else
        output << context.schedule.latencyCycles;
    output << ",\"state_count\":" << context.schedule.stateCount << "},"
           << "\"post_binding\":{";
    output << "\"register_count\":" << context.binding.registerCount
           << ",\"register_bits\":" << context.binding.registerBits
           << ",\"operator_instances\":"
           << context.binding.operators.size() << ",\"memory_objects\":"
           << context.memoryObjects.size() << ",\"memory_accesses\":"
           << context.binding.memoryAccesses.size() << "},"
           << "\"post_rtl_synthesis\":";
    if (!context.synthesis.valid)
    {
        output << "null";
    }
    else
    {
        output << "{\"tool\":" << jsonString(context.synthesis.tool)
               << ",\"tool_version\":"
               << jsonString(context.synthesis.toolVersion)
               << ",\"part\":" << jsonString(context.synthesis.part)
               << ",\"lut\":" << context.synthesis.lut
               << ",\"ff\":" << context.synthesis.ff
               << ",\"dsp\":" << context.synthesis.dsp
               << ",\"bram_tiles\":" << context.synthesis.bramTiles;
        if (context.synthesis.hasTiming)
            output << ",\"worst_slack_ns\":"
                   << context.synthesis.worstSlackNs;
        output << '}';
    }
    output << ",\"comparison_note\":\"Pre-backend values are heuristic, post-binding values are structural, and post-RTL synthesis values are measured for the selected FPGA part.\"}\n";
    return output.str();
}

} // namespace

void HLSBackendContext::addError(const std::string &code,
                                 const std::string &message,
                                 const std::string &location)
{
    diagnostics.push_back({DiagnosticSeverity::Error, code, message, location});
}

void HLSBackendContext::addWarning(const std::string &code,
                                   const std::string &message,
                                   const std::string &location)
{
    diagnostics.push_back({DiagnosticSeverity::Warning, code, message, location});
}

bool HLSBackendContext::hasErrors() const
{
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [](const BackendDiagnostic &diagnostic) {
                           return diagnostic.severity == DiagnosticSeverity::Error;
                       });
}

HLSBackendContextPtr createBackendContext(
    llvm::Module &module, const BackendConfiguration &configuration)
{
    auto context = std::make_shared<HLSBackendContext>();
    context->module = &module;
    context->configuration = configuration;
    context->synthesis.requested = configuration.runSynthesis;
    context->synthesis.tool = configuration.synthesisTool;
    context->synthesis.part = configuration.synthesisPart;

    for (llvm::Function &function : module)
    {
        if (function.isDeclaration())
            continue;
        const std::string functionName = function.getName().str();
        const std::string demangled = llvm::demangle(functionName);
        bool mangledBaseNameMatches = false;
        if (functionName.rfind("_Z", 0) == 0)
        {
            std::size_t begin = 2;
            std::size_t end = begin;
            while (end < functionName.size() &&
                   std::isdigit(static_cast<unsigned char>(functionName[end])))
            {
                ++end;
            }
            if (end > begin)
            {
                const std::size_t length = static_cast<std::size_t>(
                    std::stoul(functionName.substr(begin, end - begin)));
                mangledBaseNameMatches =
                    end + length <= functionName.size() &&
                    functionName.substr(end, length) ==
                        configuration.topFunction;
            }
        }
        if (functionName == configuration.topFunction ||
            demangled == configuration.topFunction ||
            demangled.rfind(configuration.topFunction + "(", 0) == 0 ||
            mangledBaseNameMatches)
        {
            context->top = &function;
            break;
        }
    }
    return context;
}

std::string memoryDepthSourceName(MemoryDepthSource source)
{
    switch (source)
    {
    case MemoryDepthSource::Frontend:
        return "frontend";
    case MemoryDepthSource::Configuration:
        return "configuration";
    case MemoryDepthSource::Allocation:
        return "allocation";
    case MemoryDepthSource::Inferred:
        return "inferred";
    case MemoryDepthSource::Unknown:
        return "unknown";
    }
    return "unknown";
}

std::string memoryAccessModeName(MemoryAccessMode mode)
{
    switch (mode)
    {
    case MemoryAccessMode::Unused:
        return "unused";
    case MemoryAccessMode::ReadOnly:
        return "read_only";
    case MemoryAccessMode::WriteOnly:
        return "write_only";
    case MemoryAccessMode::ReadWrite:
        return "read_write";
    }
    return "unused";
}

std::string memoryBindingTypeName(MemoryBindingType type)
{
    switch (type)
    {
    case MemoryBindingType::Automatic:
        return "auto";
    case MemoryBindingType::RAM1P:
        return "ram_1p";
    case MemoryBindingType::RAM2P:
        return "ram_2p";
    }
    return "auto";
}

std::string memoryImplementationName(MemoryImplementation implementation)
{
    switch (implementation)
    {
    case MemoryImplementation::Automatic:
        return "auto";
    case MemoryImplementation::BlockRAM:
        return "bram";
    case MemoryImplementation::UltraRAM:
        return "uram";
    case MemoryImplementation::DistributedRAM:
        return "lutram";
    }
    return "auto";
}

std::string partitionKindName(PartitionKind kind)
{
    switch (kind)
    {
    case PartitionKind::None:
        return "none";
    case PartitionKind::Cyclic:
        return "cyclic";
    case PartitionKind::Block:
        return "block";
    case PartitionKind::Complete:
        return "complete";
    }
    return "unknown";
}

unsigned partitionBankForAddress(const MemoryObject &memory,
                                 unsigned long long address)
{
    if (memory.banks <= 1 || memory.dimensions.empty())
        return 0;

    unsigned bank = 0;
    unsigned bankStride = 1;
    unsigned long long coordinateStride = 1;
    for (std::size_t reverse = memory.dimensions.size(); reverse > 0;
         --reverse)
    {
        const unsigned dimension = static_cast<unsigned>(reverse - 1);
        const unsigned long long extent = memory.dimensions[dimension];
        const unsigned long long coordinate =
            extent == 0 ? 0 : (address / coordinateStride) % extent;
        const auto partition = std::find_if(
            memory.partitionDimensions.begin(),
            memory.partitionDimensions.end(),
            [dimension](const MemoryPartitionDimension &candidate) {
                return candidate.dimension == dimension;
            });
        if (partition != memory.partitionDimensions.end())
        {
            unsigned component = 0;
            if (partition->kind == PartitionKind::Block)
            {
                const unsigned long long blockSize =
                    std::max(1ULL, partition->localExtent);
                component = static_cast<unsigned>(
                    std::min<unsigned long long>(coordinate / blockSize,
                                                 partition->factor - 1));
            }
            else
            {
                component = static_cast<unsigned>(coordinate %
                                                  partition->factor);
            }
            bank += component * bankStride;
            bankStride *= partition->factor;
        }
        if (extent != 0)
            coordinateStride *= extent;
    }
    return bank;
}

unsigned long long partitionLocalAddressForAddress(
    const MemoryObject &memory, unsigned long long address)
{
    if (memory.banks <= 1 || memory.dimensions.empty())
        return address;

    unsigned long long localAddress = 0;
    unsigned long long localStride = 1;
    unsigned long long coordinateStride = 1;
    for (std::size_t reverse = memory.dimensions.size(); reverse > 0;
         --reverse)
    {
        const unsigned dimension = static_cast<unsigned>(reverse - 1);
        const unsigned long long extent = memory.dimensions[dimension];
        const unsigned long long coordinate =
            extent == 0 ? 0 : (address / coordinateStride) % extent;
        const auto partition = std::find_if(
            memory.partitionDimensions.begin(),
            memory.partitionDimensions.end(),
            [dimension](const MemoryPartitionDimension &candidate) {
                return candidate.dimension == dimension;
            });
        unsigned long long localCoordinate = coordinate;
        unsigned long long localExtent = extent;
        if (partition != memory.partitionDimensions.end())
        {
            localExtent = partition->localExtent;
            if (partition->kind == PartitionKind::Block)
                localCoordinate = coordinate % std::max(1ULL, localExtent);
            else
                localCoordinate = coordinate / partition->factor;
        }
        localAddress += localCoordinate * localStride;
        if (localExtent != 0)
            localStride *= localExtent;
        if (extent != 0)
            coordinateStride *= extent;
    }
    return localAddress;
}

std::string dependenceKindName(DependenceKind kind)
{
    switch (kind)
    {
    case DependenceKind::ScalarRecurrence:
        return "scalar_recurrence";
    case DependenceKind::MemoryRAW:
        return "memory_raw";
    case DependenceKind::MemoryWAR:
        return "memory_war";
    case DependenceKind::MemoryWAW:
        return "memory_waw";
    case DependenceKind::FIFOOrder:
        return "fifo_order";
    }
    return "unknown";
}

std::string memoryEventKindName(MemoryEventKind kind)
{
    switch (kind)
    {
    case MemoryEventKind::ReadRequest:
        return "read_request";
    case MemoryEventKind::WriteRequest:
        return "write_request";
    }
    return "unknown";
}

namespace
{

std::string normalizeVerilogIdentifier(const std::string &name,
                                       const std::string &fallback)
{
    std::string result;
    result.reserve(name.size() + 1);
    for (char character : name)
    {
        const unsigned char value = static_cast<unsigned char>(character);
        if (std::isalnum(value) || character == '_')
            result.push_back(character);
        else
            result.push_back('_');
    }
    if (result.empty())
        result = fallback;
    if (std::isdigit(static_cast<unsigned char>(result.front())))
        result.insert(result.begin(), '_');
    return result;
}

bool isVerilogKeyword(const std::string &name)
{
    static const std::set<std::string> keywords = {
        "always", "and", "assign", "automatic", "begin", "buf", "bufif0",
        "bufif1", "case", "casex", "casez", "cell", "cmos", "config",
        "deassign", "default", "defparam", "design", "disable", "edge",
        "else", "end", "endcase", "endconfig", "endfunction",
        "endgenerate", "endmodule", "endprimitive", "endspecify",
        "endtable", "endtask", "event", "for", "force", "forever", "fork",
        "function", "generate", "genvar", "highz0", "highz1", "if",
        "ifnone", "incdir", "include", "initial", "inout", "input",
        "instance", "integer", "join", "large", "liblist", "library",
        "localparam", "macromodule", "medium", "module", "nand", "negedge",
        "nmos", "nor", "noshowcancelled", "not", "notif0", "notif1", "or",
        "output", "parameter", "pmos", "posedge", "primitive", "pull0",
        "pull1", "pulldown", "pullup", "pulsestyle_onevent",
        "pulsestyle_ondetect", "rcmos", "real", "realtime", "reg", "release",
        "repeat", "rnmos", "rpmos", "rtran", "rtranif0", "rtranif1",
        "scalared", "showcancelled", "signed", "small", "specify",
        "specparam", "strong0", "strong1", "supply0", "supply1", "table",
        "task", "time", "tran", "tranif0", "tranif1", "tri", "tri0", "tri1",
        "triand", "trior", "trireg", "unsigned", "use", "vectored", "wait",
        "wand", "weak0", "weak1", "while", "wire", "wor", "xnor", "xor"};
    return keywords.count(name) != 0;
}

} // namespace

std::string sanitizeVerilogIdentifier(const std::string &name,
                                      const std::string &fallback)
{
    std::string result = normalizeVerilogIdentifier(name, fallback);
    if (isVerilogKeyword(result))
        result += "_v";
    return result;
}

std::string sanitizeVerilogPortIdentifier(const std::string &name,
                                          const std::string &fallback)
{
    std::string result = normalizeVerilogIdentifier(name, fallback);
    if (isVerilogKeyword(result))
        result += "_r";
    return result;
}

std::string stableFileHash(const std::string &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return "unavailable";
    unsigned long long hash = 14695981039346656037ULL;
    char character = 0;
    while (input.get(character))
    {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << "fnv1a64:" << std::hex << std::setw(16) << std::setfill('0')
           << hash;
    return output.str();
}

bool emitBackendArtifacts(const HLSBackendContext &context,
                          std::string &errorMessage)
{
    namespace fs = std::filesystem;
    const fs::path root(context.configuration.outputDirectory);
    std::error_code error;
    for (const char *directory : {".lighthls", "estimate", "scheduling",
                                  "binding", "rtl", "vivado_report", "summary"})
    {
        fs::create_directories(root / directory, error);
        if (error)
        {
            errorMessage = "cannot create backend output directory '" +
                           (root / directory).string() + "': " + error.message();
            return false;
        }
    }

    if (!writeFile(root / "manifest.json", makeManifestJSON(context), errorMessage) ||
        !writeFile(root / "report.rpt", makeReportIndex(context), errorMessage) ||
        !writeFile(root / "estimate/pre_backend_estimate.json",
                   makeEstimateJSON(context), errorMessage) ||
        !writeFile(root / "estimate/pre_backend_estimate.rpt",
                   makeEstimateReport(context), errorMessage) ||
        !writeFile(root / "scheduling/schedule.json",
                   makeScheduleJSON(context, true), errorMessage) ||
        !writeFile(root / "scheduling/schedule.rpt",
                   makeScheduleReport(context), errorMessage) ||
        !writeFile(root / "scheduling/pipeline_schedule.json",
                   makePipelineScheduleJSON(context), errorMessage) ||
        !writeFile(root / "scheduling/pipeline_schedule.rpt",
                   makePipelineScheduleReport(context), errorMessage) ||
        !writeFile(root / "scheduling/dataflow_graph.json",
                   makeDataflowJSON(context), errorMessage) ||
        !writeFile(root / "scheduling/dataflow_graph.rpt",
                   makeDataflowReport(context), errorMessage) ||
        !writeFile(root / "scheduling/post_schedule_qor.json",
                   makeScheduleJSON(context, false), errorMessage) ||
        !writeFile(root / "binding/binding.json",
                   makeBindingJSON(context, true), errorMessage) ||
        !writeFile(root / "binding/binding.rpt",
                   makeBindingReport(context), errorMessage) ||
        !writeFile(root / "binding/post_binding_qor.json",
                   makeBindingJSON(context, false), errorMessage) ||
        !writeFile(root / "rtl/rtl_generation.rpt",
                   makeRTLReport(context), errorMessage) ||
        !writeFile(root / "vivado_report/vivado_report.json",
                   makeSynthesisJSON(context), errorMessage) ||
        !writeFile(root / "vivado_report/vivado_report.rpt",
                   makeSynthesisReport(context), errorMessage) ||
        !writeFile(root / "summary/qor_comparison.json",
                   makeComparisonJSON(context), errorMessage) ||
        !writeFile(root / "summary/qor_summary.rpt",
                   makeComparisonReport(context), errorMessage))
    {
        return false;
    }

    if (context.rtl.valid && !context.rtl.verilog.empty())
    {
        if (!writeFile(root / "rtl/top.v", context.rtl.verilog, errorMessage))
            return false;
    }
    else
    {
        std::ostringstream failure;
        failure << '{';
        writeCommonFields(failure, context, "rtl", false);
        failure << "}\n";
        if (!writeFile(root / "rtl/failure.json", failure.str(), errorMessage))
            return false;
    }
    return true;
}

std::set<std::string> dataflowCompletionTasks(const DataflowGraph &graph)
{
    std::set<std::string> completions;
    for (const DataflowTask &task : graph.tasks)
    {
        bool impliedByConsumer = false;
        if (!graph.cyclic)
            for (const DataflowChannel &channel : graph.channels)
            {
                if (channel.producer != task.name || !channel.rateKnown ||
                    channel.productionRate == 0 || channel.productionRate != channel.consumptionRate ||
                    channel.initialTokens != 0)
                    continue;
                // PIPO ownership transfers only after producer completion.
                // FIFO data implies completion only when the final token is
                // emitted on the producer's final enabled edge. Earlier tokens
                // cannot cover trailing memory effects or cleanup work.
                const __int128 lastWrite = static_cast<__int128>(channel.producerFirstAccessCycle) +
                    static_cast<__int128>(channel.productionRate - 1) * channel.producerAccessInterval;
                impliedByConsumer |= channel.type == "pipo" ||
                    (channel.type == "fifo" && task.latencyExact && task.latencyCycles > 0 &&
                     channel.producerAccessTimingExact && channel.producerFirstAccessCycle > 0 &&
                     (channel.productionRate == 1 || channel.producerAccessInterval > 0) &&
                     lastWrite == task.latencyCycles);
            }
        if (!impliedByConsumer)
            completions.insert(task.name);
    }
    return completions;
}

double ordinaryEntryValueDelay(const HLSBackendContext &context, const llvm::Value *value)
{
    std::map<const llvm::Value *, double> cache;
    std::function<double(const llvm::Value *)> delay = [&](const llvm::Value *current) -> double {
        if (!current || !current->getType()->isIntegerTy())
            return INFINITY;
        if (llvm::isa<llvm::Argument>(current) || llvm::isa<llvm::ConstantInt>(current) ||
            context.constantValues.count(current))
            return 0;
        const auto cached = cache.find(current);
        if (cached != cache.end())
            return cached->second;
        const auto *instruction = llvm::dyn_cast<llvm::Instruction>(current);
        if (!instruction || instruction->getParent() != &context.top->getEntryBlock() ||
            instruction->mayReadOrWriteMemory() || instruction->mayHaveSideEffects())
            return INFINITY;
        const bool wiring = llvm::isa<llvm::FreezeInst>(instruction) ||
            (llvm::isa<llvm::CastInst>(instruction) && instruction->getOperand(0)->getType()->isIntegerTy());
        if (!wiring && !llvm::isa<llvm::BinaryOperator>(instruction) &&
            !llvm::isa<llvm::ICmpInst>(instruction) && !llvm::isa<llvm::SelectInst>(instruction))
            return INFINITY;
        const auto modeledDelay = context.delayModel.find(instruction);
        const auto latency = context.latencyModel.find(instruction);
        if (!wiring && (modeledDelay == context.delayModel.end() || latency == context.latencyModel.end() ||
            latency->second != 0 || !std::isfinite(modeledDelay->second) || modeledDelay->second < 0))
            return INFINITY;
        cache[current] = INFINITY;
        double inputDelay = 0;
        for (const llvm::Use &operand : instruction->operands())
            inputDelay = std::max(inputDelay, delay(operand.get()));
        return cache[current] = inputDelay + (wiring ? 0 : modeledDelay->second);
    };
    return delay(value);
}

} // namespace backend
} // namespace light_hls
