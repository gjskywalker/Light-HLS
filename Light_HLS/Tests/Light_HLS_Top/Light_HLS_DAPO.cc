#include "Light_HLS_Top.h"
#include "ConfigParse.h"
#include "HI_ScheduledLatency.h"
#include "HI_ConstantMemoryLowering.h"
#include "HI_BackendIRLegalization.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/Signals.h"
#include "llvm/Config/llvm-config.h"

#include <cmath>
#include <functional>
#include <limits>
#include <regex>
#include <stdexcept>

using namespace llvm;
using namespace light_hls::backend;
using JSON = nlohmann::json;
namespace fs = std::filesystem;

std::string HLS_lib_path;
std::string clock_period_str;
double DSP_limit;
double FF_limit;
double LUT_limit;
double BRAM_limit;
int ClockNum_limit;
bool all_sub_function_inline = false;
bool dataflowApplied = false;

namespace {

struct Options {
    bool prepare = false;
    bool estimateOnly = false;
    bool synthesis = false;
    std::string latencyModel = "heuristic";
    std::string input;
    std::string top;
    std::string manifest;
    std::string output;
    std::string board = "u250";
    std::string synthesisExecutable = "vivado";
    std::string synthesisPart;
    double period = 0;
};

void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}

JSON readJSON(const fs::path &path) {
    std::ifstream input(path);
    require(static_cast<bool>(input), "Cannot read " + path.string());
    JSON value;
    input >> value;
    return value;
}

void writeJSON(const fs::path &path, const JSON &value) {
    std::ofstream output(path);
    require(static_cast<bool>(output), "Cannot write " + path.string());
    output << value.dump(2) << '\n';
}

void writeIR(llvm::Module &module, const fs::path &path) {
    std::error_code error;
    raw_fd_ostream output(path.string(), error, sys::fs::OF_None);
    require(!error, error.message());
    WriteBitcodeToFile(module, output);
    output.flush();
}

Options parseOptions(int argc, char **argv) {
    Options options;
    int index = 1;
    if (index < argc && std::string(argv[index]) == "--prepare-source") {
        options.prepare = true;
        ++index;
    }
    require(index + 1 < argc,
            "Usage: Light_HLS_DAPO [--prepare-source] INPUT TOP --freq 100MHz "
            "[--manifest MANIFEST] [--board u250] [--output ROOT] "
            "[--estimate-only] [--latency-model heuristic|scheduled|hybrid] [--synthesis enable|disable]");
    options.input = fs::absolute(argv[index++]).string();
    options.top = argv[index++];
    std::set<std::string> seen;
    while (index < argc) {
        const std::string key = argv[index++];
        require(seen.insert(key).second, "Repeated option: " + key);
        if (key == "--estimate-only") {
            options.estimateOnly = true;
            continue;
        }
        require(index < argc, "Missing value for " + key);
        const std::string value = argv[index++];
        if (key == "--manifest")
            options.manifest = fs::absolute(value).string();
        else if (key == "--output")
            options.output = fs::absolute(value).string();
        else if (key == "--board")
            options.board = value;
        else if (key == "--latency-model") {
            require(value == "heuristic" || value == "scheduled" || value == "hybrid",
                    "--latency-model must be heuristic, scheduled, or hybrid");
            options.latencyModel = value;
        }
        else if (key == "--synthesis-executable")
            options.synthesisExecutable = value;
        else if (key == "--synthesis-part")
            options.synthesisPart = value;
        else if (key == "--synthesis") {
            require(value == "enable" || value == "disable",
                    "--synthesis must be enable or disable");
            options.synthesis = value == "enable";
        } else if (key == "--freq") {
            std::smatch match;
            require(std::regex_match(value, match,
                    std::regex("^([0-9]+(?:\\.[0-9]+)?)(Hz|kHz|MHz|GHz)$")),
                    "--freq requires a positive frequency with units");
            double mhz = std::stod(match[1].str());
            const std::string unit = match[2].str();
            if (unit == "Hz") mhz /= 1000000.0;
            if (unit == "kHz") mhz /= 1000.0;
            if (unit == "GHz") mhz *= 1000.0;
            require(std::isfinite(mhz) && mhz > 0, "Invalid frequency");
            options.period = 1000.0 / mhz;
        } else {
            throw std::runtime_error("Unknown option: " + key);
        }
    }
    require(options.period > 0, "Missing required option --freq");
    require(options.prepare || !options.manifest.empty(),
            "IR input requires --manifest from --prepare-source");
    require(!(options.prepare && options.estimateOnly),
            "--estimate-only requires optimized IR input, not --prepare-source");
    require(!options.prepare || options.latencyModel == "heuristic",
            "--latency-model requires optimized IR input");
    require(!(options.synthesis && options.estimateOnly),
            "--estimate-only cannot be combined with --synthesis enable");
    if (options.output.empty()) options.output = fs::current_path().string();
    return options;
}

std::unique_ptr<llvm::Module> loadIR(const std::string &path, LLVMContext &context) {
    SMDiagnostic error;
    auto module = parseIRFile(path, error, context);
    if (!module) error.print("Light_HLS_DAPO", errs());
    require(static_cast<bool>(module), "Cannot parse LLVM IR: " + path);
    require(!verifyModule(*module, &errs()), "Input LLVM IR verification failed");
    return module;
}

void collectSourceMaps(llvm::Module &module,
        std::map<std::string, std::string> &labels,
        std::map<std::string, std::vector<int>> &functionLines,
        std::map<std::string, int> &tripCounts, const std::string &log) {
    labels.clear();
    functionLines.clear();
    tripCounts.clear();
    legacy::PassManager analysis;
    analysis.add(new HI_IR2SourceCode(log.c_str(), labels, functionLines,
                                     tripCounts, true));
    analysis.run(module);
    // Unknown loop metadata may survive a CFG rewrite better than line ranges.
    for (Function &function : module) {
        if (function.isDeclaration()) continue;
        std::map<unsigned, std::string> debugLabels;
        for (BasicBlock &block : function)
            for (Instruction &instruction : block)
                if (const auto *label = dyn_cast<DbgLabelInst>(&instruction))
                    debugLabels[label->getLabel()->getLine()] = label->getLabel()->getName().str();
        DominatorTree tree(function);
        LoopInfo loops(tree);
        for (Loop *loop : loops.getLoopsInPreorder()) {
            const std::string loopKey = function.getName().str() + "-" +
                                        loop->getHeader()->getName().str();
            const auto debugLabel = debugLabels.find(loop->getStartLoc().getLine());
            if (debugLabel != debugLabels.end()) labels[loopKey] = debugLabel->second;
            MDNode *id = loop->getLoopID();
            if (!id) continue;
            for (unsigned index = 1; index < id->getNumOperands(); ++index) {
                auto *node = dyn_cast_or_null<MDNode>(id->getOperand(index));
                if (!node || node->getNumOperands() != 2) continue;
                auto *key = dyn_cast_or_null<MDString>(node->getOperand(0));
                auto *value = dyn_cast_or_null<MDString>(node->getOperand(1));
                if (key && value && key->getString() == "dapo.loop.label")
                    labels[loopKey] = value->getString().str();
            }
        }
    }
}

void annotateLoops(llvm::Module &module, const std::map<std::string, std::string> &labels) {
    for (Function &function : module) {
        if (function.isDeclaration()) continue;
        DominatorTree tree(function);
        LoopInfo loops(tree);
        for (Loop *loop : loops.getLoopsInPreorder()) {
            const auto entry = labels.find(function.getName().str() + "-" +
                                          loop->getHeader()->getName().str());
            if (entry == labels.end()) continue;
            SmallVector<Metadata *, 8> operands{nullptr};
            if (MDNode *id = loop->getLoopID())
                for (unsigned index = 1; index < id->getNumOperands(); ++index)
                    operands.push_back(id->getOperand(index));
            operands.push_back(MDNode::get(module.getContext(), {
                MDString::get(module.getContext(), "dapo.loop.label"),
                MDString::get(module.getContext(), entry->second)}));
            auto *id = MDNode::getDistinct(module.getContext(), operands);
            id->replaceOperandWith(0, id);
            loop->setLoopID(id);
        }
    }
}

void seedSourceLoopLabels(llvm::Module &module) {
    std::map<std::string, std::string> labels;
    for (Function &function : module) {
        if (function.isDeclaration()) continue;
        std::map<unsigned, std::string> lines;
        for (BasicBlock &block : function)
            for (Instruction &instruction : block)
                if (const auto *label = dyn_cast<DbgLabelInst>(&instruction))
                    lines[label->getLabel()->getLine()] = label->getLabel()->getName().str();
        DominatorTree tree(function);
        LoopInfo loops(tree);
        for (Loop *loop : loops.getLoopsInPreorder()) {
            const auto found = lines.find(loop->getStartLoc().getLine());
            if (found != lines.end())
                labels[function.getName().str() + "-" + loop->getHeader()->getName().str()] = found->second;
        }
    }
    annotateLoops(module, labels);
}

void addTargetAnalyses(legacy::PassManager &passes, const llvm::Module &module) {
    TargetLibraryInfoImpl library(Triple(module.getTargetTriple()));
    passes.add(new TargetLibraryInfoWrapperPass(library));
    passes.add(createTargetTransformInfoWrapperPass(TargetIRAnalysis()));
}

class AuditedUnroll : public HI_LoopUnroll {
    std::set<std::string> &applied;
public:
    AuditedUnroll(std::map<std::string, std::string> &labels,
                  std::map<std::string, int> &factors, std::set<std::string> &applied)
        : HI_LoopUnroll(labels, factors, 1, false), applied(applied) {}
    bool runOnLoop(Loop *loop, LPPassManager &manager) override {
        const std::string key = loop->getHeader()->getParent()->getName().str() + "-" +
                                loop->getHeader()->getName().str();
        const std::string label = IRLoop2LoopLabel.count(key) ? IRLoop2LoopLabel.at(key) : "";
        const bool changed = HI_LoopUnroll::runOnLoop(loop, manager);
        if (changed && !label.empty()) applied.insert(label);
        return changed;
    }
};

JSON prepare(const Options &options, const fs::path &reports,
             const fs::path &intermediate, LLVMContext &context) {
    std::map<std::string, int> outerSizes;
    std::map<std::string, std::vector<unsigned long long>> dimensions;
    require(clangPreProcess(options.input, options.top, outerSizes, dimensions),
            "Source preprocessing failed");
    const std::string config = (intermediate / "effective_config.cfg").string();
    unsigned pragmaCount = 0;
    std::string board, library, error;
    require(BuildEffectiveConfigFromSourcePragmas(
                "tmp_loopLabeled.cc", options.input, options.top, "", options.board,
                std::to_string(options.period), config, pragmaCount,
                board, library, error), error);
    require(!dataflowApplied, "DAPO pilot preparation does not support DATAFLOW");
    auto compiler = sys::findProgramByName("clang++");
    require(static_cast<bool>(compiler), "clang++ is not available");
    std::vector<std::string> arguments{
        *compiler, "-O0", "-Xclang", "-disable-O0-optnone", "-g",
        "-fno-discard-value-names", "-emit-llvm", "-c", "-std=c++17",
        "-I" + fs::path(options.input).parent_path().string(),
        "-I" LIGHT_HLS_INCLUDE_DIR, "tmp_loopLabeled.cc", "-o", "frontend.bc"};
    SmallVector<StringRef, 16> references;
    for (const auto &argument : arguments) references.push_back(argument);
    require(sys::ExecuteAndWait(*compiler, references, std::nullopt, {}, 120) == 0,
            "Source-to-IR compilation failed");
    auto module = loadIR("frontend.bc", context);
    // CFG normalization may discard dbg.label intrinsics in unreachable label blocks.
    // Bind directive identity before any transformation can remove those labels.
    seedSourceLoopLabels(*module);
    legacy::PassManager normalization;
    addTargetAnalyses(normalization, *module);
    normalization.add(createPromoteMemoryToRegisterPass());
    normalization.add(createCFGSimplificationPass());
    normalization.add(createLoopSimplifyPass());
    normalization.add(createLoopRotatePass());
    normalization.add(createLCSSAPass());
    normalization.run(*module);

    std::map<std::string, std::string> labels;
    std::map<std::string, std::vector<int>> lines;
    std::map<std::string, int> trips, unroll, pipeline;
    collectSourceMaps(*module, labels, lines, trips, "HI_DAPO_preparation_map");
    annotateLoops(*module, labels);
    Parse_Config(config.c_str(), unroll, pipeline);
    std::set<std::string> foundPipelines, requiredFullUnroll;
    for (Function &function : *module) {
        if (function.isDeclaration()) continue;
        DominatorTree tree(function);
        LoopInfo loops(tree);
        std::function<void(Loop *, bool)> visit = [&](Loop *loop, bool ancestorPipeline) {
            const std::string key = function.getName().str() + "-" +
                                    loop->getHeader()->getName().str();
            const auto found = labels.find(key);
            const std::string label = found == labels.end() ? "" : found->second;
            const bool hasPipeline = pipeline.count(label);
            require(!(hasPipeline && ancestorPipeline),
                    "Nested PIPELINE directives on the same loop chain are unsupported");
            if (hasPipeline) foundPipelines.insert(label);
            if (ancestorPipeline || (unroll.count(label) && unroll.at(label) == 0)) {
                require(!label.empty(), "Cannot identify loop required for full unrolling");
                const int trip = trips.count(key) ? trips.at(key) : 0;
                require(trip > 0 && trip != std::numeric_limits<int>::max(),
                        "Required full unroll has no fixed trip count: " + label);
                unroll[label] = trip;
                if (ancestorPipeline) requiredFullUnroll.insert(label);
            }
            for (Loop *child : loop->getSubLoops())
                visit(child, ancestorPipeline || hasPipeline);
        };
        for (Loop *loop : loops) visit(loop, false);
    }
    for (const auto &directive : pipeline)
        require(foundPipelines.count(directive.first),
                "Unmapped source PIPELINE directive: " + directive.first);
    for (const auto &directive : unroll) {
        bool found = false;
        for (const auto &label : labels) found |= label.second == directive.first;
        require(found, "Unmapped source UNROLL directive: " + directive.first);
    }
    legacy::PassManager lowering;
    addTargetAnalyses(lowering, *module);
    std::set<std::string> appliedUnroll;
    lowering.add(new AuditedUnroll(labels, unroll, appliedUnroll));
    lowering.add(createCFGSimplificationPass());
    lowering.add(createLoopSimplifyPass());
    lowering.add(createLCSSAPass());
    lowering.run(*module);
    for (const auto &directive : unroll)
        require(directive.second <= 1 || appliedUnroll.count(directive.first),
                "Requested UNROLL was not materialized: " + directive.first);
    collectSourceMaps(*module, labels, lines, trips, "HI_DAPO_prepared_map");
    for (const auto &label : labels)
        require(!requiredFullUnroll.count(label.second),
                "PIPELINE descendant was not fully unrolled: " + label.second);
    annotateLoops(*module, labels);
    require(!verifyModule(*module, &errs()), "Prepared LLVM IR verification failed");
    const fs::path prepared = intermediate / "prepared.bc";
    writeIR(*module, prepared);
    JSON manifest = {
        {"schema", "dapo_ir_manifest_v1"}, {"name", fs::path(options.input).stem().string()},
        {"source", options.input}, {"source_hash", stableFileHash(options.input)},
        {"ir", prepared.string()}, {"ir_hash", stableFileHash(prepared.string())},
        {"top", options.top}, {"config", config}, {"config_hash", stableFileHash(config)}, {"board", board},
        {"llvm_version", LLVM_VERSION_STRING},
        {"library", library}, {"clock_period_ns", options.period},
        {"source_pragma_count", pragmaCount}, {"outer_sizes", outerSizes},
        {"dimensions", dimensions}, {"fixed_unroll", unroll}, {"applied_unroll", appliedUnroll},
        {"pipeline", pipeline}, {"prepared_loop_labels", labels},
        {"prepared_loop_trip_counts", trips},
        {"preparation_passes", {"clang-O0-disable-optnone", "mem2reg", "simplifycfg",
           "loop-simplify", "loop-rotate", "lcssa", "source-pragma-unroll",
           "simplifycfg", "loop-simplify", "lcssa"}},
        {"post_search_legalization", {"direct-helper-inlining-v1", "constant-expression-materialization-v1",
                                      "constant-memory-initializers-v1", "same-base-pointer-phi-v1", "scalar-libm-no-errno-v1", "optional-fmuladd-unfused-v1",
                                      "mem2reg", "lower-switch", "loop-simplify", "lcssa"}}
    };
    writeJSON(reports / "dapo_manifest.json", manifest);
    return manifest;
}

int generate(const Options &options, JSON manifest, const fs::path &reports,
             const fs::path &intermediate, LLVMContext &context) {
    require(manifest.at("schema") == "dapo_ir_manifest_v1", "Unsupported manifest schema");
    require(manifest.at("top") == options.top, "Manifest top function mismatch");
    require(manifest.at("board") == options.board, "Manifest board mismatch");
    require(std::abs(manifest.at("clock_period_ns").get<double>() - options.period) < 1e-9,
            "Manifest clock mismatch; prepare a separate fixed-clock experiment");
    require(stableFileHash(manifest.at("source")) == manifest.at("source_hash"),
            "Source changed after preparation");
    require(stableFileHash(manifest.at("config")) == manifest.at("config_hash"),
            "Effective configuration changed after preparation");
    auto module = loadIR(options.input, context);
    const std::string inputHash = stableFileHash(options.input);
    writeIR(*module, intermediate / "input.bc");
    const unsigned helperCalls = inlineDirectBackendCalls(*module);
    const unsigned constantExpressions = materializeBackendConstantExpressions(*module);
    const unsigned constantInitializers = lowerConstantMemoryInitializers(*module);
    const unsigned pointerPhis = lowerSameBasePointerPhis(*module);
    const unsigned mathCalls = lowerFloatingMathCalls(*module);
    const unsigned optionalContractions = lowerOptionalFloatingContraction(*module);
    legacy::PassManager legalization;
    addTargetAnalyses(legalization, *module);
    legalization.add(createPromoteMemoryToRegisterPass());
    legalization.add(createLowerSwitchPass());
    legalization.add(createLoopSimplifyPass());
    legalization.add(createLCSSAPass());
    legalization.run(*module);
    require(!verifyModule(*module, &errs()), "Legalized LLVM IR verification failed");
    writeIR(*module, intermediate / "legalized.bc");
    JSON audit = {
        {"schema", "dapo_ir_audit_v1"}, {"input", options.input}, {"input_hash", inputHash},
        {"legalized_ir_hash", stableFileHash((intermediate / "legalized.bc").string())},
        {"same_base_pointer_phis_lowered", pointerPhis},
        {"helper_calls_inlined", helperCalls},
        {"constant_expressions_materialized", constantExpressions},
        {"manifest", options.manifest}, {"manifest_hash", stableFileHash(options.manifest)},
        {"config_hash", manifest.at("config_hash")}, {"llvm_version", LLVM_VERSION_STRING},
        {"post_search_legalization", {"direct-helper-inlining-v1", "constant-expression-materialization-v1",
                                      "constant-memory-initializers-v1", "same-base-pointer-phi-v1", "scalar-libm-no-errno-v1", "optional-fmuladd-unfused-v1",
                                      "mem2reg", "lower-switch", "loop-simplify", "lcssa"}},
        {"constant_initializers_lowered", constantInitializers},
        {"scalar_math_calls_lowered", mathCalls},
        {"optional_fmuladd_expanded", optionalContractions},
        {"optional_optimization_passes", JSON::array()}, {"status", "legalized"}
    };
    writeJSON(reports / "dapo_ir_audit.json", audit);

    // This machine-readable contract is shared by estimate-only and RTL runs.
    // A failed or interrupted evaluation must never look like a usable reward.
    fs::create_directories(reports / "estimate");
    JSON estimate = {
        {"schema_version", "1.0"}, {"evaluator", "HI_WithDirectiveTimingResourceEvaluation"},
        {"valid", false}, {"status", "pending"}, {"latency_cycles", nullptr},
        {"board", options.board}, {"clock_period_ns", options.period},
        {"target_frequency_mhz", 1000.0 / options.period}, {"top_function", options.top},
        {"input", options.input}, {"input_hash", inputHash},
        {"source", manifest.at("source")}, {"source_hash", manifest.at("source_hash")},
        {"config", manifest.at("config")}, {"config_hash", manifest.at("config_hash")},
        {"manifest", options.manifest}, {"manifest_hash", stableFileHash(options.manifest)},
        {"library", manifest.at("library")}, {"llvm_version", LLVM_VERSION_STRING},
        {"requested_latency_model", options.latencyModel},
        {"post_search_legalization", audit.at("post_search_legalization")},
        {"legalized_ir_hash", audit.at("legalized_ir_hash")},
        {"note", "Heuristic pre-backend estimate with fixed source directives and the selected board library; not RTL simulation or measured synthesis QoR."}
    };
    const fs::path estimatePath = reports / "estimate" / "dapo_estimate.json";
    writeJSON(estimatePath, estimate);

    // Reject unsupported pass-generated IR before legacy estimator assertions.
    BackendConfiguration preflightConfiguration;
    preflightConfiguration.topFunction = options.top;
    preflightConfiguration.design = manifest.at("name");
    preflightConfiguration.outputDirectory = reports.string();
    preflightConfiguration.sourcePath = manifest.at("source");
    preflightConfiguration.sourceHash = manifest.at("source_hash");
    preflightConfiguration.board = options.board;
    preflightConfiguration.clockPeriodNs = options.period;
    if (manifest.contains("pipeline"))
        for (const auto &entry : manifest.at("pipeline").items())
            preflightConfiguration.pragmas.pipeline.emplace_back(entry.key(), entry.value().get<int>());
    auto preflight = createBackendContext(*module, preflightConfiguration);
    legacy::PassManager validation;
    validation.add(new HI_BackendValidation(preflight, ValidationStage::PreBackend));
    validation.run(*module);
    const bool floatingPoint = preflight->requiredFeatures.floatingPoint;
    estimate["floating_point"] = {
        {"present", floatingPoint}, {"implementation", "hardfloat_release1_scalar_ce_v2"},
        {"formats", {"binary16", "binary32", "binary64"}}, {"rounding", "nearest_ties_even"},
        {"half_resource_model", "uncharacterized; legacy FP32 operator-cost surrogate, not FP16 calibration"},
        {"integer_conversion_rounding", "toward_zero"},
        {"clock_characterized", false}, {"resource_estimate_calibrated", false},
        {"note", "Cycle counts use the emitted schedule; combinational FP timing and resources require synthesis validation."}};
    if (preflight->hasErrors()) {
        std::string artifactError;
        emitBackendArtifacts(*preflight, artifactError);
        for (const auto &diagnostic : preflight->diagnostics)
            if (diagnostic.severity == light_hls::backend::DiagnosticSeverity::Error)
                errs() << "Light-HLS DAPO error [" << diagnostic.code << "]: " << diagnostic.message << '\n';
        audit["status"] = "unsupported_ir";
        writeJSON(reports / "dapo_ir_audit.json", audit);
        estimate["status"] = "unsupported_ir";
        writeJSON(estimatePath, estimate);
        return 1;
    }

    std::map<std::string, std::string> labels;
    std::map<std::string, std::vector<int>> lines;
    std::map<std::string, int> trips, unroll, pipeline;
    collectSourceMaps(*module, labels, lines, trips, "HI_DAPO_candidate_map");
    const std::string config = manifest.at("config");
    Parse_Config(config.c_str(), unroll, pipeline);
    std::map<std::string, int> outerSizes = manifest.at("outer_sizes");
    std::map<std::string, std::vector<unsigned long long>> dimensions = manifest.at("dimensions");
    std::map<Value *, ArrayInfo *> arrays;
    legacy::PassManager arrayAnalysis;
    arrayAnalysis.add(new HI_ArrayInfo("HI_DAPO_ArrayInfo", arrays, true));
    arrayAnalysis.run(*module);
    auto *evaluator = new HI_WithDirectiveTimingResourceEvaluation(
        config.c_str(), "HI_DAPO_timing", "HI_DAPO_BRAM", "HI_DAPO_arrays", options.top.c_str(),
        labels, trips, pipeline, unroll, outerSizes, lines, arrays, true);
    legacy::PassManager estimation;
    estimation.add(evaluator);
    estimation.run(*module);
    const bool directivesValid = evaluator->pipelineDirectivesValid &&
        evaluator->arrayPartitionDirectivesValid && evaluator->dataflowDirectivesValid;
    // Latency arithmetic saturates at INT_MAX, which is not a meaningful reward.
    const bool latencyValid = evaluator->topFunctionFound &&
        evaluator->top_function_latency > 0 &&
        evaluator->top_function_latency < std::numeric_limits<int>::max();
    const bool resourcesValid = evaluator->topFunctionFound &&
        evaluator->topFunction_resource.DSP >= 0 && evaluator->topFunction_resource.FF >= 0 &&
        evaluator->topFunction_resource.LUT >= 0 && evaluator->topFunction_resource.BRAM >= 0;
    const bool libraryMatches = fs::weakly_canonical(evaluator->HLS_lib_path) ==
        fs::weakly_canonical(manifest.at("library").get<std::string>());
    const bool clockMatches = std::isfinite(evaluator->clock_period) &&
        std::abs(evaluator->clock_period - options.period) <= 1e-5 * std::max(1.0, options.period);
    estimate["valid"] = evaluator->topFunctionFound && directivesValid && latencyValid &&
        resourcesValid && libraryMatches && clockMatches;
    estimate["status"] = estimate.at("valid").get<bool>() ? "estimate_completed" : "invalid_estimate";
    if (latencyValid && estimate.at("valid").get<bool>())
        estimate["latency_cycles"] = evaluator->top_function_latency;
    estimate["validation"] = {{"top_function_found", evaluator->topFunctionFound},
        {"pipeline_directives_valid", evaluator->pipelineDirectivesValid},
        {"array_partition_directives_valid", evaluator->arrayPartitionDirectivesValid},
        {"dataflow_directives_valid", evaluator->dataflowDirectivesValid},
        {"positive_unsaturated_latency", latencyValid}, {"nonnegative_resources", resourcesValid},
        {"library_matches_manifest", libraryMatches}, {"clock_matches_manifest", clockMatches}};
    estimate["library"] = evaluator->HLS_lib_path;
    estimate["evaluator_clock_period_ns"] = evaluator->clock_period;
    estimate["pragma_configuration"] = {
        {"unroll", JSON::object()}, {"pipeline", JSON::object()},
        {"fixed_unroll", manifest.at("fixed_unroll")},
        {"applied_unroll", manifest.at("applied_unroll")},
        {"cyclic_partitions", evaluator->configInfo.cyclicPartitionConfigs},
        {"block_partitions", evaluator->configInfo.blockPartitionConfigs},
        {"complete_partitions", evaluator->configInfo.completePartitionConfigs}};
    for (const auto &entry : evaluator->configInfo.loopUnrollConfigs)
        estimate["pragma_configuration"]["unroll"][entry.first] = entry.second;
    for (const auto &entry : evaluator->configInfo.loopPipelineConfigs)
        estimate["pragma_configuration"]["pipeline"][entry.first] = entry.second;
    if (evaluator->topFunctionFound) {
        estimate["estimated_resources"] = {{"dsp", evaluator->topFunction_resource.DSP},
            {"ff", evaluator->topFunction_resource.FF}, {"lut", evaluator->topFunction_resource.LUT},
            {"bram", evaluator->topFunction_resource.BRAM}};
        estimate["dataflow_ii"] = evaluator->top_function_dataflow_II;
    }
    estimate["loop_labels"] = labels;
    estimate["loop_trip_counts"] = trips;
    estimate["loop_smallest_ii"] = evaluator->LoopLabel2SmallestII;
    estimate["loop_iteration_latency"] = evaluator->LoopLabel2IterationLatency;
    writeIR(*module, intermediate / "estimated.bc");
    estimate["estimated_ir_hash"] = stableFileHash((intermediate / "estimated.bc").string());
    writeJSON(estimatePath, estimate);
    if (options.estimateOnly && options.latencyModel == "heuristic") {
        if (floatingPoint) {
            estimate["valid"] = false;
            estimate["status"] = "unsupported_floating_heuristic";
            writeJSON(estimatePath, estimate);
            require(false, "Floating-point RL estimation requires scheduled latency; heuristic rewards are not calibrated for HardFloat");
        }
        audit["status"] = estimate.at("status");
        audit["estimated_ir_hash"] = estimate.at("estimated_ir_hash");
        audit["loop_labels"] = labels;
        audit["loop_trip_counts"] = trips;
        audit["pass_sequence"] = {"constant-memory-initializers-v1", "scalar-libm-no-errno-v1", "optional-fmuladd-unfused-v1",
            "mem2reg(legalization)", "lower-switch(legalization)",
            "loop-simplify(legalization)", "lcssa(legalization)",
            "HI_BackendValidation(preflight)", "HI_IR2SourceCode", "HI_ArrayInfo",
            "HI_WithDirectiveTimingResourceEvaluation"};
        writeJSON(reports / "dapo_ir_audit.json", audit);
        require(estimate.at("valid").get<bool>(),
                "Modern estimator result is invalid; inspect dapo_estimate.json validation fields");
        outs() << "Modern estimate: " << evaluator->top_function_latency << " cycles; "
               << estimatePath.string() << '\n';
        return 0;
    }
    require(evaluator->topFunctionFound, "Requested top function was not found");
    require(evaluator->pipelineDirectivesValid && evaluator->arrayPartitionDirectivesValid &&
            evaluator->dataflowDirectivesValid, "Invalid source pragma after IR optimization");

    BackendConfiguration configuration;
    configuration.design = manifest.at("name");
    configuration.topFunction = options.top;
    configuration.sourcePath = manifest.at("source");
    configuration.sourceHash = manifest.at("source_hash");
    configuration.board = options.board;
    configuration.outputDirectory = reports.string();
    configuration.clockPeriodNs = options.period;
    configuration.pragmas.unroll = evaluator->configInfo.loopUnrollConfigs;
    configuration.pragmas.pipeline = evaluator->configInfo.loopPipelineConfigs;
    configuration.irLoopLabels = labels;
    configuration.irLoopTripCounts = trips;
    configuration.runSynthesis = options.synthesis;
    configuration.synthesisExecutable = options.synthesisExecutable;
    configuration.synthesisPart = options.synthesisPart;
    if (configuration.synthesisPart.empty())
        configuration.synthesisPart = readJSON(fs::path(manifest.at("library").get<std::string>()) / "board.json").at("part");
    std::string error;
    require(ParseMemoryDepthConfig(config, options.top, configuration.configuredMemoryDepths, error), error);
    std::map<std::string, MemoryInterfaceConfig> interfaces;
    require(ParseMemoryInterfaceConfig(config, options.top, interfaces, error), error);
    for (const auto &entry : interfaces) {
        configuration.configuredMemoryPorts[entry.first] = entry.second.ports;
        configuration.configuredMemoryReadLatencies[entry.first] = entry.second.readLatency;
    }
    std::map<std::string, StorageBindingConfig> bindings;
    require(ParseStorageBindingConfig(config, options.top, bindings, error), error);
    for (const auto &entry : bindings) {
        StorageBindingDirective directive;
        directive.scope = options.top;
        directive.memory = entry.first;
        directive.type = entry.second.type == "ram_2p" ? MemoryBindingType::RAM2P : MemoryBindingType::RAM1P;
        if (entry.second.implementation == "bram") directive.implementation = MemoryImplementation::BlockRAM;
        if (entry.second.implementation == "uram") directive.implementation = MemoryImplementation::UltraRAM;
        if (entry.second.implementation == "lutram") directive.implementation = MemoryImplementation::DistributedRAM;
        directive.readLatency = entry.second.readLatency;
        configuration.storageBindings.push_back(directive);
    }
    const std::string prefix = options.top + "-";
    for (const auto &entry : dimensions) {
        if (entry.first.rfind(prefix, 0) != 0) continue;
        const auto separator = entry.first.rfind('-');
        if (separator <= prefix.size()) continue;
        const std::string argument = entry.first.substr(prefix.size(), separator - prefix.size());
        unsigned long long depth = 1;
        for (auto dimension : entry.second) {
            require(dimension && depth <= std::numeric_limits<unsigned long long>::max() / dimension,
                    "Invalid memory dimensions for " + argument);
            depth *= dimension;
        }
        configuration.frontendMemoryDepths[argument] = depth;
        configuration.frontendMemoryDimensions[argument] = entry.second;
    }
    for (const auto &entry : evaluator->configInfo.cyclicPartitionConfigs)
        configuration.arrayPartitions.push_back({entry.first, entry.second.first, PartitionKind::Cyclic,
            static_cast<unsigned>(entry.second.second.first), static_cast<unsigned>(entry.second.second.second)});
    for (const auto &entry : evaluator->configInfo.blockPartitionConfigs)
        configuration.arrayPartitions.push_back({entry.first, entry.second.first, PartitionKind::Block,
            static_cast<unsigned>(entry.second.second.first), static_cast<unsigned>(entry.second.second.second)});
    for (const auto &entry : evaluator->configInfo.completePartitionConfigs)
        configuration.arrayPartitions.push_back({entry.first, entry.second.first, PartitionKind::Complete,
            static_cast<unsigned>(entry.second.second), 0});
    configuration.pragmas.arrayPartitionCount = configuration.arrayPartitions.size();
    configuration.passSequence = {"direct-helper-inlining-v1", "constant-expression-materialization-v1",
        "constant-memory-initializers-v1", "same-base-pointer-phi-v1", "scalar-libm-no-errno-v1", "optional-fmuladd-unfused-v1",
        "mem2reg(legalization)", "lower-switch(legalization)",
        "loop-simplify(legalization)", "lcssa(legalization)", "HI_IR2SourceCode", "HI_ArrayInfo",
        "HI_WithDirectiveTimingResourceEvaluation", "HI_PredicatedLoopLowering", "HI_BackendValidation(pre_backend)",
        "HI_MemoryObjectAnalysis", "HI_LoopPipelineAnalysis", "HI_ArrayPartitionAnalysis",
        "HI_StorageBindingAnalysis", "HI_BackendDependenceAnalysis", "HI_OperationScheduling",
        "HI_PipelineStorageAnalysis", "HI_ResourceBinding", "HI_RTLGeneration",
        "HI_BackendValidation(post_rtl)", "HI_VerilogEmission", "HI_RTLSynthesis"};
    auto backend = createBackendContext(*module, configuration);
    backend->estimate.valid = true;
    backend->estimate.latencyCycles = evaluator->top_function_latency;
    backend->estimate.initiationInterval = evaluator->top_function_dataflow_II;
    backend->estimate.dsp = evaluator->topFunction_resource.DSP;
    backend->estimate.ff = evaluator->topFunction_resource.FF;
    backend->estimate.lut = evaluator->topFunction_resource.LUT;
    backend->estimate.bram = evaluator->topFunction_resource.BRAM;
    for (const auto &entry : evaluator->Inst_Schedule) {
        const auto timing = evaluator->getInstructionLatency(entry.first);
        backend->latencyModel[entry.first] = std::max(0, timing.latency);
        backend->delayModel[entry.first] = timing.timing;
    }
    writeIR(*module, intermediate / "backend_input.bc");
    audit["backend_ir_hash"] = stableFileHash((intermediate / "backend_input.bc").string());
    audit["pass_sequence"] = configuration.passSequence;
    audit["loop_labels"] = labels;
    audit["loop_trip_counts"] = trips;
    audit["status"] = "backend_started";
    writeJSON(reports / "dapo_ir_audit.json", audit);
    legacy::PassManager passes;
    passes.add(new HI_PredicatedLoopLowering(backend));
    passes.add(new HI_BackendValidation(backend, ValidationStage::PreBackend));
    passes.add(new HI_MemoryObjectAnalysis(backend));
    passes.add(new HI_LoopPipelineAnalysis(backend));
    passes.add(new HI_ArrayPartitionAnalysis(backend));
    passes.add(new HI_StorageBindingAnalysis(backend));
    passes.add(new HI_BackendDependenceAnalysis(backend));
    passes.add(new HI_OperationScheduling(backend));
    passes.run(*module);
    writeIR(*module, intermediate / "backend_lowered.bc");
    estimate["predicated_pipeline_loops"] = backend->predicatedPipelineLoops;
    if (options.latencyModel != "heuristic") {
        require(backend->top != nullptr, "Scheduled estimator could not resolve the top function");
        auto scheduled = ScheduledLatencyAnalysis(*backend, *backend->top).run();
        auto rangeJSON = [](const ScheduledLatencyRange &range) -> JSON {
            if (!range.present) return nullptr;
            return {{"minimum", range.minimum}, {"maximum", range.maximum},
                    {"constant", range.minimum == range.maximum}};
        };
        JSON details = {{"model", "scheduled_cfg_v1"}, {"valid", scheduled.valid},
            {"reason", scheduled.reason}, {"cycles", rangeJSON(scheduled.cycles)},
            {"loops", JSON::array()}, {"blocks", JSON::array()},
            {"assumptions", "Fixed-latency backend memory protocol; no external backpressure. Bounds do not model correlations between input-dependent branches."}};
        for (const auto &loop : scheduled.loops)
            details["loops"].push_back({{"header", loop.header}, {"backedges", loop.backedges},
                {"minimum_backedges", loop.minimumBackedges}, {"constant_backedges", loop.constantBackedges},
                {"backedge_bound_source", loop.backedgeBoundSource},
                {"repeat", rangeJSON(loop.repeat)}, {"exit", rangeJSON(loop.exit)},
                {"total", rangeJSON(loop.total)}});
        for (const auto &block : backend->schedule.blocks)
            details["blocks"].push_back({{"name", block.name}, {"cycles", block.executionLatency},
                                         {"pipelined", block.pipelined}});
        estimate["scheduled_latency"] = details;
        estimate["heuristic_valid"] = estimate["valid"];
        estimate["selected_latency_model"] = scheduled.valid ? "scheduled_cfg_v1" : "heuristic_fallback";
        estimate["selected_latency_cycles"] = scheduled.valid ? JSON(scheduled.cycles.maximum) :
            (options.latencyModel == "hybrid" ? estimate["latency_cycles"] : JSON(nullptr));
        if (scheduled.valid && evaluator->topFunctionFound && directivesValid && resourcesValid &&
            libraryMatches && clockMatches && !backend->hasErrors()) {
            // A zero-latency combinational heuristic does not invalidate a
            // positive transaction latency proved by the actual control schedule.
            estimate["valid"] = true;
            estimate["status"] = "estimate_completed";
        }
        if (backend->hasErrors() || (!scheduled.valid && (options.latencyModel == "scheduled" || floatingPoint))) {
            estimate["valid"] = false;
            estimate["status"] = "invalid_scheduled_estimate";
            estimate["selected_latency_cycles"] = nullptr;
        }
        writeJSON(estimatePath, estimate);
    }
    if (backend->hasErrors()) {
        std::string artifactError;
        require(emitBackendArtifacts(*backend, artifactError), artifactError);
        for (const auto &diagnostic : backend->diagnostics)
            if (diagnostic.severity == light_hls::backend::DiagnosticSeverity::Error)
                errs() << "Light-HLS DAPO error [" << diagnostic.code << "]: " << diagnostic.message << '\n';
    }
    if (options.estimateOnly) {
        legacy::PassManager bindingPasses;
        bindingPasses.add(new HI_PipelineStorageAnalysis(backend));
        bindingPasses.add(new HI_ResourceBinding(backend));
        bindingPasses.run(*module);
        estimate["floating_operator_resources"] = JSON::parse(floatingResourceEstimateJSON(*backend));
        if (backend->hasErrors()) {
            estimate["valid"] = false;
            estimate["status"] = "resource_binding_failed";
        }
        writeJSON(estimatePath, estimate);
        audit["pass_sequence"] = JSON::array();
        for (const auto &pass : configuration.passSequence) {
            audit["pass_sequence"].push_back(pass);
            if (pass == "HI_ResourceBinding") break;
        }
        audit["status"] = estimate.at("valid").get<bool>() ? "estimate_completed" : "invalid_scheduled_estimate";
        writeJSON(reports / "dapo_ir_audit.json", audit);
        std::string artifactError;
        require(emitBackendArtifacts(*backend, artifactError), artifactError);
        require(estimate.at("valid").get<bool>(), "Scheduled estimate failed; inspect dapo_estimate.json");
        return 0;
    }
    require(options.latencyModel != "scheduled" || estimate.at("valid").get<bool>(),
            "Scheduled estimate failed; inspect dapo_estimate.json");
    legacy::PassManager remaining;
    remaining.add(new HI_PipelineStorageAnalysis(backend));
    remaining.add(new HI_ResourceBinding(backend));
    remaining.add(new HI_RTLGeneration(backend));
    remaining.add(new HI_BackendValidation(backend, ValidationStage::PostRTL));
    remaining.add(new HI_VerilogEmission(backend));
    remaining.add(new HI_RTLSynthesis(backend));
    remaining.run(*module);
    estimate["floating_operator_resources"] = JSON::parse(floatingResourceEstimateJSON(*backend));
    writeJSON(estimatePath, estimate);
    audit["status"] = backend->hasErrors() ? "backend_failed" : "rtl_generated";
    writeJSON(reports / "dapo_ir_audit.json", audit);
    for (const auto &diagnostic : backend->diagnostics)
        if (diagnostic.severity == light_hls::backend::DiagnosticSeverity::Error)
            errs() << "Light-HLS DAPO error [" << diagnostic.code << "]: " << diagnostic.message << '\n';
    return backend->hasErrors() ? 1 : 0;
}

} // namespace

int main(int argc, char **argv) {
    sys::PrintStackTraceOnErrorSignal(argv[0]);
    try {
        const auto options = parseOptions(argc, argv);
        const fs::path reports = fs::path(options.output) / "reports";
        const fs::path intermediate = reports / ".lighthls";
        fs::create_directories(intermediate);
        fs::current_path(intermediate);
        LLVMInitializeX86TargetInfo();
        LLVMInitializeX86Target();
        LLVMInitializeX86TargetMC();
        LLVMContext context;
        if (options.prepare) {
            prepare(options, reports, intermediate, context);
            outs() << "Prepared IR and manifest: " << (reports / "dapo_manifest.json").string() << '\n';
            return 0;
        }
        return generate(options, readJSON(options.manifest), reports, intermediate, context);
    } catch (const std::exception &error) {
        errs() << "Light-HLS DAPO error: " << error.what() << '\n';
        return 1;
    }
}
