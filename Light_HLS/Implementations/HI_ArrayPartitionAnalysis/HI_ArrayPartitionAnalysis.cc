#include "HI_ArrayPartitionAnalysis.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instructions.h"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;
using namespace light_hls::backend;

namespace
{

struct LinearExpression
{
    bool valid = false;
    long long coefficient = 0;
    long long constant = 0;
    std::string reason;
};

bool checkedValue(__int128 value, long long &result)
{
    if (value < std::numeric_limits<long long>::min() ||
        value > std::numeric_limits<long long>::max())
    {
        return false;
    }
    result = static_cast<long long>(value);
    return true;
}

LinearExpression invalidExpression(const std::string &reason)
{
    LinearExpression result;
    result.reason = reason;
    return result;
}

LinearExpression constantExpression(long long value)
{
    return {true, 0, value, "exact constant address"};
}

LinearExpression addExpressions(const LinearExpression &lhs,
                                const LinearExpression &rhs)
{
    if (!lhs.valid)
        return lhs;
    if (!rhs.valid)
        return rhs;
    LinearExpression result;
    if (!checkedValue(static_cast<__int128>(lhs.coefficient) +
                          rhs.coefficient,
                      result.coefficient) ||
        !checkedValue(static_cast<__int128>(lhs.constant) + rhs.constant,
                      result.constant))
    {
        return invalidExpression("partition address expression overflow");
    }
    result.valid = true;
    result.reason = "exact affine address";
    return result;
}

LinearExpression multiplyExpressions(const LinearExpression &lhs,
                                     const LinearExpression &rhs)
{
    if (!lhs.valid)
        return lhs;
    if (!rhs.valid)
        return rhs;
    if (lhs.coefficient != 0 && rhs.coefficient != 0)
        return invalidExpression("nonlinear partition address");

    LinearExpression result;
    if (!checkedValue(static_cast<__int128>(lhs.coefficient) * rhs.constant +
                          static_cast<__int128>(rhs.coefficient) * lhs.constant,
                      result.coefficient) ||
        !checkedValue(static_cast<__int128>(lhs.constant) * rhs.constant,
                      result.constant))
    {
        return invalidExpression("partition address multiplication overflow");
    }
    result.valid = true;
    result.reason = "exact affine address";
    return result;
}

LinearExpression evaluateSCEV(const SCEV *expression, const Loop *loop,
                              const Value *base,
                              const HLSBackendContext &context,
                              ScalarEvolution &scalarEvolution)
{
    if (const auto *constant = dyn_cast<SCEVConstant>(expression))
    {
        const APInt &value = constant->getAPInt();
        if (!value.isSignedIntN(64))
            return invalidExpression("partition address constant exceeds 64 bits");
        return constantExpression(value.getSExtValue());
    }
    if (const auto *unknown = dyn_cast<SCEVUnknown>(expression))
    {
        const Value *value = unknown->getValue();
        const auto provenance = context.addressProvenance.find(value);
        if (value == base ||
            (provenance != context.addressProvenance.end() &&
             provenance->second == base))
        {
            return constantExpression(0);
        }
        return invalidExpression("partition address contains an unknown value");
    }
    if (const auto *cast = dyn_cast<SCEVCastExpr>(expression))
    {
        return evaluateSCEV(cast->getOperand(), loop, base, context,
                            scalarEvolution);
    }
    if (const auto *addition = dyn_cast<SCEVAddExpr>(expression))
    {
        LinearExpression result = constantExpression(0);
        for (const SCEV *operand : addition->operands())
        {
            result = addExpressions(
                result, evaluateSCEV(operand, loop, base, context,
                                     scalarEvolution));
            if (!result.valid)
                return result;
        }
        return result;
    }
    if (const auto *multiplication = dyn_cast<SCEVMulExpr>(expression))
    {
        LinearExpression result = constantExpression(1);
        for (const SCEV *operand : multiplication->operands())
        {
            result = multiplyExpressions(
                result, evaluateSCEV(operand, loop, base, context,
                                     scalarEvolution));
            if (!result.valid)
                return result;
        }
        return result;
    }
    if (const auto *recurrence = dyn_cast<SCEVAddRecExpr>(expression))
    {
        if (!loop || recurrence->getLoop() != loop || !recurrence->isAffine())
            return invalidExpression("partition address recurrence is unsupported");
        LinearExpression start = evaluateSCEV(
            recurrence->getStart(), loop, base, context, scalarEvolution);
        LinearExpression step = evaluateSCEV(
            recurrence->getStepRecurrence(scalarEvolution), loop, base,
            context, scalarEvolution);
        if (!start.valid)
            return start;
        if (!step.valid)
            return step;
        if (start.coefficient != 0 || step.coefficient != 0)
            return invalidExpression("nested partition address recurrence");
        start.coefficient = step.constant;
        start.reason = "exact affine loop address";
        return start;
    }
    return invalidExpression("unsupported partition ScalarEvolution expression");
}

unsigned long long scalarElementCount(const Type *type)
{
    if (const auto *array = dyn_cast<ArrayType>(type))
    {
        const unsigned long long nested =
            scalarElementCount(array->getElementType());
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

LinearExpression analyzePointer(const Value *pointer,
                                const MemoryObject &memory, const Loop *loop,
                                const HLSBackendContext &context,
                                ScalarEvolution &scalarEvolution,
                                std::set<const Value *> &visiting)
{
    if (!pointer || !visiting.insert(pointer).second)
        return invalidExpression("cyclic partition pointer expression");

    LinearExpression result;
    if (pointer == memory.base)
    {
        result = constantExpression(0);
    }
    else if (const auto *intToPtr = dyn_cast<IntToPtrInst>(pointer))
    {
        result = evaluateSCEV(scalarEvolution.getSCEV(intToPtr->getOperand(0)),
                              loop, memory.base, context, scalarEvolution);
    }
    else if (const auto *cast = dyn_cast<BitCastInst>(pointer))
    {
        result = analyzePointer(cast->getOperand(0), memory, loop, context,
                                scalarEvolution, visiting);
    }
    else if (const auto *cast = dyn_cast<AddrSpaceCastInst>(pointer))
    {
        result = analyzePointer(cast->getOperand(0), memory, loop, context,
                                scalarEvolution, visiting);
    }
    else if (const auto *gep = dyn_cast<GetElementPtrInst>(pointer))
    {
        result = analyzePointer(gep->getPointerOperand(), memory, loop,
                                context, scalarEvolution, visiting);
        Type *indexedType = gep->getSourceElementType();
        unsigned indexOrdinal = 0;
        for (const Use &indexUse : gep->indices())
        {
            if (!result.valid)
                break;
            unsigned long long scale = 1;
            if (indexOrdinal == 0)
            {
                scale = scalarElementCount(indexedType);
            }
            else if (const auto *array = dyn_cast<ArrayType>(indexedType))
            {
                indexedType = array->getElementType();
                scale = scalarElementCount(indexedType);
            }
            if (scale == 0 ||
                scale > static_cast<unsigned long long>(
                            std::numeric_limits<long long>::max()))
            {
                result = invalidExpression("partition GEP scale exceeds 64 bits");
                break;
            }
            LinearExpression index = evaluateSCEV(
                scalarEvolution.getSCEV(indexUse.get()), loop, memory.base,
                context, scalarEvolution);
            result = addExpressions(
                result,
                multiplyExpressions(
                    index, constantExpression(static_cast<long long>(scale))));
            ++indexOrdinal;
        }
    }
    else
    {
        result = invalidExpression("unsupported partition pointer expression");
    }
    visiting.erase(pointer);
    return result;
}

MemoryObject *findMemoryObject(HLSBackendContext &context,
                               const std::string &name)
{
    for (MemoryObject &memory : context.memoryObjects)
    {
        if (memory.name == name)
            return &memory;
    }
    return nullptr;
}

const LoopPipelineInfo *findPipeline(const HLSBackendContext &context,
                                     const Instruction *instruction)
{
    if (!instruction)
        return nullptr;
    for (const LoopPipelineInfo &pipeline : context.pipelineRegions)
    {
        if (pipeline.header == instruction->getParent())
            return &pipeline;
    }
    return nullptr;
}

unsigned addressWidthForDepth(unsigned long long depth)
{
    if (depth == 0)
        return 64;
    unsigned width = 0;
    unsigned long long maximum = depth - 1;
    while (maximum)
    {
        ++width;
        maximum >>= 1;
    }
    return std::max(1U, width);
}

bool multiplyUnsigned(unsigned long long lhs, unsigned long long rhs,
                      unsigned long long &result)
{
    if (lhs != 0 && rhs > std::numeric_limits<unsigned long long>::max() / lhs)
        return false;
    result = lhs * rhs;
    return true;
}

bool resolveMemoryPartition(MemoryObject &memory,
                            const std::vector<ArrayPartitionDirective> &directives,
                            HLSBackendContext &context)
{
    if (directives.empty())
        return true;
    if (memory.dimensions.empty() || memory.depth == 0)
    {
        context.addError(
            "BACKEND_ARRAY_PARTITION_DIMENSIONS_UNKNOWN",
            "Array partitioning requires exact source or allocation dimensions for MemoryObject '" +
                memory.name + "'.",
            memory.name);
        return false;
    }

    std::map<unsigned, ArrayPartitionDirective> byDimension;
    for (const ArrayPartitionDirective &directive : directives)
    {
        std::vector<unsigned> targets;
        if (directive.dimension == 0)
        {
            for (unsigned dimension = 0; dimension < memory.dimensions.size();
                 ++dimension)
            {
                targets.push_back(dimension);
            }
        }
        else if (directive.dimension <= memory.dimensions.size())
        {
            targets.push_back(directive.dimension - 1);
        }
        else
        {
            context.addError(
                "BACKEND_ARRAY_PARTITION_DIMENSION_OUT_OF_RANGE",
                "ARRAY_PARTITION dimension " +
                    std::to_string(directive.dimension) + " is invalid for '" +
                    memory.name + "', which has " +
                    std::to_string(memory.dimensions.size()) + " dimension(s).",
                memory.name);
            return false;
        }
        for (unsigned target : targets)
        {
            if (!byDimension.emplace(target, directive).second)
            {
                context.addError(
                    "BACKEND_ARRAY_PARTITION_DIMENSION_CONFLICT",
                    "More than one ARRAY_PARTITION directive targets dimension " +
                        std::to_string(target + 1) + " of '" + memory.name + "'.",
                    memory.name);
                return false;
            }
        }
    }

    memory.partitionDimensions.clear();
    memory.banks = 1;
    memory.bankDepth = 1;
    bool allComplete = true;
    PartitionKind firstKind = PartitionKind::None;
    for (unsigned dimension = 0; dimension < memory.dimensions.size();
         ++dimension)
    {
        const unsigned long long extent = memory.dimensions[dimension];
        unsigned long long localExtent = extent;
        auto configured = byDimension.find(dimension);
        if (configured != byDimension.end())
        {
            const ArrayPartitionDirective &directive = configured->second;
            unsigned long long requestedFactor =
                directive.kind == PartitionKind::Complete
                    ? extent
                    : directive.factor;
            if (requestedFactor == 0 || extent == 0)
            {
                context.addError(
                    "BACKEND_ARRAY_PARTITION_FACTOR_INVALID",
                    "ARRAY_PARTITION requires a positive factor and dimension extent for '" +
                        memory.name + "'.",
                    memory.name);
                return false;
            }
            const unsigned factor = static_cast<unsigned>(
                std::min<unsigned long long>(requestedFactor, extent));
            localExtent = (extent + factor - 1) / factor;
            MemoryPartitionDimension partition;
            partition.dimension = dimension;
            partition.kind = directive.kind;
            partition.extent = extent;
            partition.factor = factor;
            partition.localExtent = localExtent;
            memory.partitionDimensions.push_back(partition);
            if (firstKind == PartitionKind::None)
                firstKind = directive.kind;
            allComplete &= directive.kind == PartitionKind::Complete;
            if (memory.banks > std::numeric_limits<unsigned>::max() / factor)
            {
                context.addError(
                    "BACKEND_ARRAY_PARTITION_BANK_COUNT_OVERFLOW",
                    "ARRAY_PARTITION bank count exceeds the backend limit for '" +
                        memory.name + "'.",
                    memory.name);
                return false;
            }
            memory.banks *= factor;
        }
        unsigned long long nextDepth = 0;
        if (!multiplyUnsigned(memory.bankDepth, localExtent, nextDepth))
        {
            context.addError(
                "BACKEND_ARRAY_PARTITION_BANK_DEPTH_OVERFLOW",
                "ARRAY_PARTITION bank depth overflows for '" + memory.name + "'.",
                memory.name);
            return false;
        }
        memory.bankDepth = nextDepth;
    }
    memory.partition = allComplete ? PartitionKind::Complete : firstKind;
    memory.bankAddressWidth = addressWidthForDepth(memory.bankDepth);
    memory.bankPortUsage.assign(
        memory.banks,
        std::vector<MemoryPortUsage>(memory.ports, MemoryPortUsage()));
    return true;
}

// Evaluate only a closed, bounded expression. APInt preserves the original
// widths, including truncation and signed/unsigned extension before banking.
std::optional<APInt> evaluateOrdinarySCEV(const SCEV *expression,
                                        const Loop *loop, unsigned iteration,
                                        ScalarEvolution &scalarEvolution,
                                        unsigned depth = 0)
{
    if (!expression || depth > 128)
        return std::nullopt;
    const unsigned width = scalarEvolution.getTypeSizeInBits(expression->getType());
    if (width == 0 || width > 64)
        return std::nullopt;
    if (const auto *constant = dyn_cast<SCEVConstant>(expression))
        return constant->getAPInt();
    auto evaluate = [&](const SCEV *operand) {
        return evaluateOrdinarySCEV(operand, loop, iteration,
                                    scalarEvolution, depth + 1);
    };
    if (const auto *unknown = dyn_cast<SCEVUnknown>(expression))
    {
        // Strength reduction may represent sign extension as an arithmetic
        // shift, which SCEV leaves opaque. Evaluate only pure integer forms.
        const auto *operation = dyn_cast<BinaryOperator>(unknown->getValue());
        if (!operation || !operation->getType()->isIntegerTy())
            return std::nullopt;
        auto left = evaluate(scalarEvolution.getSCEV(operation->getOperand(0)));
        auto right = evaluate(scalarEvolution.getSCEV(operation->getOperand(1)));
        if (!left || !right || left->getBitWidth() != width || right->getBitWidth() != width)
            return std::nullopt;
        switch (operation->getOpcode())
        {
        case Instruction::And: return *left & *right;
        case Instruction::Or: return *left | *right;
        case Instruction::Xor: return *left ^ *right;
        case Instruction::Shl:
        case Instruction::LShr:
        case Instruction::AShr:
            if (right->getZExtValue() >= width)
                return std::nullopt;
            if (operation->getOpcode() == Instruction::Shl)
                return left->shl(right->getZExtValue());
            if (operation->getOpcode() == Instruction::LShr)
                return left->lshr(right->getZExtValue());
            return left->ashr(right->getZExtValue());
        default: return std::nullopt;
        }
    }
    if (const auto *cast = dyn_cast<SCEVCastExpr>(expression))
    {
        auto operand = evaluate(cast->getOperand());
        if (!operand)
            return std::nullopt;
        if (isa<SCEVSignExtendExpr>(cast))
            return operand->sextOrTrunc(width);
        return operand->zextOrTrunc(width);
    }
    if (isa<SCEVAddExpr>(expression) || isa<SCEVMulExpr>(expression))
    {
        const bool addition = isa<SCEVAddExpr>(expression);
        APInt result(width, addition ? 0 : 1);
        for (const SCEV *operand : cast<SCEVNAryExpr>(expression)->operands())
        {
            auto value = evaluate(operand);
            if (!value || value->getBitWidth() != width)
                return std::nullopt;
            result = addition ? result + *value : result * *value;
        }
        return result;
    }
    if (const auto *recurrence = dyn_cast<SCEVAddRecExpr>(expression))
    {
        if (recurrence->getLoop() != loop || !recurrence->isAffine())
            return std::nullopt;
        auto start = evaluate(recurrence->getStart());
        auto step = evaluate(recurrence->getStepRecurrence(scalarEvolution));
        if (!start || !step || start->getBitWidth() != width || step->getBitWidth() != width)
            return std::nullopt;
        return *start + *step * APInt(width, iteration);
    }
    return std::nullopt;
}

// Remove exactly one additive object base. Substituting zero for an arbitrary
// occurrence of the base would be unsound across multiplication or truncation.
std::optional<APInt> evaluateOrdinaryAddressSCEV(
    const SCEV *expression, const MemoryObject &memory, const Loop *loop,
    unsigned iteration, ScalarEvolution &scalarEvolution, unsigned depth = 0)
{
    if (!expression || depth > 128)
        return std::nullopt;
    const unsigned width = scalarEvolution.getTypeSizeInBits(expression->getType());
    if (width != scalarEvolution.getTypeSizeInBits(memory.base->getType()) || width > 64)
        return std::nullopt;
    if (const auto *unknown = dyn_cast<SCEVUnknown>(expression))
    {
        const Value *value = unknown->getValue();
        if (value == memory.base)
            return APInt(width, 0);
        const auto *pointerCast = dyn_cast<PtrToIntInst>(value);
        if (pointerCast && pointerCast->getPointerOperand() == memory.base)
            return APInt(width, 0);
    }
    if (const auto *cast = dyn_cast<SCEVPtrToIntExpr>(expression))
    {
        const auto *base = dyn_cast<SCEVUnknown>(cast->getOperand());
        if (base && base->getValue() == memory.base)
            return APInt(width, 0);
    }
    auto evaluateAddress = [&](const SCEV *operand) {
        return evaluateOrdinaryAddressSCEV(operand, memory, loop, iteration,
                                           scalarEvolution, depth + 1);
    };
    if (const auto *addition = dyn_cast<SCEVAddExpr>(expression))
    {
        APInt result(width, 0);
        bool foundBase = false;
        for (const SCEV *operand : addition->operands())
        {
            auto value = evaluateOrdinarySCEV(operand, loop, iteration, scalarEvolution);
            if (!value)
            {
                if (foundBase)
                    return std::nullopt;
                value = evaluateAddress(operand);
                foundBase = true;
            }
            if (!value || value->getBitWidth() != width)
                return std::nullopt;
            result += *value;
        }
        return foundBase ? std::optional<APInt>(result) : std::nullopt;
    }
    if (const auto *recurrence = dyn_cast<SCEVAddRecExpr>(expression))
    {
        if (recurrence->getLoop() != loop || !recurrence->isAffine())
            return std::nullopt;
        auto start = evaluateAddress(recurrence->getStart());
        auto step = evaluateOrdinarySCEV(recurrence->getStepRecurrence(scalarEvolution),
                                         loop, iteration, scalarEvolution);
        if (start && step && step->getBitWidth() == width)
            return *start + *step * APInt(width, iteration);
    }
    return std::nullopt;
}

std::optional<APInt> evaluateOrdinaryPointer(const Value *pointer,
                                           const MemoryObject &memory,
                                           const Loop *loop, unsigned iteration,
                                           ScalarEvolution &scalarEvolution,
                                           unsigned depth = 0)
{
    if (!pointer || depth > 128)
        return std::nullopt;
    const unsigned width = scalarEvolution.getTypeSizeInBits(pointer->getType());
    if (width == 0 || width > 64)
        return std::nullopt;
    if (pointer == memory.base)
        return APInt(width, 0);
    if (const auto *cast = dyn_cast<IntToPtrInst>(pointer))
    {
        auto address = evaluateOrdinaryAddressSCEV(
            scalarEvolution.getSCEV(cast->getOperand(0)), memory, loop, iteration,
            scalarEvolution);
        return address ? std::optional<APInt>(address->zextOrTrunc(width)) : std::nullopt;
    }
    if (isa<BitCastInst>(pointer) || isa<AddrSpaceCastInst>(pointer))
    {
        if (scalarEvolution.getTypeSizeInBits(cast<Instruction>(pointer)->getOperand(0)->getType()) != width)
            return std::nullopt;
        auto address = evaluateOrdinaryPointer(cast<Instruction>(pointer)->getOperand(0),
            memory, loop, iteration, scalarEvolution, depth + 1);
        return address ? std::optional<APInt>(address->zextOrTrunc(width)) : std::nullopt;
    }
    const auto *gep = dyn_cast<GetElementPtrInst>(pointer);
    if (!gep)
        return std::nullopt;
    if (scalarEvolution.getDataLayout().getIndexTypeSizeInBits(pointer->getType()) != width)
        return std::nullopt;
    auto address = evaluateOrdinaryPointer(gep->getPointerOperand(), memory, loop,
                                          iteration, scalarEvolution, depth + 1);
    if (!address || address->getBitWidth() != width)
        return std::nullopt;
    Type *indexedType = gep->getSourceElementType();
    unsigned ordinal = 0;
    for (const Use &indexUse : gep->indices())
    {
        if (ordinal++ != 0)
        {
            const auto *array = dyn_cast<ArrayType>(indexedType);
            if (!array)
                return std::nullopt;
            indexedType = array->getElementType();
        }
        const unsigned long long scale = scalarElementCount(indexedType);
        auto index = evaluateOrdinarySCEV(scalarEvolution.getSCEV(indexUse.get()),
                                         loop, iteration, scalarEvolution);
        if (!scale || !index)
            return std::nullopt;
        *address += index->sextOrTrunc(width) * APInt(width, scale);
    }
    return address;
}

bool analyzeOrdinaryLoopBanks(MemoryAccessInfo &access, const MemoryObject &memory,
                              const Loop &loop, ScalarEvolution &scalarEvolution,
                              unsigned enumerationLimit, std::set<unsigned> &banks)
{
    access.bankAnalysisSource = "ordinary_loop";
    if (loop.getParentLoop() || !loop.getSubLoops().empty())
    {
        access.bankAnalysisReason = "nested ordinary loops retain conservative bank coverage";
        return false;
    }
    const unsigned iterations = scalarEvolution.getSmallConstantTripCount(&loop);
    access.bankAnalysisIterations = iterations;
    if (!iterations || iterations > enumerationLimit || !memory.depth)
    {
        access.bankAnalysisReason = "unknown/excessive trip count or unknown memory extent";
        return false;
    }
    for (unsigned iteration = 0; iteration < iterations; ++iteration)
    {
        auto address = evaluateOrdinaryPointer(access.pointerOperand, memory, &loop,
                                               iteration, scalarEvolution);
        if (!address || address->getZExtValue() >= memory.depth)
        {
            banks.clear();
            access.bankAnalysisReason = address ? "address is outside the memory extent" :
                "runtime-dependent or unsupported bounded address expression";
            return false;
        }
        banks.insert(partitionBankForAddress(memory, address->getZExtValue()));
    }
    access.bankAnalysisReason = "bit-precise address coverage over a constant loop trip count";
    return true;
}

void analyzeAccessBanks(HLSBackendContext &context, LoopInfo &loopInfo,
                        ScalarEvolution &scalarEvolution)
{
    constexpr unsigned enumerationLimit = 1000000;
    for (auto &entry : context.memoryAccesses)
    {
        MemoryAccessInfo &access = entry.second;
        MemoryObject *memory = findMemoryObject(context, access.memory);
        if (!memory)
            continue;
        access.possibleBanks.clear();
        access.iterationBanks.clear();
        access.bankMappingExact = false;
        access.bankAnalysisIterations = 0;
        if (memory->banks <= 1)
        {
            access.possibleBanks.push_back(0);
            access.bankMappingExact = true;
            access.bankAnalysisSource = "single_bank";
            access.bankAnalysisReason = "unpartitioned memory";
            continue;
        }

        const LoopPipelineInfo *pipeline = findPipeline(context, entry.first);
        const Loop *ordinaryLoop = loopInfo.getLoopFor(entry.first->getParent());
        // Preserve correlations rather than just the union of reachable banks.
        // The bounded APInt evaluator also handles XOR, masks and narrow casts.
        if (pipeline && ordinaryLoop && pipeline->tripCount &&
            pipeline->tripCount <= 4096 && memory->depth)
        {
            std::set<unsigned> banks;
            for (unsigned iteration = 0; iteration < pipeline->tripCount; ++iteration)
            {
                auto address = evaluateOrdinaryPointer(access.pointerOperand, *memory,
                    ordinaryLoop, iteration, scalarEvolution);
                if (!address || address->getZExtValue() >= memory->depth)
                {
                    access.iterationBanks.clear();
                    break;
                }
                const unsigned bank = partitionBankForAddress(*memory, address->getZExtValue());
                access.iterationBanks.push_back(bank);
                banks.insert(bank);
            }
            if (access.iterationBanks.size() == pipeline->tripCount)
            {
                access.possibleBanks.assign(banks.begin(), banks.end());
                access.bankMappingExact = true;
                access.bankAnalysisSource = "pipeline_iteration_trace";
                access.bankAnalysisIterations = pipeline->tripCount;
                access.bankAnalysisReason = "bit-precise request banks, including cross-iteration overlap";
                continue;
            }
        }
        if (!pipeline && ordinaryLoop)
        {
            std::set<unsigned> banks;
            access.bankMappingExact = analyzeOrdinaryLoopBanks(access, *memory,
                *ordinaryLoop, scalarEvolution, enumerationLimit, banks);
            if (!access.bankMappingExact)
                for (unsigned bank = 0; bank < memory->banks; ++bank)
                    banks.insert(bank);
            access.possibleBanks.assign(banks.begin(), banks.end());
            continue;
        }
        access.bankAnalysisSource = pipeline ? "pipeline_affine" : "constant_address";
        const Loop *loop = pipeline
                               ? loopInfo.getLoopFor(const_cast<BasicBlock *>(
                                     pipeline->header))
                               : nullptr;
        std::set<const Value *> visiting;
        const LinearExpression expression = analyzePointer(
            access.pointerOperand, *memory, loop, context, scalarEvolution,
            visiting);
        std::set<unsigned> banks;
        bool exact = expression.valid;
        unsigned iterations = 1;
        if (pipeline)
        {
            iterations = pipeline->tripCount;
            exact &= iterations <= enumerationLimit;
        }
        else
        {
            exact &= expression.coefficient == 0;
        }
        access.bankAnalysisIterations = iterations;
        access.bankAnalysisReason = expression.reason;
        if (expression.valid && !exact)
            access.bankAnalysisReason = "nonconstant address or excessive enumeration bound";
        if (exact)
        {
            for (unsigned iteration = 0; iteration < iterations; ++iteration)
            {
                long long address = 0;
                if (!checkedValue(
                        static_cast<__int128>(expression.coefficient) * iteration +
                            expression.constant,
                        address) ||
                    address < 0 ||
                    (memory->depth != 0 &&
                     static_cast<unsigned long long>(address) >= memory->depth))
                {
                    exact = false;
                    access.bankAnalysisReason = "address overflow or outside the memory extent";
                    banks.clear();
                    break;
                }
                banks.insert(partitionBankForAddress(
                    *memory, static_cast<unsigned long long>(address)));
            }
        }
        if (!exact)
        {
            for (unsigned bank = 0; bank < memory->banks; ++bank)
                banks.insert(bank);
        }
        access.possibleBanks.assign(banks.begin(), banks.end());
        access.bankMappingExact = exact;
    }
}

} // namespace

char HI_ArrayPartitionAnalysis::ID = 0;

HI_ArrayPartitionAnalysis::HI_ArrayPartitionAnalysis(
    HLSBackendContextPtr context)
    : ModulePass(ID), context(std::move(context))
{
}

bool HI_ArrayPartitionAnalysis::runOnModule(Module &module)
{
    if (!context || context->hasErrors() || !context->top)
        return false;

    context->module = &module;
    std::map<std::string, std::vector<ArrayPartitionDirective>> byMemory;
    for (const ArrayPartitionDirective &directive :
         context->configuration.arrayPartitions)
    {
        if (directive.scope == context->configuration.topFunction)
            byMemory[directive.memory].push_back(directive);
    }

    std::set<std::string> resolved;
    for (MemoryObject &memory : context->memoryObjects)
    {
        auto directives = byMemory.find(memory.name);
        if (directives == byMemory.end())
            continue;
        resolveMemoryPartition(memory, directives->second, *context);
        // Fully scalarized external banks are input wires, not synchronous
        // RAM responses. Preserve explicitly requested adapter latencies.
        if (memory.storage == MemoryStorageKind::ExternalAPMemory &&
            memory.banks > 1 && memory.bankDepth == 1 && memory.banks == memory.depth &&
            !context->configuration.configuredMemoryReadLatencies.count(memory.name))
            memory.readLatency = 0;
        resolved.insert(memory.name);
    }
    for (const auto &configured : byMemory)
    {
        if (!resolved.count(configured.first))
        {
            context->addError(
                "BACKEND_ARRAY_PARTITION_TARGET_UNKNOWN",
                "ARRAY_PARTITION target '" + configured.first +
                    "' is not a MemoryObject in the top function.",
                configured.first);
        }
    }
    if (context->hasErrors())
        return false;

    LoopInfo &loopInfo =
        getAnalysis<LoopInfoWrapperPass>(*context->top).getLoopInfo();
    ScalarEvolution &scalarEvolution =
        getAnalysis<ScalarEvolutionWrapperPass>(*context->top).getSE();
    analyzeAccessBanks(*context, loopInfo, scalarEvolution);
    return false;
}

void HI_ArrayPartitionAnalysis::getAnalysisUsage(AnalysisUsage &usage) const
{
    usage.addRequired<LoopInfoWrapperPass>();
    usage.addRequired<ScalarEvolutionWrapperPass>();
    usage.setPreservesAll();
}
