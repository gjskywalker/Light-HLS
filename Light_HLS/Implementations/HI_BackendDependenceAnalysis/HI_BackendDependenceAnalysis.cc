#include "HI_BackendDependenceAnalysis.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Instructions.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <set>
#include <string>
#include <unordered_map>
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
    std::map<const SCEV *, long long> invariantTerms;
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
    LinearExpression result;
    result.valid = true;
    result.constant = value;
    result.reason = "exact affine expression";
    return result;
}

LinearExpression invariantExpression(const SCEV *expression)
{
    LinearExpression result = constantExpression(0);
    result.invariantTerms[expression] = 1;
    result.reason = "exact affine expression with loop-invariant offset";
    return result;
}

bool hasIterationVaryingTerms(const LinearExpression &expression)
{
    return expression.coefficient != 0 || !expression.invariantTerms.empty();
}

bool addScaledInvariantTerms(
    const std::map<const SCEV *, long long> &terms, long long scale,
    std::map<const SCEV *, long long> &result)
{
    for (const auto &term : terms)
    {
        long long scaled = 0;
        long long combined = 0;
        if (!checkedValue(static_cast<__int128>(term.second) * scale,
                          scaled) ||
            !checkedValue(static_cast<__int128>(result[term.first]) + scaled,
                          combined))
        {
            return false;
        }
        if (combined == 0)
            result.erase(term.first);
        else
            result[term.first] = combined;
    }
    return true;
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
                      result.constant) ||
        !addScaledInvariantTerms(lhs.invariantTerms, 1,
                                 result.invariantTerms) ||
        !addScaledInvariantTerms(rhs.invariantTerms, 1,
                                 result.invariantTerms))
    {
        return invalidExpression("affine expression overflow");
    }
    result.valid = true;
    result.reason = "exact affine expression";
    return result;
}

LinearExpression multiplyExpressions(const LinearExpression &lhs,
                                     const LinearExpression &rhs)
{
    if (!lhs.valid)
        return lhs;
    if (!rhs.valid)
        return rhs;
    if (hasIterationVaryingTerms(lhs) && hasIterationVaryingTerms(rhs))
        return invalidExpression("nonlinear multiplication in address");

    LinearExpression result;
    const __int128 coefficient =
        static_cast<__int128>(lhs.coefficient) * rhs.constant +
        static_cast<__int128>(rhs.coefficient) * lhs.constant;
    const __int128 constant =
        static_cast<__int128>(lhs.constant) * rhs.constant;
    if (!checkedValue(coefficient, result.coefficient) ||
        !checkedValue(constant, result.constant) ||
        !addScaledInvariantTerms(lhs.invariantTerms, rhs.constant,
                                 result.invariantTerms) ||
        !addScaledInvariantTerms(rhs.invariantTerms, lhs.constant,
                                 result.invariantTerms))
    {
        return invalidExpression("affine multiplication overflow");
    }
    result.valid = true;
    result.reason = "exact affine expression";
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
            return invalidExpression("address constant exceeds 64 bits");
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
        if (scalarEvolution.isLoopInvariant(expression, loop))
            return invariantExpression(expression);
        return invalidExpression("loop-varying address value is not affine");
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
        if (recurrence->getLoop() != loop)
        {
            if (scalarEvolution.isLoopInvariant(recurrence, loop))
                return invariantExpression(recurrence);
            return invalidExpression(
                "address recurrence is not invariant in the pipeline loop");
        }
        if (!recurrence->isAffine())
            return invalidExpression("non-affine address recurrence");
        LinearExpression start = evaluateSCEV(
            recurrence->getStart(), loop, base, context, scalarEvolution);
        LinearExpression step = evaluateSCEV(
            recurrence->getStepRecurrence(scalarEvolution), loop, base,
            context, scalarEvolution);
        if (!start.valid)
            return start;
        if (!step.valid)
            return step;
        if (start.coefficient != 0 || step.coefficient != 0 ||
            !step.invariantTerms.empty())
            return invalidExpression("nested affine recurrence is unsupported");
        start.coefficient = step.constant;
        start.reason = start.invariantTerms.empty()
                           ? "exact affine loop recurrence"
                           : "exact affine loop recurrence with invariant offset";
        return start;
    }

    return invalidExpression("unsupported ScalarEvolution address expression");
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
        return invalidExpression("cyclic pointer expression");

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
                result = invalidExpression("GEP scale exceeds 64 bits");
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
        result = invalidExpression("unsupported pointer expression");
    }
    visiting.erase(pointer);
    return result;
}

const MemoryObject *findMemoryObject(const HLSBackendContext &context,
                                     const Value *base)
{
    for (const MemoryObject &memory : context.memoryObjects)
    {
        if (memory.base == base)
            return &memory;
    }
    return nullptr;
}

bool evaluateAt(const AffineMemoryAccessPattern &pattern, unsigned iteration,
                long long &address)
{
    return checkedValue(static_cast<__int128>(pattern.coefficient) * iteration +
                            pattern.constant,
                        address);
}

DependenceKind memoryDependenceKind(const AffineMemoryAccessPattern &source,
                                    const AffineMemoryAccessPattern &sink)
{
    if (source.isWrite && !sink.isWrite)
        return DependenceKind::MemoryRAW;
    if (!source.isWrite && sink.isWrite)
        return DependenceKind::MemoryWAR;
    return DependenceKind::MemoryWAW;
}

void setConservativeDependence(MemoryDependenceInfo &result,
                               const AffineMemoryAccessPattern &source,
                               const AffineMemoryAccessPattern &sink,
                               const std::string &reason)
{
    result.exists = true;
    result.exact = false;
    result.distance = source.order < sink.order ? 0U : 1U;
    result.loopCarried = result.distance > 0;
    result.reason = reason;
}

MemoryDependenceInfo analyzeMemoryPair(
    const AffineMemoryAccessPattern &source,
    const AffineMemoryAccessPattern &sink, unsigned tripCount)
{
    MemoryDependenceInfo result;
    result.source = source.instruction;
    result.sink = sink.instruction;
    result.memory = source.memory;
    result.kind = memoryDependenceKind(source, sink);
    result.sourceEvent = source.isWrite ? MemoryEventKind::WriteRequest
                                        : MemoryEventKind::ReadRequest;
    result.sinkEvent = sink.isWrite ? MemoryEventKind::WriteRequest
                                    : MemoryEventKind::ReadRequest;

    if (source.bankMappingExact && sink.bankMappingExact)
    {
        std::vector<unsigned> commonBanks;
        std::set_intersection(
            source.possibleBanks.begin(), source.possibleBanks.end(),
            sink.possibleBanks.begin(), sink.possibleBanks.end(),
            std::back_inserter(commonBanks));
        if (commonBanks.empty())
        {
            result.exists = false;
            result.exact = true;
            result.reason = "disjoint array-partition banks";
            return result;
        }
    }

    if (!source.affine || !sink.affine)
    {
        setConservativeDependence(
            result, source, sink,
            !source.affine ? source.reason : sink.reason);
        return result;
    }

    if (source.invariantTerms != sink.invariantTerms)
    {
        setConservativeDependence(
            result, source, sink,
            "relative loop-invariant address offset is unknown");
        return result;
    }

    result.exact = true;
    if (source.maximum < sink.minimum || sink.maximum < source.minimum)
    {
        result.exists = false;
        result.reason = "disjoint affine address ranges";
        return result;
    }

    constexpr unsigned enumerationLimit = 1000000;
    if (tripCount > enumerationLimit)
    {
        setConservativeDependence(
            result, source, sink,
            "trip count exceeds exact affine enumeration limit");
        return result;
    }

    std::unordered_map<long long, std::vector<unsigned>> sinkIterations;
    for (unsigned iteration = 0; iteration < tripCount; ++iteration)
    {
        long long address = 0;
        if (!evaluateAt(sink, iteration, address))
        {
            setConservativeDependence(
                result, source, sink,
                "affine address evaluation overflow");
            return result;
        }
        sinkIterations[address].push_back(iteration);
    }

    unsigned minimumDistance = std::numeric_limits<unsigned>::max();
    for (unsigned iteration = 0; iteration < tripCount; ++iteration)
    {
        long long address = 0;
        if (!evaluateAt(source, iteration, address))
        {
            setConservativeDependence(
                result, source, sink,
                "affine address evaluation overflow");
            return result;
        }
        const auto candidates = sinkIterations.find(address);
        if (candidates == sinkIterations.end())
            continue;
        const unsigned firstAllowed =
            iteration + (source.order < sink.order ? 0U : 1U);
        const auto match = std::lower_bound(candidates->second.begin(),
                                            candidates->second.end(),
                                            firstAllowed);
        if (match == candidates->second.end())
            continue;
        minimumDistance = std::min(minimumDistance, *match - iteration);
    }

    if (minimumDistance == std::numeric_limits<unsigned>::max())
    {
        result.exists = false;
        result.reason = "no equal affine address in the loop iteration domain";
        return result;
    }

    result.exists = true;
    result.distance = minimumDistance;
    result.loopCarried = minimumDistance > 0;
    result.reason = result.loopCarried
                        ? "exact affine loop-carried request dependence"
                        : "exact same-iteration memory request dependence";
    return result;
}

void analyzeScalarRecurrences(LoopPipelineInfo &pipeline)
{
    pipeline.scalarRecurrences.clear();
    for (const PipelinePhiInfo &phiInfo : pipeline.phis)
    {
        if (phiInfo.affine)
            continue;
        ScalarRecurrenceDependence dependence;
        dependence.phi = phiInfo.phi;
        dependence.distance = 1;
        dependence.source =
            dyn_cast_or_null<Instruction>(phiInfo.recurrenceValue);
        if (!dependence.source ||
            dependence.source->getParent() != pipeline.header)
        {
            dependence.reason =
                "recurrence value is not produced in the pipeline loop";
            pipeline.scalarRecurrences.push_back(std::move(dependence));
            continue;
        }
        for (const User *user : phiInfo.phi->users())
        {
            const auto *sink = dyn_cast<Instruction>(user);
            if (!sink || sink->getParent() != pipeline.header ||
                isa<PHINode>(sink) || sink->isTerminator() ||
                isa<DbgInfoIntrinsic>(sink))
            {
                continue;
            }
            dependence.sinks.push_back(sink);
        }
        dependence.exact = !dependence.sinks.empty();
        dependence.reason = dependence.exact
                                ? "exact distance-one PHI recurrence"
                                : "recurrence PHI has no schedulable sink";
        pipeline.scalarRecurrences.push_back(std::move(dependence));
    }
}

void analyzeMemoryDependences(LoopPipelineInfo &pipeline, const Loop *loop,
                              HLSBackendContext &context,
                              ScalarEvolution &scalarEvolution)
{
    pipeline.memoryAccessPatterns.clear();
    pipeline.memoryDependences.clear();
    if (!pipeline.header || !loop)
        return;

    unsigned order = 0;
    for (const Instruction &instruction : *pipeline.header)
    {
        const auto access = context.memoryAccesses.find(&instruction);
        if (access == context.memoryAccesses.end())
        {
            ++order;
            continue;
        }
        AffineMemoryAccessPattern pattern;
        pattern.instruction = &instruction;
        pattern.memory = access->second.memory;
        pattern.isWrite = access->second.isWrite;
        pattern.bankMappingExact = access->second.bankMappingExact;
        pattern.possibleBanks = access->second.possibleBanks;
        pattern.order = order++;
        const MemoryObject *memory =
            findMemoryObject(context, access->second.memoryBase);
        if (!memory)
        {
            pattern.reason = "memory object is unavailable";
            pipeline.memoryAccessPatterns.push_back(std::move(pattern));
            continue;
        }
        std::set<const Value *> visiting;
        const LinearExpression expression = analyzePointer(
            access->second.pointerOperand, *memory, loop, context,
            scalarEvolution, visiting);
        pattern.affine = expression.valid;
        pattern.coefficient = expression.coefficient;
        pattern.constant = expression.constant;
        pattern.invariantTerms = expression.invariantTerms;
        pattern.reason = expression.reason;
        if (pattern.affine)
        {
            long long first = 0;
            long long last = 0;
            if (!evaluateAt(pattern, 0, first) ||
                !evaluateAt(pattern, pipeline.tripCount - 1, last))
            {
                pattern.affine = false;
                pattern.reason = "affine range evaluation overflow";
            }
            else
            {
                pattern.minimum = std::min(first, last);
                pattern.maximum = std::max(first, last);
            }
        }
        pipeline.memoryAccessPatterns.push_back(std::move(pattern));
    }

    for (const AffineMemoryAccessPattern &source :
         pipeline.memoryAccessPatterns)
    {
        for (const AffineMemoryAccessPattern &sink :
             pipeline.memoryAccessPatterns)
        {
            if (source.memory == sink.memory &&
                context.memoryAccesses.at(source.instruction).isFIFO)
            {
                MemoryDependenceInfo order;
                order.source = source.instruction;
                order.sink = sink.instruction;
                order.memory = source.memory;
                order.kind = DependenceKind::FIFOOrder;
                order.sourceEvent = source.isWrite ? MemoryEventKind::WriteRequest
                                                   : MemoryEventKind::ReadRequest;
                order.sinkEvent = sink.isWrite ? MemoryEventKind::WriteRequest
                                               : MemoryEventKind::ReadRequest;
                order.distance = source.order < sink.order ? 0 : 1;
                order.loopCarried = order.distance != 0;
                order.exact = true;
                order.reason = "FIFO token order, including iteration boundaries";
                pipeline.memoryDependences.push_back(std::move(order));
                continue;
            }
            if (source.memory != sink.memory ||
                (!source.isWrite && !sink.isWrite))
            {
                continue;
            }
            MemoryDependenceInfo dependence =
                analyzeMemoryPair(source, sink, pipeline.tripCount);
            const MemoryObject *memory = findMemoryObject(
                context, context.memoryAccesses.at(source.instruction).memoryBase);
            if (memory && memory->hasCombinationalScalarReads() &&
                dependence.kind == DependenceKind::MemoryWAR)
                dependence.minimumSeparation = 0;
            pipeline.memoryDependences.push_back(std::move(dependence));
        }
    }
}

} // namespace

char HI_BackendDependenceAnalysis::ID = 0;

HI_BackendDependenceAnalysis::HI_BackendDependenceAnalysis(
    HLSBackendContextPtr context)
    : ModulePass(ID), context(std::move(context))
{
}

bool HI_BackendDependenceAnalysis::runOnModule(Module &module)
{
    if (!context || context->hasErrors() || !context->top)
        return false;

    context->module = &module;
    LoopInfo &loopInfo =
        getAnalysis<LoopInfoWrapperPass>(*context->top).getLoopInfo();
    ScalarEvolution &scalarEvolution =
        getAnalysis<ScalarEvolutionWrapperPass>(*context->top).getSE();
    for (LoopPipelineInfo &pipeline : context->pipelineRegions)
    {
        analyzeScalarRecurrences(pipeline);
        Loop *loop = loopInfo.getLoopFor(
            const_cast<BasicBlock *>(pipeline.header));
        analyzeMemoryDependences(pipeline, loop, *context, scalarEvolution);
        pipeline.dependenceAnalysisValid = true;
    }
    return false;
}

void HI_BackendDependenceAnalysis::getAnalysisUsage(
    AnalysisUsage &usage) const
{
    usage.addRequired<LoopInfoWrapperPass>();
    usage.addRequired<ScalarEvolutionWrapperPass>();
    usage.setPreservesAll();
}
