#include "HI_BackendContext.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include <cmath>

using namespace llvm;

namespace light_hls { namespace backend {

std::string floatingResourceEstimateJSON(const HLSBackendContext &context)
{
    json::Object result{{"schema_version", 1}, {"complete", false},
        {"scope", "isolated_floating_operators_only"},
        {"implementation", "hardfloat_release1_scalar_ce_v2"},
        {"whole_design_calibrated", false},
        {"note", "Additive bound-FU costs; excludes memory, controller, muxes, context storage, CE overhead, and cross-operator optimization."}};
    auto finish = [&]() {
        std::string text;
        raw_string_ostream stream(text);
        stream << json::Value(std::move(result));
        return stream.str();
    };
    auto buffer = MemoryBuffer::getFile(LIGHT_HLS_FP_RESOURCE_MODEL);
    if (!buffer) { result["status"] = "model_not_installed"; return finish(); }
    auto parsed = json::parse(buffer.get()->getBuffer());
    if (!parsed) { consumeError(parsed.takeError()); result["status"] = "invalid_model"; return finish(); }
    const auto *model = parsed->getAsObject();
    if (!model || model->getString("implementation") != "hardfloat_release1_scalar_ce_v2" ||
        model->getString("status") != "completed" || model->getString("board") != context.configuration.board ||
        !model->getNumber("clock_period_ns") ||
        std::abs(*model->getNumber("clock_period_ns") - context.configuration.clockPeriodNs) > 1e-9 ||
        !model->getObject("entries"))
    { result["status"] = "model_contract_mismatch"; return finish(); }
    result["model_hash"] = stableFileHash(LIGHT_HLS_FP_RESOURCE_MODEL);
    result["model_path"] = LIGHT_HLS_FP_RESOURCE_MODEL;
    const auto *entries = model->getObject("entries");
    json::Array missing, units;
    int64_t lut = 0, ff = 0, dsp = 0;
    unsigned requested = 0, matched = 0;
    bool childrenCovered = true;
    json::Array tasks;
    for (const auto &task : context.dataflowImplementations)
    {
        auto childValue = json::parse(task.floatingResourceEstimate);
        if (!childValue) { consumeError(childValue.takeError()); childrenCovered = false; continue; }
        const auto *child = childValue->getAsObject();
        const auto *resources = child ? child->getObject("matched_operator_resources") : nullptr;
        if (!child || !resources) { childrenCovered = false; continue; }
        childrenCovered &= child->getBoolean("complete").value_or(false);
        requested += child->getInteger("requested_instances").value_or(0);
        matched += child->getInteger("matched_instances").value_or(0);
        lut += resources->getInteger("lut").value_or(0);
        ff += resources->getInteger("ff").value_or(0);
        dsp += resources->getInteger("dsp").value_or(0);
        tasks.push_back(json::Object{{"task", task.task}, {"estimate", std::move(*childValue)}});
    }
    for (const auto &instance : context.binding.operators)
    {
        const Instruction *instruction = nullptr;
        for (const auto &binding : context.binding.operations)
            if (binding.instance == instance.name) { instruction = binding.instruction; break; }
        if (!instruction || !isFloatingOperator(*instruction)) continue;
        // Pure wiring uses no arithmetic cells; routing/mux costs are excluded.
        if (isa<BitCastInst>(instruction) || instruction->getOpcode() == Instruction::FNeg) continue;
        ++requested;
        std::string key = instance.opcode + "|" + std::to_string(instance.bitWidth) + "|";
        for (unsigned i = 0; i < instance.operandWidths.size(); ++i)
            key += (i ? "," : "") + std::to_string(instance.operandWidths[i]);
        const auto *entry = entries->getObject(key);
        const auto *resources = entry ? entry->getObject("resources") : nullptr;
        if (!resources || !resources->getInteger("lut") || !resources->getInteger("ff") ||
            !resources->getInteger("dsp") || *resources->getInteger("lut") < 0 ||
            *resources->getInteger("ff") < 0 || *resources->getInteger("dsp") < 0)
        { missing.push_back(key); continue; }
        ++matched;
        lut += *resources->getInteger("lut");
        ff += *resources->getInteger("ff");
        dsp += *resources->getInteger("dsp");
        units.push_back(json::Object{{"instance", instance.name}, {"key", key}});
    }
    result["status"] = context.binding.valid ? (missing.empty() && childrenCovered ? "covered" : "partial_coverage") : "binding_unavailable";
    result["complete"] = context.binding.valid && missing.empty() && childrenCovered;
    result["tasks"] = std::move(tasks);
    result["requested_instances"] = requested;
    result["matched_instances"] = matched;
    result["missing_keys"] = std::move(missing);
    result["matched_units"] = std::move(units);
    result["matched_operator_resources"] = json::Object{{"lut", lut}, {"ff", ff}, {"dsp", dsp}};
    return finish();
}

} }
