#include "Light_HLS_Top.h"
#include "ConfigParse.h"
#include "llvm/Support/Signals.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <regex>
#include <sstream>

using namespace llvm;
using namespace polly;

// using namespace clang;
using namespace clang::driver;
using namespace clang::tooling;

std::string HLS_lib_path;
std::string clock_period_str;
double DSP_limit;
double FF_limit;
double LUT_limit;
double BRAM_limit;
int ClockNum_limit;
bool all_sub_function_inline;
bool dataflowApplied;

namespace
{

bool parseTargetFrequency(const std::string &text, double &frequencyMHz,
                          double &clockPeriodNs, std::string &error)
{
    std::smatch match;
    const std::regex pattern(
        "^([0-9]+(?:\\.[0-9]+)?)\\s*(Hz|kHz|MHz|GHz)$",
        std::regex::icase);
    if (!std::regex_match(text, match, pattern))
    {
        error = "frequency must use a positive value with a unit, for example "
                "100MHz";
        return false;
    }

    double value = 0.0;
    try
    {
        value = std::stod(match[1].str());
    }
    catch (const std::exception &)
    {
        error = "frequency value is not a valid number";
        return false;
    }
    std::string unit = match[2].str();
    std::transform(unit.begin(), unit.end(), unit.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    if (unit == "hz")
        frequencyMHz = value / 1000000.0;
    else if (unit == "khz")
        frequencyMHz = value / 1000.0;
    else if (unit == "mhz")
        frequencyMHz = value;
    else
        frequencyMHz = value * 1000.0;

    if (!std::isfinite(frequencyMHz) || frequencyMHz <= 0.0)
    {
        error = "frequency must be greater than zero";
        return false;
    }
    clockPeriodNs = 1000.0 / frequencyMHz;
    return true;
}

std::string conciseNumber(double value)
{
    std::ostringstream output;
    output << std::setprecision(12) << value;
    return output.str();
}

bool readBoardPart(const std::string &boardLibraryPath, std::string &part,
                   std::string &error)
{
    const std::filesystem::path metadataPath =
        std::filesystem::path(boardLibraryPath) / "board.json";
    std::ifstream input(metadataPath);
    if (!input)
    {
        error = "cannot open board metadata '" + metadataPath.string() + "'";
        return false;
    }
    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    std::smatch match;
    if (!std::regex_search(content, match,
                           std::regex("\\\"part\\\"\\s*:\\s*\\\"([^\\\"]+)\\\"")))
    {
        error = "board metadata does not define a non-empty part";
        return false;
    }
    part = match[1].str();
    return true;
}

bool resolveFullUnrollFactors(
    std::map<std::string, int> &loopLabel2UnrollFactor,
    const std::map<std::string, std::string> &irLoop2LoopLabel,
    const std::map<std::string, int> &irLoop2OriginTripCount)
{
    for (auto &directive : loopLabel2UnrollFactor)
    {
        if (directive.second != 0)
            continue;

        int tripCount = 0;
        for (const auto &loopLabel : irLoop2LoopLabel)
        {
            if (loopLabel.second != directive.first)
                continue;

            auto tripCountEntry = irLoop2OriginTripCount.find(loopLabel.first);
            if (tripCountEntry != irLoop2OriginTripCount.end())
                tripCount = tripCountEntry->second;
            break;
        }

        if (tripCount <= 0 || tripCount == std::numeric_limits<int>::max())
        {
            errs() << "Cannot fully unroll " << directive.first
                   << ": its trip count is not a positive compile-time constant.\n";
            return false;
        }

        directive.second = tripCount;
        print_info("Resolved full unroll for " + directive.first +
                   " to factor=" + std::to_string(tripCount));
    }
    return true;
}

std::string getIRLoopName(const Loop *loop)
{
    return loop->getHeader()->getParent()->getName().str() + "-" +
           loop->getHeader()->getName().str();
}

bool configurePipelineDescendantUnrolls(
    Loop *loop, const std::string &pipelinedAncestor,
    const std::map<std::string, int> &loopLabel2II,
    std::map<std::string, int> &loopLabel2UnrollFactor,
    const std::map<std::string, std::string> &irLoop2LoopLabel,
    const std::map<std::string, int> &irLoop2OriginTripCount,
    std::map<std::string, std::string> &pipelineRequiredFullUnrolls,
    std::set<std::string> &foundPipelineLabels)
{
    const std::string irLoopName = getIRLoopName(loop);
    auto loopLabelEntry = irLoop2LoopLabel.find(irLoopName);
    if (loopLabelEntry == irLoop2LoopLabel.end())
    {
        if (!pipelinedAncestor.empty())
        {
            errs() << "Cannot pipeline " << pipelinedAncestor
                   << ": nested IR loop '" << irLoopName
                   << "' has no source loop label and cannot be fully unrolled.\n";
            return false;
        }

        for (Loop *subLoop : loop->getSubLoops())
        {
            if (!configurePipelineDescendantUnrolls(
                    subLoop, "", loopLabel2II, loopLabel2UnrollFactor,
                    irLoop2LoopLabel, irLoop2OriginTripCount,
                    pipelineRequiredFullUnrolls, foundPipelineLabels))
            {
                return false;
            }
        }
        return true;
    }

    const std::string &loopLabel = loopLabelEntry->second;
    std::string pipelineOwner = pipelinedAncestor;
    if (!pipelineOwner.empty() &&
        loopLabel2II.find(loopLabel) != loopLabel2II.end())
    {
        errs() << "Light-HLS error [BACKEND_UNSUPPORTED_NESTED_PIPELINES]: "
                  "Nested PIPELINE directives are unsupported: outer loop "
               << pipelineOwner << " and inner loop " << loopLabel
               << " both request pipelining. Apply PIPELINE to the innermost "
                  "loop, or apply it only to the outer loop so its descendants "
                  "can be fully unrolled.\n";
        return false;
    }
    if (!pipelineOwner.empty())
    {
        auto tripCountEntry = irLoop2OriginTripCount.find(irLoopName);
        const int tripCount =
            tripCountEntry == irLoop2OriginTripCount.end() ? 0
                                                          : tripCountEntry->second;
        if (tripCount <= 0 || tripCount == std::numeric_limits<int>::max())
        {
            errs() << "Cannot pipeline " << pipelineOwner << ": inner loop "
                   << loopLabel
                   << " does not have a positive compile-time constant trip count "
                      "required for full unrolling.\n";
            return false;
        }

        auto explicitUnroll = loopLabel2UnrollFactor.find(loopLabel);
        if (explicitUnroll != loopLabel2UnrollFactor.end() &&
            explicitUnroll->second != tripCount)
        {
            print_warning("PIPELINE on " + pipelineOwner +
                          " overrides the partial UNROLL factor on " + loopLabel +
                          "; using full-unroll factor=" +
                          std::to_string(tripCount) + ".");
        }
        loopLabel2UnrollFactor[loopLabel] = tripCount;
        pipelineRequiredFullUnrolls[loopLabel] = pipelineOwner;
        print_info("Vitis-style PIPELINE on " + pipelineOwner +
                   " fully unrolls inner loop " + loopLabel +
                   " with factor=" + std::to_string(tripCount));
    }

    if (loopLabel2II.find(loopLabel) != loopLabel2II.end())
    {
        foundPipelineLabels.insert(loopLabel);
        if (pipelineOwner.empty())
            pipelineOwner = loopLabel;
    }

    for (Loop *subLoop : loop->getSubLoops())
    {
        if (!configurePipelineDescendantUnrolls(
                subLoop, pipelineOwner, loopLabel2II, loopLabel2UnrollFactor,
                irLoop2LoopLabel, irLoop2OriginTripCount,
                pipelineRequiredFullUnrolls, foundPipelineLabels))
        {
            return false;
        }
    }
    return true;
}

bool applyVitisPipelineSemantics(
    llvm::Module &module, const std::map<std::string, int> &loopLabel2II,
    std::map<std::string, int> &loopLabel2UnrollFactor,
    const std::map<std::string, std::string> &irLoop2LoopLabel,
    const std::map<std::string, int> &irLoop2OriginTripCount,
    std::map<std::string, std::string> &pipelineRequiredFullUnrolls)
{
    std::set<std::string> foundPipelineLabels;
    for (llvm::Function &function : module)
    {
        if (function.isDeclaration())
            continue;

        DominatorTree dominatorTree(function);
        LoopInfo loopInfo(dominatorTree);
        for (Loop *loop : loopInfo)
        {
            if (!configurePipelineDescendantUnrolls(
                    loop, "", loopLabel2II, loopLabel2UnrollFactor,
                    irLoop2LoopLabel, irLoop2OriginTripCount,
                    pipelineRequiredFullUnrolls, foundPipelineLabels))
            {
                return false;
            }
        }
    }

    for (const auto &pipelineDirective : loopLabel2II)
    {
        if (foundPipelineLabels.find(pipelineDirective.first) ==
            foundPipelineLabels.end())
        {
            errs() << "Cannot apply PIPELINE: loop " << pipelineDirective.first
                   << " was not found after LLVM preprocessing.\n";
            return false;
        }
    }
    return true;
}

bool validatePipelineDescendantUnrolls(
    const std::map<std::string, std::string> &pipelineRequiredFullUnrolls,
    const std::map<std::string, std::string> &remainingIRLoop2LoopLabel)
{
    bool valid = true;
    for (const auto &irLoopAndLabel : remainingIRLoop2LoopLabel)
    {
        auto requirement =
            pipelineRequiredFullUnrolls.find(irLoopAndLabel.second);
        if (requirement == pipelineRequiredFullUnrolls.end())
            continue;

        errs() << "Cannot pipeline " << requirement->second << ": required inner loop "
               << requirement->first << " was not fully unrolled.\n";
        valid = false;
    }
    return valid;
}

} // namespace

int main(int argc, const char **argv)
{
    // Preserve a stack trace in the run log if a legacy analysis aborts.
    llvm::sys::PrintStackTraceOnErrorSignal(argv[0]);
    if (argc < 3)
    {
        errs() << "Usage: " << argv[0]
               << " <C/C++ file> <Top_Function_Name> [Config_File_Path] "
                  "--freq <frequency> [--board <board>] "
                  "[--synthesis <enable|disable>] [--output <directory>] "
                  "[--synthesis-part <part>]\n";
        return 1;
    }
    std::error_code EC;

    std::string top_str = std::string(argv[2]);
    std::string baseConfigFile;
    std::string requestedBoard;
    std::string outputDirectoryArgument;
    std::optional<double> requestedFrequencyMHz;
    std::optional<double> requestedClockPeriodNs;
    bool runSynthesis = false;
    bool synthesisSettingSpecified = false;
    std::string synthesisExecutable = "vivado";
    bool synthesisExecutableSpecified = false;
    std::string synthesisPart;
    const bool debugFlag = true;
    for (int argumentIndex = 3; argumentIndex < argc; ++argumentIndex)
    {
        const std::string argument = argv[argumentIndex];
        if (argument == "DEBUG")
        {
            print_warning(
                "DEBUG is no longer required; intermediate artifacts are "
                "always emitted under reports/.lighthls.");
        }
        else if (argument == "--board")
        {
            if (++argumentIndex == argc || requestedBoard.size())
            {
                errs() << "--board requires exactly one board name.\n";
                return 1;
            }
            requestedBoard = argv[argumentIndex];
        }
        else if (argument.rfind("--board=", 0) == 0)
        {
            if (requestedBoard.size() ||
                argument.size() == std::string("--board=").size())
            {
                errs() << "--board requires exactly one board name.\n";
                return 1;
            }
            requestedBoard = argument.substr(std::string("--board=").size());
        }
        else if (argument == "--freq")
        {
            if (++argumentIndex == argc || requestedFrequencyMHz.has_value())
            {
                errs() << "--freq requires exactly one frequency.\n";
                return 1;
            }
            double frequencyMHz = 0.0;
            double clockPeriodNs = 0.0;
            std::string frequencyError;
            if (!parseTargetFrequency(argv[argumentIndex], frequencyMHz,
                                      clockPeriodNs, frequencyError))
            {
                errs() << "Invalid --freq value: " << frequencyError << ".\n";
                return 1;
            }
            requestedFrequencyMHz = frequencyMHz;
            requestedClockPeriodNs = clockPeriodNs;
        }
        else if (argument.rfind("--freq=", 0) == 0)
        {
            if (requestedFrequencyMHz.has_value() ||
                argument.size() == std::string("--freq=").size())
            {
                errs() << "--freq requires exactly one frequency.\n";
                return 1;
            }
            double frequencyMHz = 0.0;
            double clockPeriodNs = 0.0;
            std::string frequencyError;
            if (!parseTargetFrequency(
                    argument.substr(std::string("--freq=").size()),
                    frequencyMHz, clockPeriodNs, frequencyError))
            {
                errs() << "Invalid --freq value: " << frequencyError << ".\n";
                return 1;
            }
            requestedFrequencyMHz = frequencyMHz;
            requestedClockPeriodNs = clockPeriodNs;
        }
        else if (argument == "--fre" || argument.rfind("--fre=", 0) == 0)
        {
            errs() << "--fre is not a valid option; use --freq.\n";
            return 1;
        }
        else if (argument == "--output" || argument == "--backend-output")
        {
            const bool deprecatedOption = argument == "--backend-output";
            if (++argumentIndex == argc || !outputDirectoryArgument.empty())
            {
                errs() << "--output requires exactly one directory.\n";
                return 1;
            }
            outputDirectoryArgument = argv[argumentIndex];
            if (deprecatedOption)
                print_warning("--backend-output is deprecated; use --output.");
        }
        else if (argument.rfind("--output=", 0) == 0 ||
                 argument.rfind("--backend-output=", 0) == 0)
        {
            const bool deprecatedOption =
                argument.rfind("--backend-output=", 0) == 0;
            const std::string prefix =
                deprecatedOption ? "--backend-output=" : "--output=";
            if (!outputDirectoryArgument.empty() ||
                argument.size() == prefix.size())
            {
                errs() << "--output requires exactly one directory.\n";
                return 1;
            }
            outputDirectoryArgument = argument.substr(prefix.size());
            if (deprecatedOption)
                print_warning("--backend-output is deprecated; use --output.");
        }
        else if (argument == "--synthesis" ||
                 argument.rfind("--synthesis=", 0) == 0)
        {
            if (synthesisSettingSpecified)
            {
                errs() << "--synthesis may be specified once.\n";
                return 1;
            }
            std::string setting;
            if (argument == "--synthesis")
            {
                if (++argumentIndex == argc)
                {
                    errs() << "--synthesis requires enable or disable.\n";
                    return 1;
                }
                setting = argv[argumentIndex];
            }
            else
            {
                setting = argument.substr(std::string("--synthesis=").size());
            }
            if (setting == "enable")
                runSynthesis = true;
            else if (setting == "disable")
                runSynthesis = false;
            else
            {
                errs() << "--synthesis requires enable or disable.\n";
                return 1;
            }
            synthesisSettingSpecified = true;
        }
        else if (argument == "--synthesize")
        {
            if (synthesisSettingSpecified)
            {
                errs() << "--synthesis may be specified once.\n";
                return 1;
            }
            runSynthesis = true;
            synthesisSettingSpecified = true;
            print_warning("--synthesize is deprecated; use --synthesis enable.");
        }
        else if (argument == "--synthesis-part")
        {
            if (++argumentIndex == argc || !synthesisPart.empty())
            {
                errs() << "--synthesis-part requires exactly one FPGA part.\n";
                return 1;
            }
            synthesisPart = argv[argumentIndex];
        }
        else if (argument.rfind("--synthesis-part=", 0) == 0)
        {
            if (!synthesisPart.empty() ||
                argument.size() == std::string("--synthesis-part=").size())
            {
                errs() << "--synthesis-part requires exactly one FPGA part.\n";
                return 1;
            }
            synthesisPart =
                argument.substr(std::string("--synthesis-part=").size());
        }
        else if (argument == "--synthesis-executable")
        {
            if (++argumentIndex == argc || synthesisExecutableSpecified)
            {
                errs() << "--synthesis-executable may be specified once.\n";
                return 1;
            }
            synthesisExecutable = argv[argumentIndex];
            synthesisExecutableSpecified = true;
        }
        else if (argument.rfind("--synthesis-executable=", 0) == 0)
        {
            if (synthesisExecutableSpecified ||
                argument.size() ==
                    std::string("--synthesis-executable=").size())
            {
                errs() << "--synthesis-executable may be specified once.\n";
                return 1;
            }
            synthesisExecutable = argument.substr(
                std::string("--synthesis-executable=").size());
            synthesisExecutableSpecified = true;
        }
        else if (argument.rfind("--", 0) == 0)
        {
            errs() << "Unknown option: " << argument << "\n";
            return 1;
        }
        else if (baseConfigFile.empty())
        {
            baseConfigFile = argument;
        }
        else
        {
            errs() << "Only one config file may be provided.\n";
            return 1;
        }
    }
    if (!requestedFrequencyMHz.has_value())
    {
        errs() << "Missing required option: --freq <frequency>.\n";
        return 1;
    }
    namespace fs = std::filesystem;
    const fs::path invocationDirectory = fs::current_path();
    const fs::path source_path = fs::absolute(argv[1]);
    if (!baseConfigFile.empty())
        baseConfigFile = fs::absolute(baseConfigFile).string();

    const fs::path outputRoot = outputDirectoryArgument.empty()
                                    ? invocationDirectory
                                    : fs::absolute(outputDirectoryArgument);
    const fs::path reportsDirectory = outputRoot / "reports";
    const fs::path intermediateDirectory = reportsDirectory / ".lighthls";
    std::error_code directoryError;
    fs::create_directories(intermediateDirectory, directoryError);
    if (directoryError)
    {
        errs() << "Cannot create Light-HLS intermediate directory '"
               << intermediateDirectory.string()
               << "': " << directoryError.message() << "\n";
        return 1;
    }
    fs::current_path(intermediateDirectory, directoryError);
    if (directoryError)
    {
        errs() << "Cannot enter Light-HLS intermediate directory '"
               << intermediateDirectory.string()
               << "': " << directoryError.message() << "\n";
        return 1;
    }
    print_info("Reports directory: " + reportsDirectory.string());
    print_info("Intermediate directory: " + intermediateDirectory.string());
    print_info("Target frequency: " + conciseNumber(*requestedFrequencyMHz) +
               " MHz (" + conciseNumber(*requestedClockPeriodNs) + " ns)");

    /////////////////////////////////////////////////////////////////////////////////////
    // Clang part for Front-End Processing                                             //
    //    extract the information of arrays in the source code                         //
    //    set labels for each loop in the source code                                  //
    /////////////////////////////////////////////////////////////////////////////////////

    // create a new Clang Tool instance (a LibTooling environment)
    std::map<std::string, int> FuncParamLine2OutermostSize;
    std::map<std::string, std::vector<unsigned long long>>
        FuncParamLine2Dimensions;
    std::map<std::string, int> LoopLabel2UnrollFactor;
    std::map<std::string, int> LoopLabel2II;

    all_sub_function_inline = 0;
    dataflowApplied = false;
    if (!baseConfigFile.empty())
        Parse_Config(baseConfigFile.c_str(), LoopLabel2UnrollFactor, LoopLabel2II);
    if (!clangPreProcess(source_path.string(), top_str,
                         FuncParamLine2OutermostSize,
                         FuncParamLine2Dimensions))
    {
        return 1;
    }

    const std::string configFile_str =
        (intermediateDirectory / "effective_config.cfg").string();

    unsigned sourcePragmaCount = 0;
    std::string selectedBoard;
    std::string selectedLibraryPath;
    std::string pragmaError;
    if (!BuildEffectiveConfigFromSourcePragmas(
            "tmp_loopLabeled.cc", source_path.string(), top_str, baseConfigFile,
            requestedBoard,
            requestedClockPeriodNs.has_value()
                ? conciseNumber(*requestedClockPeriodNs)
                : "",
            configFile_str, sourcePragmaCount, selectedBoard,
            selectedLibraryPath, pragmaError))
    {
        errs() << "Failed to load source pragmas: " << pragmaError << "\n";
        return 1;
    }
    print_info("Loaded " + std::to_string(sourcePragmaCount) +
               " supported Vitis HLS pragma(s) from source.");
    print_info("Using FPGA board library '" + selectedBoard + "' from " +
               selectedLibraryPath);
    if (runSynthesis && synthesisPart.empty())
    {
        std::string boardPartError;
        if (!readBoardPart(selectedLibraryPath, synthesisPart, boardPartError))
        {
            errs() << "Failed to select the synthesis part for board '"
                   << selectedBoard << "': " << boardPartError << "\n";
            return 1;
        }
    }
    LoopLabel2UnrollFactor.clear();
    LoopLabel2II.clear();
    Parse_Config(configFile_str.c_str(), LoopLabel2UnrollFactor, LoopLabel2II);
    std::map<std::string, unsigned long long> configuredMemoryDepths;
    std::string memoryDepthError;
    if (!ParseMemoryDepthConfig(configFile_str, top_str, configuredMemoryDepths,
                                memoryDepthError))
    {
        errs() << "Failed to load memory depth configuration: "
               << memoryDepthError << "\n";
        return 1;
    }
    std::map<std::string, MemoryInterfaceConfig> configuredMemoryInterfaces;
    std::string memoryInterfaceError;
    if (!ParseMemoryInterfaceConfig(configFile_str, top_str,
                                    configuredMemoryInterfaces,
                                    memoryInterfaceError))
    {
        errs() << "Failed to load memory interface configuration: "
               << memoryInterfaceError << "\n";
        return 1;
    }
    std::map<std::string, StorageBindingConfig> configuredStorageBindings;
    std::string storageBindingError;
    if (!ParseStorageBindingConfig(configFile_str, top_str,
                                   configuredStorageBindings,
                                   storageBindingError))
    {
        errs() << "Failed to load storage binding configuration: "
               << storageBindingError << "\n";
        return 1;
    }

    // Compile the source code into IR and Parse the input LLVM IR file into a module
    const std::string sourceInclude =
        " -I\"" + source_path.parent_path().string() + "\""
        " -I\"" LIGHT_HLS_INCLUDE_DIR "\" ";
    std::string cmd_str =
        "clang++ -O1 " +
        std::string(dataflowApplied ? "-fno-inline-functions " : "") +
        "-emit-llvm -S -g" + sourceInclude +
        "tmp_loopLabeled.cc -o top_notinline.bc 2>&1";
    print_cmd(cmd_str.c_str());
    bool result = sysexec(cmd_str.c_str());
    if (!result)
    {
        errs() << "Failed to compile rewritten source to LLVM IR.\n";
        return 1;
    }
    cmd_str = "opt -passes=always-inline top_notinline.bc -o top.bc 2>&1";
    print_cmd(cmd_str.c_str());
    result = sysexec(cmd_str.c_str());
    if (!result)
    {
        errs() << "Failed to inline rewritten source module.\n";
        return 1;
    }

    // system(cmd_str.c_str());

    SMDiagnostic Err;
    LLVMContext Context;
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
    std::unique_ptr<llvm::Module> Mod(parseIRFile("top.bc", Err, Context));
    if (!Mod)
    {
        Err.print(argv[0], errs());
        return 1;
    }

    std::map<std::string, Info_type_list> BiOp_Info_name2list_map;
    DES_Load_Instruction_Info(configFile_str.c_str(), BiOp_Info_name2list_map);

    for (auto pair : BiOp_Info_name2list_map)
    {
        llvm::errs() << "instruction type: [" << pair.first << "] loaded\n";
    }

    // you can set different configuration for different iteration
    for (int i = 0; i < 1; i++)
    {
        std::string cntStr = std::to_string(i);
        std::unique_ptr<llvm::Module> Mod_tmp = CloneModule(*Mod);

        // Create a pass manager and fill it with the passes we want to run.
        legacy::PassManager PM_pre, PM0, PM1, PM2, PM3, PM4, PM_eval;

        LLVMTargetRef T;
        ModulePassManager MPM;

        char *Error;

        if (LLVMGetTargetFromTriple((Mod_tmp->getTargetTriple()).c_str(), &T, &Error))
        {
            print_error(Error);
        }
        else
        {
            std::string targetname = LLVMGetTargetName(T);
            targetname = "The target machine is: " + targetname;
            print_info(targetname.c_str());
        }

        std::map<std::string, std::string> IRLoop2LoopLabel;
        std::map<std::string, int> LoopLabel2UnrollFactor;
        std::map<std::string, int> LoopLabel2II;
        std::map<std::string, int> IRLoop2OriginTripCount;
        std::map<std::string, int> IRLoop2OriginTripCount_eval;

        Parse_Config(configFile_str.c_str(), LoopLabel2UnrollFactor, LoopLabel2II);

        /////////////////////////////////////////////////////////////////////////////////////
        // Front-end Passes running for optimizations, including                           //
        // Loop Extraction                                                                 //
        // Loop Simplification                                                             //
        // Loop Stregnth Reducation                                                        //
        // Duplicated Instruction Removal                                                  //
        // Function Initiation                                                             //
        // Instruction Optimization: Multiplication / Instruction Hoisting /               //
        //                          Redundant Access Removal / Bitwidth Reduction/ etc..   //
        /////////////////////////////////////////////////////////////////////////////////////

        print_info("Enable LoopSimplify Pass");
        auto loopsimplifypass_pre = createLoopSimplifyPass();
        PM_pre.add(loopsimplifypass_pre);

        // auto indvarsimplifypass_pre = createIndVarSimplifyPass();
        // PM_pre.add(indvarsimplifypass_pre);
        // print_info("Enable IndVarSimplifyPass Pass");

        auto CFGSimplification_pass22_pre = createCFGSimplificationPass();
        PM_pre.add(CFGSimplification_pass22_pre);
        print_info("Enable CFGSimplificationPass Pass");

        // Canonical DATAFLOW regions already contain top-level task calls.
        // Extracting every loop in the module also extracts loops inside task
        // functions, which destroys their source labels before PIPELINE and
        // UNROLL directives are applied.
        if (all_sub_function_inline)
        {
            auto loopextract = createLoopExtractorPass(); //"HI_LoopUnroll"
            PM_pre.add(loopextract);
            print_info("Enable LoopExtractor Pass");

            // auto indvarsimplifypass_pre_loopextract = createIndVarSimplifyPass();
            // PM_pre.add(indvarsimplifypass_pre_loopextract);
            // print_info("Enable IndVarSimplifyPass Pass");

            auto CFGSimplification_pass22_pre_loopextract = createCFGSimplificationPass();
            PM_pre.add(CFGSimplification_pass22_pre_loopextract);
            print_info("Enable CFGSimplificationPass Pass");
        }

        std::map<std::string, std::vector<int>> IRFunc2BeginLine;
        auto hi_ir2sourcecode = new HI_IR2SourceCode("HI_IR2SourceCode", IRLoop2LoopLabel, IRFunc2BeginLine,
                                                     IRLoop2OriginTripCount, debugFlag);
        PM_pre.add(hi_ir2sourcecode);
        print_info("Enable HI_IR2SourceCode Pass");

        auto hi_PragmaTargetExtraction = new HI_PragmaTargetExtraction(
            top_str.c_str(), IRLoop2LoopLabel, FuncParamLine2OutermostSize, IRFunc2BeginLine, debugFlag);
        PM_pre.add(hi_PragmaTargetExtraction);
        print_info("Enable HI_PragmaTargetExtraction Pass");

        std::map<llvm::Value *, ArrayInfo *> Target2BasicArrayInfo;
        auto hi_arrayinfo =
            new HI_ArrayInfo("BasicArrayInfo", Target2BasicArrayInfo, debugFlag);
        PM_pre.add(hi_arrayinfo);
        print_info("Enable HI_ArrayInfo Pass");

        print_status("Start LLVM pre-processing");
        PM_pre.run(*Mod_tmp);
        print_status("Accomplished LLVM pre-processing");

        if (!resolveFullUnrollFactors(LoopLabel2UnrollFactor, IRLoop2LoopLabel,
                                      IRLoop2OriginTripCount))
        {
            return 1;
        }

        std::map<std::string, std::string> pipelineRequiredFullUnrolls;
        if (!applyVitisPipelineSemantics(
                *Mod_tmp, LoopLabel2II, LoopLabel2UnrollFactor,
                IRLoop2LoopLabel, IRLoop2OriginTripCount,
                pipelineRequiredFullUnrolls))
        {
            return 1;
        }

        if (debugFlag)
        {
            llvm::raw_fd_ostream OS3("top_output_loopextract.bc", EC, llvm::sys::fs::OF_None);
            WriteBitcodeToFile(*Mod_tmp, OS3);
            OS3.flush();
        }

        Triple ModuleTriple(Mod_tmp->getTargetTriple());
        TargetLibraryInfoImpl TLII(ModuleTriple);
        PM0.add(new TargetLibraryInfoWrapperPass(TLII));

        print_info("Enable LoopSimplify Pass");
        auto loopsimplifypass = createLoopSimplifyPass();
        PM0.add(loopsimplifypass);

        // auto indvarsimplifypass = createIndVarSimplifyPass();
        // PM0.add(indvarsimplifypass);
        // print_info("Enable IndVarSimplifyPass Pass");

        PM0.add(createTargetTransformInfoWrapperPass(TargetIRAnalysis()));
        print_info("Enable TargetIRAnalysis Pass");

        auto hi_mulorderopt = new HI_MulOrderOpt("HI_MulOrderOpt");
        PM0.add(hi_mulorderopt);
        print_info("Enable HI_MulOrderOpt Pass");

        auto CFGSimplification_pass22 = createCFGSimplificationPass();
        PM0.add(CFGSimplification_pass22);
        print_info("Enable CFGSimplificationPass Pass");

        auto hi_separateconstoffsetfromgep =
            new HI_SeparateConstOffsetFromGEP("HI_SeparateConstOffsetFromGEP", true, debugFlag);
        PM0.add(hi_separateconstoffsetfromgep);
        print_info("Enable HI_SeparateConstOffsetFromGEP Pass");

        auto hi_instructionmovebackward0 =
            new HI_InstructionMoveBackward("HI_InstructionMoveBackward0", (debugFlag));
        PM0.add(hi_instructionmovebackward0);
        print_info("Enable HI_InstructionMoveBackward Pass");

        PM0.run(*Mod_tmp);

        if (debugFlag)
        {
            llvm::raw_fd_ostream OSPM0("top_output_PM0.bc", EC, llvm::sys::fs::OF_None);
            WriteBitcodeToFile(*Mod_tmp, OSPM0);
            OSPM0.flush();
        }

        auto hi_loopunroll = new HI_LoopUnroll(IRLoop2LoopLabel, LoopLabel2UnrollFactor, 1, false); //"HI_LoopUnroll"
        PM1.add(hi_loopunroll);
        print_info("Enable HI_LoopUnroll Pass");

        auto hi_faddorderopt = new HI_FaddOrderOpt("HI_FaddOrderOpt");
        PM1.add(hi_faddorderopt);
        print_info("Enable HI_FaddOrderOpt Pass");

        auto hi_mul2shl = new HI_Mul2Shl("HI_Mul2Shl", debugFlag);
        PM1.add(hi_mul2shl);
        print_info("Enable HI_Mul2Shl Pass");

        auto loopstrengthreducepass = createLoopStrengthReducePass();
        PM1.add(loopstrengthreducepass);
        print_info("Enable LoopStrengthReducePass Pass");

        auto hi_aggressivelsr_mul = new HI_AggressiveLSR_MUL("AggressiveLSR", debugFlag);
        PM1.add(hi_aggressivelsr_mul);
        print_info("Enable HI_AggressiveLSR_MUL Pass");

        // don't remove chained operations
        auto hi_hlsduplicateinstrm = new HI_HLSDuplicateInstRm("HLSrmInsts", debugFlag);
        PM1.add(hi_hlsduplicateinstrm);
        print_info("Enable HI_HLSDuplicateInstRm Pass");

        auto CFGSimplification_pass_PM1 = createCFGSimplificationPass();
        PM1.add(CFGSimplification_pass_PM1);
        print_info("Enable CFGSimplificationPass Pass");

        auto hi_functioninstantiation = new HI_FunctionInstantiation("HI_FunctionInstantiation", top_str);
        PM1.add(hi_functioninstantiation);
        print_info("Enable HI_FunctionInstantiation Pass");

        auto hi_replaceselectaccess = new HI_ReplaceSelectAccess("HI_ReplaceSelectAccess", debugFlag);
        PM1.add(hi_replaceselectaccess);
        print_info("Enable HI_ReplaceSelectAccess Pass");

        auto lowerswitch_pass = createLowerSwitchPass();
        PM1.add(lowerswitch_pass);
        print_info("Enable LowerSwitchPass Pass");

        // auto ADCE_pass = createAggressiveDCEPass();
        // PM1.add(ADCE_pass);
        // print_info("Enable AggressiveDCEPass Pass");

        auto CFGSimplification_pass1 = createCFGSimplificationPass();
        PM1.add(CFGSimplification_pass1);
        print_info("Enable CFGSimplificationPass Pass");

        PM1.run(*Mod_tmp);

        if (debugFlag)
        {
            llvm::raw_fd_ostream OS111("top_output_PM1.bc", EC, llvm::sys::fs::OF_None);
            WriteBitcodeToFile(*Mod_tmp, OS111);
            OS111.flush();
        }

        // don't remove chained operations
        auto hi_hlsduplicateinstrm1 = new HI_HLSDuplicateInstRm("HLSrmInsts1", (debugFlag));
        PM2.add(hi_hlsduplicateinstrm1);
        print_info("Enable HI_HLSDuplicateInstRm Pass");

        auto CFGSimplification_pass2 = createCFGSimplificationPass();
        PM2.add(CFGSimplification_pass2);
        print_info("Enable CFGSimplificationPass Pass");

        auto hi_removeredundantaccessPM2 =
            new HI_RemoveRedundantAccess("HI_RemoveRedundantAccessPM2", top_str, (debugFlag));
        PM2.add(hi_removeredundantaccessPM2);
        print_info("Enable HI_RemoveRedundantAccess Pass");

        auto hi_instructionmovebackward1 =
            new HI_InstructionMoveBackward("HI_InstructionMoveBackward1", (debugFlag));
        PM2.add(hi_instructionmovebackward1);
        print_info("Enable HI_InstructionMoveBackward Pass");

        // don't remove chained operations
        auto hi_hlsduplicateinstrm2 = new HI_HLSDuplicateInstRm("HLSrmInsts2", (debugFlag));
        PM2.add(hi_hlsduplicateinstrm2);
        print_info("Enable HI_HLSDuplicateInstRm Pass");

        auto hi_removeredundantaccessPM2_2 =
            new HI_RemoveRedundantAccess("HI_RemoveRedundantAccessPM2_2", top_str, (debugFlag));
        PM2.add(hi_removeredundantaccessPM2_2);
        print_info("Enable HI_RemoveRedundantAccess Pass");

        auto HI_LoadALAPPM2 = new HI_LoadALAP("HI_LoadALAP", debugFlag);
        PM2.add(HI_LoadALAPPM2);
        print_info("Enable HI_LoadALAP Pass");

        std::string logName_varwidthreduce = "VarWidth__forCheck_" + cntStr;
        auto hi_varwidthreduce1 = new HI_VarWidthReduce(logName_varwidthreduce.c_str(), debugFlag);
        PM2.add(hi_varwidthreduce1);
        print_info("Enable HI_VarWidthReduce Pass");

        PM2.run(*Mod_tmp);

        if (debugFlag)
        {
            llvm::raw_fd_ostream OSPM2("top_output_PM2.bc", EC, llvm::sys::fs::OF_None);
            WriteBitcodeToFile(*Mod_tmp, OSPM2);
            OSPM2.flush();
        }

        /////////////////////////////////////////////////////////////
        // Front-end Passes running just before back-end analysis. //
        // map the IR loops to the loop labels in the source code  //
        // for the configurations of loops.                        //
        /////////////////////////////////////////////////////////////

        std::map<std::string, std::string> IRLoop2LoopLabel_eval;
        // std::map<std::string, std::vector<int>> IRFunc2BeginLine_eval;
        auto hi_ir2sourcecode_eval = new HI_IR2SourceCode("HI_IR2SourceCode_eval", IRLoop2LoopLabel_eval,
                                                          IRFunc2BeginLine, IRLoop2OriginTripCount_eval, debugFlag);
        PM3.add(hi_ir2sourcecode_eval);
        print_info("Enable HI_IR2SourceCode Pass");
        PM3.run(*Mod_tmp);

        if (!validatePipelineDescendantUnrolls(
                pipelineRequiredFullUnrolls, IRLoop2LoopLabel_eval))
        {
            return 1;
        }

        /////////////////////////////////////////////////////////////
        // Front-end Passes running just before back-end analysis. //
        // mainly account for inserting MUX for the accesses to    //
        // array partitions.                                       //
        /////////////////////////////////////////////////////////////

        auto loopinfowrapperpass = new LoopInfoWrapperPass();
        PM4.add(loopinfowrapperpass);
        print_info("Enable LoopInfoWrapperPass Pass");

        auto scalarevolutionwrapperpass = new ScalarEvolutionWrapperPass();
        PM4.add(scalarevolutionwrapperpass);
        print_info("Enable ScalarEvolutionWrapperPass Pass");

        // auto hi_MuxInsertionArrayPartition = new HI_MuxInsertionArrayPartition(
        //     configFile_str.c_str(), top_str.c_str(), FuncParamLine2OutermostSize, IRFunc2BeginLine, debugFlag);
        // print_info("Enable HI_MuxInsertionArrayPartition Pass");
        // PM4.add(hi_MuxInsertionArrayPartition);

        auto lowerswitch_pass_eval = createLowerSwitchPass();
        PM4.add(lowerswitch_pass_eval);
        // print_info("Enable LowerSwitchPass Pass");

        // auto ADCE_pass_eval = createAggressiveDCEPass();
        // PM4.add(ADCE_pass_eval);
        // print_info("Enable AggressiveDCEPass Pass");

        auto CFGSimplification_pass1_eval = createCFGSimplificationPass();
        PM4.add(CFGSimplification_pass1_eval);
        print_status("Start LLVM processing");
        PM4.run(*Mod_tmp);
        print_status("Accomplished LLVM processing");

        if (debugFlag)
        {
            llvm::raw_fd_ostream OSPM2("top_output_PM4.bc", EC, llvm::sys::fs::OF_None);
            WriteBitcodeToFile(*Mod_tmp, OSPM2);
            OSPM2.flush();
        }

        ////////////////////////////////////////////////////////////////
        // Backend-end Pass running for scheduling and binding        //
        // After running this Pass, the information will be stored in //
        // The public variables of the Pass pointer.                  //
        // such as function latencies, loop tripcounts and etc.       //
        // The information variables are declared in:                 //
        //         HI_WithDirectiveTimingResourceEvaluation.h         //
        ////////////////////////////////////////////////////////////////

        std::string logName_evaluation = "HI_WithDirectiveTimingResourceEvaluation__forCheck_" + cntStr;
        std::string logName_array = "ArrayLog__forCheck_" + cntStr;
        auto hi_withdirectivetimingresourceevaluation = new HI_WithDirectiveTimingResourceEvaluation(
            configFile_str.c_str(), logName_evaluation.c_str(), "BRAM_info_0", logName_array.c_str(), top_str.c_str(),
            IRLoop2LoopLabel_eval, IRLoop2OriginTripCount, LoopLabel2II, LoopLabel2UnrollFactor,
            FuncParamLine2OutermostSize, IRFunc2BeginLine, Target2BasicArrayInfo, debugFlag);
        print_info("Enable HI_WithDirectiveTimingResourceEvaluation Pass");
        PM_eval.add(hi_withdirectivetimingresourceevaluation);

        PM_eval.run(*Mod_tmp);

        if (!hi_withdirectivetimingresourceevaluation->pipelineDirectivesValid)
            return 1;
        if (!hi_withdirectivetimingresourceevaluation->arrayPartitionDirectivesValid)
            return 1;
        if (!hi_withdirectivetimingresourceevaluation->dataflowDirectivesValid)
            return 1;

        assert(hi_withdirectivetimingresourceevaluation->topFunctionFound &&
               "The specified top function is not found in the program");

        ///////////////////////////////////////////////////////////////////////
        // Independent scheduling, binding, and RTL generation backend.       //
        // The evaluator above is imported only as a pre-backend cost model.  //
        ///////////////////////////////////////////////////////////////////////

        light_hls::backend::BackendConfiguration backendConfiguration;
        backendConfiguration.design = source_path.stem().string();
        backendConfiguration.topFunction = top_str;
        backendConfiguration.sourcePath = source_path.string();
        backendConfiguration.sourceHash =
            light_hls::backend::stableFileHash(source_path.string());
        backendConfiguration.board = selectedBoard;
        backendConfiguration.clockPeriodNs =
            hi_withdirectivetimingresourceevaluation->clock_period;
        backendConfiguration.pragmas.unroll =
            hi_withdirectivetimingresourceevaluation->configInfo.loopUnrollConfigs;
        backendConfiguration.pragmas.pipeline =
            hi_withdirectivetimingresourceevaluation->configInfo.loopPipelineConfigs;
        backendConfiguration.pragmas.arrayPartitionCount =
            hi_withdirectivetimingresourceevaluation->configInfo.cyclicPartitionConfigs.size() +
            hi_withdirectivetimingresourceevaluation->configInfo.blockPartitionConfigs.size() +
            hi_withdirectivetimingresourceevaluation->configInfo.completePartitionConfigs.size();
        for (const auto &dataflowConfig :
             hi_withdirectivetimingresourceevaluation->configInfo.funcDataflowConfigs)
        {
            if (dataflowConfig.second)
                ++backendConfiguration.pragmas.dataflowCount;
        }
        backendConfiguration.pragmas.streamCount =
            hi_withdirectivetimingresourceevaluation->configInfo.streamConfigs.size();
        for (const auto &stream :
             hi_withdirectivetimingresourceevaluation->configInfo.streamConfigs)
        {
            backendConfiguration.streams.push_back(
                {stream.functionName, stream.variableName, stream.type,
                 static_cast<unsigned>(std::max(1, stream.depth)),
                 static_cast<unsigned>(std::max(0, stream.bitwidth)),
                 static_cast<unsigned>(std::max(1, stream.instances)),
                 static_cast<unsigned>(std::max(0, stream.initialTokens))});
        }
        backendConfiguration.irLoopLabels = IRLoop2LoopLabel_eval;
        backendConfiguration.irLoopTripCounts = IRLoop2OriginTripCount_eval;
        backendConfiguration.configuredMemoryDepths = configuredMemoryDepths;
        for (const auto &memoryInterface : configuredMemoryInterfaces)
        {
            backendConfiguration.configuredMemoryPorts[memoryInterface.first] =
                memoryInterface.second.ports;
            backendConfiguration
                .configuredMemoryReadLatencies[memoryInterface.first] =
                memoryInterface.second.readLatency;
        }
        for (const auto &storageBinding : configuredStorageBindings)
        {
            light_hls::backend::StorageBindingDirective directive;
            directive.scope = top_str;
            directive.memory = storageBinding.first;
            directive.type = storageBinding.second.type == "ram_2p"
                                 ? light_hls::backend::MemoryBindingType::RAM2P
                                 : light_hls::backend::MemoryBindingType::RAM1P;
            if (storageBinding.second.implementation == "bram")
            {
                directive.implementation =
                    light_hls::backend::MemoryImplementation::BlockRAM;
            }
            else if (storageBinding.second.implementation == "uram")
            {
                directive.implementation =
                    light_hls::backend::MemoryImplementation::UltraRAM;
            }
            else if (storageBinding.second.implementation == "lutram")
            {
                directive.implementation =
                    light_hls::backend::MemoryImplementation::DistributedRAM;
            }
            directive.readLatency = storageBinding.second.readLatency;
            backendConfiguration.storageBindings.push_back(directive);
        }
        backendConfiguration.runSynthesis = runSynthesis;
        backendConfiguration.synthesisTool = "vivado";
        backendConfiguration.synthesisExecutable = synthesisExecutable;
        backendConfiguration.synthesisPart = synthesisPart;
        const std::string sourceDepthPrefix = top_str + "-";
        for (const auto &sourceDimensions : FuncParamLine2Dimensions)
        {
            if (sourceDimensions.first.rfind(sourceDepthPrefix, 0) != 0)
                continue;
            const std::size_t lineSeparator =
                sourceDimensions.first.rfind('-');
            if (lineSeparator == std::string::npos ||
                lineSeparator <= sourceDepthPrefix.size())
            {
                continue;
            }
            const std::string argumentName = sourceDimensions.first.substr(
                sourceDepthPrefix.size(),
                lineSeparator - sourceDepthPrefix.size());
            unsigned long long totalDepth = 1;
            bool validDepth = true;
            for (unsigned long long dimension : sourceDimensions.second)
            {
                if (dimension == 0 ||
                    totalDepth >
                        std::numeric_limits<unsigned long long>::max() /
                            dimension)
                {
                    validDepth = false;
                    break;
                }
                totalDepth *= dimension;
            }
            if (validDepth)
            {
                backendConfiguration.frontendMemoryDepths[argumentName] =
                    totalDepth;
                backendConfiguration.frontendMemoryDimensions[argumentName] =
                    sourceDimensions.second;
            }
        }
        for (const auto &partition :
             hi_withdirectivetimingresourceevaluation->configInfo
                 .cyclicPartitionConfigs)
        {
            backendConfiguration.arrayPartitions.push_back(
                {partition.first, partition.second.first,
                 light_hls::backend::PartitionKind::Cyclic,
                 static_cast<unsigned>(partition.second.second.first),
                 static_cast<unsigned>(partition.second.second.second)});
        }
        for (const auto &partition :
             hi_withdirectivetimingresourceevaluation->configInfo
                 .blockPartitionConfigs)
        {
            backendConfiguration.arrayPartitions.push_back(
                {partition.first, partition.second.first,
                 light_hls::backend::PartitionKind::Block,
                 static_cast<unsigned>(partition.second.second.first),
                 static_cast<unsigned>(partition.second.second.second)});
        }
        for (const auto &partition :
             hi_withdirectivetimingresourceevaluation->configInfo
                 .completePartitionConfigs)
        {
            backendConfiguration.arrayPartitions.push_back(
                {partition.first, partition.second.first,
                 light_hls::backend::PartitionKind::Complete,
                 static_cast<unsigned>(partition.second.second), 0});
        }
        const bool runDataflowBackend =
            backendConfiguration.pragmas.dataflowCount != 0;
        for (const auto &functionLatency :
             hi_withdirectivetimingresourceevaluation->FunctionLatency)
        {
            if (!functionLatency.first || functionLatency.first->isDeclaration())
                continue;
            const std::string functionName =
                functionLatency.first->getName().str();
            const long long latency =
                std::max(1, functionLatency.second.latency);
            backendConfiguration.dataflowTaskLatencies[functionName] = latency;
            backendConfiguration.dataflowTaskInitiationIntervals[functionName] =
                static_cast<unsigned>(latency);
        }
        if (runDataflowBackend)
        {
            backendConfiguration.passSequence = {
                "HI_WithDirectiveTimingResourceEvaluation",
                "HI_DataflowAnalysis",
                "HI_DataflowTaskGeneration",
                "HI_DataflowPerformanceAnalysis",
                "HI_DataflowRTLGeneration",
                "HI_BackendValidation(post_rtl)",
                "HI_VerilogEmission",
                "HI_RTLSynthesis(vivado)"};
        }
        else
        {
            backendConfiguration.passSequence = {
                "HI_WithDirectiveTimingResourceEvaluation",
                "HI_BackendValidation(pre_backend)",
                "HI_MemoryObjectAnalysis",
                "HI_LoopPipelineAnalysis",
                "HI_ArrayPartitionAnalysis",
                "HI_StorageBindingAnalysis",
                "HI_BackendDependenceAnalysis",
                "HI_OperationScheduling",
                "HI_PipelineStorageAnalysis",
                "HI_ResourceBinding",
                "HI_RTLGeneration",
                "HI_BackendValidation(post_rtl)",
                "HI_VerilogEmission",
                "HI_RTLSynthesis(vivado)"};
        }

        backendConfiguration.outputDirectory = reportsDirectory.string();

        auto backendContext = light_hls::backend::createBackendContext(
            *Mod_tmp, backendConfiguration);
        if (runDataflowBackend)
        {
            backendContext->capabilities.dataflow = true;
            backendContext->capabilities.streamInterface = true;
        }
        backendContext->estimate.valid = true;
        backendContext->estimate.latencyCycles =
            hi_withdirectivetimingresourceevaluation->top_function_latency;
        backendContext->estimate.initiationInterval =
            hi_withdirectivetimingresourceevaluation->top_function_dataflow_II;
        backendContext->estimate.dsp =
            hi_withdirectivetimingresourceevaluation->topFunction_resource.DSP;
        backendContext->estimate.ff =
            hi_withdirectivetimingresourceevaluation->topFunction_resource.FF;
        backendContext->estimate.lut =
            hi_withdirectivetimingresourceevaluation->topFunction_resource.LUT;
        backendContext->estimate.bram =
            hi_withdirectivetimingresourceevaluation->topFunction_resource.BRAM;

        if (backendContext->top)
        {
            for (auto &scheduledInstruction :
                 hi_withdirectivetimingresourceevaluation->Inst_Schedule)
            {
                llvm::Instruction *instruction = scheduledInstruction.first;
                // Preserve zero-cycle operators and physical delay for chaining,
                // including instructions in separately generated DATAFLOW tasks.
                const auto timing = hi_withdirectivetimingresourceevaluation
                                        ->getInstructionLatency(instruction);
                backendContext->latencyModel[instruction] =
                    std::max(0, timing.latency);
                backendContext->delayModel[instruction] = timing.timing;
            }
        }

        legacy::PassManager PM_backend;
        if (runDataflowBackend)
        {
            PM_backend.add(new HI_DataflowAnalysis(backendContext));
            PM_backend.add(new HI_DataflowTaskGeneration(backendContext));
            PM_backend.add(
                new HI_DataflowPerformanceAnalysis(backendContext));
            PM_backend.add(new HI_DataflowRTLGeneration(backendContext));
        }
        else
        {
            PM_backend.add(new HI_BackendValidation(
                backendContext,
                light_hls::backend::ValidationStage::PreBackend));
            PM_backend.add(new HI_MemoryObjectAnalysis(backendContext));
            PM_backend.add(new HI_LoopPipelineAnalysis(backendContext));
            PM_backend.add(new HI_ArrayPartitionAnalysis(backendContext));
            PM_backend.add(new HI_StorageBindingAnalysis(backendContext));
            PM_backend.add(new HI_BackendDependenceAnalysis(backendContext));
            PM_backend.add(new HI_OperationScheduling(backendContext));
            PM_backend.add(new HI_PipelineStorageAnalysis(backendContext));
            PM_backend.add(new HI_ResourceBinding(backendContext));
            PM_backend.add(new HI_RTLGeneration(backendContext));
        }
        PM_backend.add(new HI_BackendValidation(
            backendContext, light_hls::backend::ValidationStage::PostRTL));
        PM_backend.add(new HI_VerilogEmission(backendContext));
        PM_backend.add(new HI_RTLSynthesis(backendContext));
        print_status("Start independent scheduling, binding, and RTL backend");
        PM_backend.run(*Mod_tmp);

        if (backendContext->hasErrors())
        {
            for (const auto &diagnostic : backendContext->diagnostics)
            {
                if (diagnostic.severity !=
                    light_hls::backend::DiagnosticSeverity::Error)
                    continue;
                errs() << "Light-HLS backend error [" << diagnostic.code
                       << "]: " << diagnostic.message;
                if (!diagnostic.location.empty())
                    errs() << " (" << diagnostic.location << ')';
                errs() << '\n';
            }
            return 1;
        }
        print_status("Accomplished independent scheduling, binding, and RTL backend");

        print_status("Writing LLVM IR to File");

        if (debugFlag)
        {
            llvm::raw_fd_ostream OS("top_output.bc", EC, llvm::sys::fs::OF_None);
            WriteBitcodeToFile(*Mod_tmp, OS);
            OS.flush();

            cmd_str = "llvm-dis top_output.bc 2>&1";
            print_cmd(cmd_str.c_str());
            result = sysexec(cmd_str.c_str());
            if (!result)
            {
                errs() << "Failed to disassemble the final LLVM module.\n";
                return 1;
            }
        }

        Mod_tmp.reset();
    }

    return 0;
}
