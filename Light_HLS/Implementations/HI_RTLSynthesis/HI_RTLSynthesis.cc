#include "HI_RTLSynthesis.h"

#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifdef __linux__
#include <sched.h>
#endif

using namespace llvm;
using namespace light_hls::backend;

namespace
{

std::string tclPath(const std::filesystem::path &path)
{
    std::string value = path.lexically_normal().string();
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('{');
    for (char character : value)
    {
        if (character == '}' || character == '\\')
            escaped.push_back('\\');
        escaped.push_back(character);
    }
    escaped.push_back('}');
    return escaped;
}

bool writeTextFile(const std::filesystem::path &path,
                   const std::string &content, std::string &error)
{
    std::ofstream output(path);
    if (!output)
    {
        error = "cannot open '" + path.string() + "'";
        return false;
    }
    output << content;
    if (!output)
    {
        error = "cannot write '" + path.string() + "'";
        return false;
    }
    return true;
}

std::map<std::string, std::string>
readMetrics(const std::filesystem::path &path)
{
    std::map<std::string, std::string> metrics;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line))
    {
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos)
            continue;
        metrics[line.substr(0, separator)] = line.substr(separator + 1);
    }
    return metrics;
}

bool parseIntegerMetric(const std::map<std::string, std::string> &metrics,
                        const std::string &name, long long &value)
{
    auto found = metrics.find(name);
    if (found == metrics.end())
        return false;
    try
    {
        std::size_t consumed = 0;
        value = std::stoll(found->second, &consumed);
        return consumed == found->second.size();
    }
    catch (const std::exception &)
    {
        return false;
    }
}

bool parseDoubleMetric(const std::map<std::string, std::string> &metrics,
                       const std::string &name, double &value)
{
    auto found = metrics.find(name);
    if (found == metrics.end())
        return false;
    try
    {
        std::size_t consumed = 0;
        value = std::stod(found->second, &consumed);
        return consumed == found->second.size();
    }
    catch (const std::exception &)
    {
        return false;
    }
}

std::string vivadoFailureMessage(int exitCode,
                                 const std::string &executionError)
{
    std::ostringstream message;
    message << "Vivado synthesis failed with exit code " << exitCode;
    if (exitCode == 134)
        message << " (SIGABRT: Vivado terminated internally)";
    else if (exitCode == 139)
        message << " (SIGSEGV: Vivado terminated internally)";
    if (!executionError.empty())
        message << ": " << executionError;
    message << ". Inspect vivado.log and vivado.err for the tool diagnostic.";
    return message.str();
}

std::optional<unsigned> firstAvailableCPU()
{
#ifdef __linux__
    cpu_set_t availableCPUs;
    CPU_ZERO(&availableCPUs);
    if (sched_getaffinity(0, sizeof(availableCPUs), &availableCPUs) != 0)
        return std::nullopt;
    for (unsigned cpu = 0; cpu < static_cast<unsigned>(CPU_SETSIZE); ++cpu)
    {
        if (CPU_ISSET(cpu, &availableCPUs))
            return cpu;
    }
#endif
    return std::nullopt;
}

std::string makeVivadoScript(const HLSBackendContext &context,
                             const std::filesystem::path &rtlPath,
                             const std::filesystem::path &synthesisDirectory)
{
    const std::filesystem::path utilizationPath =
        synthesisDirectory / "utilization.rpt";
    const std::filesystem::path timingPath =
        synthesisDirectory / "timing_summary.rpt";
    const std::filesystem::path checkpointPath =
        synthesisDirectory / "post_synth.dcp";
    const std::filesystem::path metricsPath =
        synthesisDirectory / "vivado_metrics.txt";

    std::ostringstream script;
    script << "cd " << tclPath(synthesisDirectory) << '\n'
           << "set_param general.maxThreads 1\n"
           << "read_verilog -sv " << tclPath(rtlPath) << '\n'
           << "read_xdc -unmanaged " << tclPath(synthesisDirectory / "constraints.xdc") << '\n'
           << "synth_design -top {" << context.rtl.moduleName << "} -part {"
           << context.configuration.synthesisPart
           << "} -mode out_of_context -flatten_hierarchy rebuilt\n"
           << "if {[llength [get_clocks -quiet ap_clk]] != 1} { error \"Required pre-synthesis clock is missing\" }\n"
           << "write_checkpoint -force " << tclPath(checkpointPath) << '\n'
           << "report_utilization -file " << tclPath(utilizationPath) << '\n'
           << "report_timing_summary -delay_type max -max_paths 10 -file "
           << tclPath(timingPath) << '\n'
           << "set utilization [report_utilization -return_string]\n"
           << "set lut -1\n"
           << "set ff -1\n"
           << "set dsp -1\n"
           << "set bram -1\n"
           << "regexp -line {\\|\\s*(Slice LUTs|CLB LUTs)\\*?\\s*\\|\\s*([0-9]+)} $utilization match label lut\n"
           << "regexp -line {\\|\\s*(Slice Registers|CLB Registers)\\s*\\|\\s*([0-9]+)} $utilization match label ff\n"
           << "regexp -line {\\|\\s*DSPs\\s*\\|\\s*([0-9]+)} $utilization match dsp\n"
           << "regexp -line {\\|\\s*Block RAM Tile\\s*\\|\\s*([0-9.]+)} $utilization match bram\n"
           << "set wns nan\n"
           << "set timing_paths [get_timing_paths -delay_type max -max_paths 1]\n"
           << "if {[llength $timing_paths] > 0} { set wns [get_property SLACK [lindex $timing_paths 0]] }\n"
           << "set metrics [open " << tclPath(metricsPath) << " w]\n"
           << "puts $metrics \"tool_version=[version -short]\"\n"
           << "puts $metrics \"lut=$lut\"\n"
           << "puts $metrics \"ff=$ff\"\n"
           << "puts $metrics \"dsp=$dsp\"\n"
           << "puts $metrics \"bram_tiles=$bram\"\n"
           << "puts $metrics \"worst_slack_ns=$wns\"\n"
           << "puts $metrics \"flow_id=vivado_ooc_native_v3_pre_synth_io20\"\n"
           << "puts $metrics \"io_budget_fraction=0.2\"\n"
           << "close $metrics\n"
           << "exit\n";
    return script.str();
}

} // namespace

char HI_RTLSynthesis::ID = 0;

HI_RTLSynthesis::HI_RTLSynthesis(HLSBackendContextPtr context)
    : ModulePass(ID), context(std::move(context))
{
}

bool HI_RTLSynthesis::runOnModule(Module &module)
{
    if (!context)
        return false;
    context->module = &module;
    RTLSynthesisResult &result = context->synthesis;
    result.requested = context->configuration.runSynthesis;
    result.tool = context->configuration.synthesisTool;
    result.part = context->configuration.synthesisPart;
    if (!result.requested)
        return false;

    result.attempted = true;
    auto fail = [&](const std::string &code, const std::string &message) {
        context->addError(code, message,
                          context->configuration.outputDirectory +
                              "/vivado_report");
        std::string artifactError;
        emitBackendArtifacts(*context, artifactError);
    };

    if (!context->rtl.valid || context->rtl.verilog.empty())
    {
        fail("BACKEND_SYNTHESIS_NO_RTL",
             "RTL synthesis was requested, but no valid Verilog design exists.");
        return false;
    }
    if (context->configuration.synthesisTool != "vivado")
    {
        fail("BACKEND_SYNTHESIS_TOOL_UNSUPPORTED",
             "Only the Vivado post-RTL synthesis adapter is currently supported.");
        return false;
    }
    if (context->configuration.synthesisPart.empty())
    {
        fail("BACKEND_SYNTHESIS_PART_UNKNOWN",
             "The selected board does not provide a synthesis part.");
        return false;
    }

    auto executable =
        sys::findProgramByName(context->configuration.synthesisExecutable);
    if (!executable)
    {
        fail("BACKEND_SYNTHESIS_TOOL_NOT_FOUND",
             "Cannot find the Vivado executable '" +
                 context->configuration.synthesisExecutable + "'.");
        return false;
    }

    namespace fs = std::filesystem;
    const fs::path outputRoot(context->configuration.outputDirectory);
    const fs::path synthesisDirectory = outputRoot / "vivado_report";
    std::error_code directoryError;
    fs::create_directories(synthesisDirectory, directoryError);
    if (directoryError)
    {
        fail("BACKEND_SYNTHESIS_DIRECTORY_FAILED",
             "Cannot create the synthesis output directory: " +
                 directoryError.message());
        return false;
    }

    const fs::path scriptPath = synthesisDirectory / "run_vivado.tcl";
    const fs::path metricsPath = synthesisDirectory / "vivado_metrics.txt";
    std::string fileError;
    std::ostringstream constraints;
    constraints << "# Apply the target clock and a symmetric external IO budget before synthesis.\n"
                << "create_clock -name ap_clk -period " << context->configuration.clockPeriodNs
                << " [get_ports ap_clk]\n"
                << "set data_inputs [get_ports -filter {DIRECTION == IN && NAME != ap_clk}]\n"
                << "set_input_delay -clock ap_clk -max " << context->configuration.clockPeriodNs * 0.2
                << " $data_inputs\nset_input_delay -clock ap_clk -min 0 $data_inputs\n"
                << "set_output_delay -clock ap_clk -max " << context->configuration.clockPeriodNs * 0.2
                << " [all_outputs]\nset_output_delay -clock ap_clk -min 0 [all_outputs]\n";
    if (!writeTextFile(synthesisDirectory / "constraints.xdc", constraints.str(), fileError))
    {
        fail("BACKEND_SYNTHESIS_CONSTRAINTS_FAILED", fileError);
        return false;
    }
    if (!writeTextFile(scriptPath,
                       makeVivadoScript(*context, outputRoot / "rtl/top.v",
                                        synthesisDirectory),
                       fileError))
    {
        fail("BACKEND_SYNTHESIS_SCRIPT_FAILED", fileError);
        return false;
    }

    const fs::path stdoutPath = synthesisDirectory / "vivado.log";
    const fs::path stderrPath = synthesisDirectory / "vivado.err";
    const std::vector<fs::path> staleArtifacts = {
        stdoutPath,
        stderrPath,
        metricsPath,
        synthesisDirectory / "post_synth.dcp",
        synthesisDirectory / "utilization.rpt",
        synthesisDirectory / "timing_summary.rpt"};
    for (const fs::path &artifact : staleArtifacts)
    {
        std::error_code cleanupError;
        fs::remove(artifact, cleanupError);
        if (cleanupError)
        {
            fail("BACKEND_SYNTHESIS_CLEANUP_FAILED",
                 "Cannot remove stale Vivado artifact '" +
                     artifact.string() + "': " + cleanupError.message());
            return false;
        }
    }
    const fs::path temporaryDirectory = synthesisDirectory / ".vivado_tmp";
    fs::create_directories(temporaryDirectory, directoryError);
    if (directoryError)
    {
        fail("BACKEND_SYNTHESIS_DIRECTORY_FAILED",
             "Cannot create the Vivado temporary directory: " +
                 directoryError.message());
        return false;
    }
    const std::string executablePath = *executable;
    const std::string scriptPathString = scriptPath.string();
    const std::string stdoutPathString = stdoutPath.string();
    const std::string stderrPathString = stderrPath.string();
    const std::string temporaryDirectoryString = temporaryDirectory.string();
    std::string launchExecutable = executablePath;
    std::vector<std::string> argumentStorage;
    const std::optional<unsigned> affinityCPU = firstAvailableCPU();
    auto taskset = sys::findProgramByName("taskset");
    if (affinityCPU && taskset)
    {
        launchExecutable = *taskset;
        result.cpuAffinity = static_cast<int>(*affinityCPU);
        argumentStorage = {launchExecutable, "-c", std::to_string(*affinityCPU),
                           executablePath};
    }
    else
    {
        argumentStorage = {executablePath};
    }
    argumentStorage.insert(argumentStorage.end(),
                           {"-mode", "batch", "-nolog", "-nojournal",
                            "-tempDir", temporaryDirectoryString, "-source",
                            scriptPathString});
    std::vector<StringRef> arguments;
    arguments.reserve(argumentStorage.size());
    for (const std::string &argument : argumentStorage)
        arguments.emplace_back(argument);
    const std::optional<StringRef> noRedirect;
    const std::optional<StringRef> stdoutRedirect(stdoutPathString);
    const std::optional<StringRef> stderrRedirect(stderrPathString);
    const std::vector<std::optional<StringRef>> redirects = {
        noRedirect, stdoutRedirect, stderrRedirect};
    std::string executionError;
    bool executionFailed = false;
    const auto begin = std::chrono::steady_clock::now();
    result.exitCode = sys::ExecuteAndWait(
        launchExecutable, arguments, std::nullopt, redirects,
        context->configuration.synthesisTimeoutSeconds, 0, &executionError,
        &executionFailed);
    const auto end = std::chrono::steady_clock::now();
    result.runtimeSeconds =
        std::chrono::duration<double>(end - begin).count();
    if (executionFailed || result.exitCode != 0)
    {
        fail("BACKEND_SYNTHESIS_EXECUTION_FAILED",
             vivadoFailureMessage(result.exitCode, executionError));
        return false;
    }

    const std::map<std::string, std::string> metrics = readMetrics(metricsPath);
    auto version = metrics.find("tool_version");
    if (version != metrics.end())
        result.toolVersion = version->second;
    const bool resourcesValid =
        parseIntegerMetric(metrics, "lut", result.lut) &&
        parseIntegerMetric(metrics, "ff", result.ff) &&
        parseIntegerMetric(metrics, "dsp", result.dsp) &&
        parseDoubleMetric(metrics, "bram_tiles", result.bramTiles) &&
        result.lut >= 0 && result.ff >= 0 && result.dsp >= 0 &&
        result.bramTiles >= 0.0;
    result.hasTiming =
        parseDoubleMetric(metrics, "worst_slack_ns", result.worstSlackNs);
    result.valid = resourcesValid;
    if (!resourcesValid)
    {
        fail("BACKEND_SYNTHESIS_METRICS_INVALID",
             "Vivado completed, but the measured utilization metrics could not be parsed.");
        return false;
    }

    std::string artifactError;
    if (!emitBackendArtifacts(*context, artifactError))
    {
        context->addError("BACKEND_ARTIFACT_WRITE_FAILED", artifactError,
                          context->configuration.outputDirectory);
    }
    return false;
}

void HI_RTLSynthesis::getAnalysisUsage(AnalysisUsage &usage) const
{
    usage.setPreservesAll();
}
