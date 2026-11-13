#include "HI_MemoryObjectAnalysis.h"

#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/LazyValueInfo.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <climits>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>

using namespace llvm;
using namespace light_hls::backend;

namespace
{

using ProvenanceSet = std::set<const Value *>;

struct OffsetRange
{
    bool valid = false;
    long long minimum = 0;
    long long maximum = 0;
};

std::string instructionLocation(const Instruction &instruction)
{
    std::string text;
    raw_string_ostream output(text);
    output << instruction;
    output.flush();
    return instruction.getParent()->getName().str() + ": " + text;
}

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

OffsetRange addRanges(const OffsetRange &lhs, const OffsetRange &rhs)
{
    OffsetRange result;
    if (!lhs.valid || !rhs.valid ||
        !checkedValue(static_cast<__int128>(lhs.minimum) + rhs.minimum,
                      result.minimum) ||
        !checkedValue(static_cast<__int128>(lhs.maximum) + rhs.maximum,
                      result.maximum))
    {
        return result;
    }
    result.valid = true;
    return result;
}

OffsetRange multiplyRanges(const OffsetRange &lhs, const OffsetRange &rhs)
{
    OffsetRange result;
    if (!lhs.valid || !rhs.valid)
        return result;

    const __int128 products[] = {
        static_cast<__int128>(lhs.minimum) * rhs.minimum,
        static_cast<__int128>(lhs.minimum) * rhs.maximum,
        static_cast<__int128>(lhs.maximum) * rhs.minimum,
        static_cast<__int128>(lhs.maximum) * rhs.maximum};
    const __int128 minimum =
        *std::min_element(std::begin(products), std::end(products));
    const __int128 maximum =
        *std::max_element(std::begin(products), std::end(products));
    if (!checkedValue(minimum, result.minimum) ||
        !checkedValue(maximum, result.maximum))
    {
        return OffsetRange();
    }
    result.valid = true;
    return result;
}

OffsetRange signedWidthRange(OffsetRange range, unsigned width)
{
    if (!range.valid || width >= 64)
        return range;
    const long long minimum = -(1LL << (width - 1));
    const long long maximum = (1LL << (width - 1)) - 1;
    if (range.minimum >= minimum && range.maximum <= maximum)
        return range;
    // Narrow recurrences and truncations wrap. An unbounded arithmetic range
    // is not a physical address range after the narrowing operation.
    return {true, minimum, maximum};
}

void mergeProvenance(ProvenanceSet &destination,
                     const ProvenanceSet &source)
{
    destination.insert(source.begin(), source.end());
}

ProvenanceSet instructionProvenance(
    const Instruction &instruction,
    const std::map<const Value *, ProvenanceSet> &provenance)
{
    ProvenanceSet result;
    auto mergeValue = [&](const Value *value) {
        auto found = provenance.find(value);
        if (found != provenance.end())
            mergeProvenance(result, found->second);
    };

    if (const auto *gep = dyn_cast<GetElementPtrInst>(&instruction))
        mergeValue(gep->getPointerOperand());
    else if (const auto *ptrToInt = dyn_cast<PtrToIntInst>(&instruction))
        mergeValue(ptrToInt->getPointerOperand());
    else if (const auto *intToPtr = dyn_cast<IntToPtrInst>(&instruction))
        mergeValue(intToPtr->getOperand(0));
    else if (const auto *cast = dyn_cast<CastInst>(&instruction))
        mergeValue(cast->getOperand(0));
    else if (const auto *binary = dyn_cast<BinaryOperator>(&instruction))
    {
        if (binary->getOpcode() == Instruction::Add ||
            binary->getOpcode() == Instruction::Sub)
        {
            mergeValue(binary->getOperand(0));
            mergeValue(binary->getOperand(1));
        }
    }
    else if (const auto *phi = dyn_cast<PHINode>(&instruction))
    {
        for (const Value *incoming : phi->incoming_values())
            mergeValue(incoming);
    }
    else if (const auto *select = dyn_cast<SelectInst>(&instruction))
    {
        mergeValue(select->getTrueValue());
        mergeValue(select->getFalseValue());
    }
    return result;
}

MemoryObject *findMemoryObject(HLSBackendContext &context,
                               const Value *base)
{
    for (MemoryObject &memory : context.memoryObjects)
    {
        if (memory.base == base)
            return &memory;
    }
    return nullptr;
}

unsigned addressWidthForDepth(unsigned long long depth)
{
    if (depth == 0)
        return 64;
    unsigned width = 0;
    unsigned long long maximumAddress = depth - 1;
    while (maximumAddress)
    {
        ++width;
        maximumAddress >>= 1;
    }
    return std::max(1U, width);
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

unsigned long long allocationDepth(const AllocaInst &allocation)
{
    const auto *arraySize = dyn_cast<ConstantInt>(allocation.getArraySize());
    if (!arraySize)
        return 0;
    const unsigned long long elements =
        scalarElementCount(allocation.getAllocatedType());
    const unsigned long long instances = arraySize->getZExtValue();
    if (elements == 0 || instances == 0 ||
        instances > std::numeric_limits<unsigned long long>::max() / elements)
    {
        return 0;
    }
    return elements * instances;
}

std::vector<unsigned long long>
allocationDimensions(const AllocaInst &allocation)
{
    std::vector<unsigned long long> dimensions;
    const auto *arraySize = dyn_cast<ConstantInt>(allocation.getArraySize());
    if (!arraySize || arraySize->isZero())
        return dimensions;
    if (!arraySize->isOne())
        dimensions.push_back(arraySize->getZExtValue());
    const Type *type = allocation.getAllocatedType();
    while (const auto *array = dyn_cast<ArrayType>(type))
    {
        dimensions.push_back(array->getNumElements());
        type = array->getElementType();
    }
    if (dimensions.empty())
        dimensions.push_back(1);
    return dimensions;
}

OffsetRange evaluateSCEVRange(const SCEV *expression,
                              const Value *baseValue,
                              const HLSBackendContext &context,
                              ScalarEvolution &scalarEvolution)
{
    if (const auto *constant = dyn_cast<SCEVConstant>(expression))
    {
        const APInt &value = constant->getAPInt();
        if (!value.isSignedIntN(64))
            return OffsetRange();
        return {true, value.getSExtValue(), value.getSExtValue()};
    }

    if (const auto *unknown = dyn_cast<SCEVUnknown>(expression))
    {
        const Value *value = unknown->getValue();
        auto provenance = context.addressProvenance.find(value);
        if (value == baseValue ||
            (provenance != context.addressProvenance.end() &&
             provenance->second == baseValue))
        {
            return {true, 0, 0};
        }
        return OffsetRange();
    }

    if (const auto *cast = dyn_cast<SCEVCastExpr>(expression))
    {
        OffsetRange range = evaluateSCEVRange(cast->getOperand(), baseValue,
                                              context, scalarEvolution);
        if (!range.valid)
            return range;
        if (!cast->getOperand()->getType()->isIntegerTy())
            return range;
        if (isa<SCEVTruncateExpr>(cast))
            return signedWidthRange(range, cast->getType()->getIntegerBitWidth());
        const unsigned sourceWidth = cast->getOperand()->getType()->getIntegerBitWidth();
        range = signedWidthRange(range, sourceWidth);
        if (isa<SCEVZeroExtendExpr>(cast) && range.minimum < 0)
        {
            if (sourceWidth >= 63)
                return OffsetRange();
            const long long modulus = 1LL << sourceWidth;
            return range.maximum < 0 ?
                OffsetRange{true, range.minimum + modulus, range.maximum + modulus} :
                OffsetRange{true, 0, modulus - 1};
        }
        return range;
    }

    if (const auto *addition = dyn_cast<SCEVAddExpr>(expression))
    {
        OffsetRange result{true, 0, 0};
        for (const SCEV *operand : addition->operands())
        {
            result = addRanges(
                result, evaluateSCEVRange(operand, baseValue, context,
                                          scalarEvolution));
            if (!result.valid)
                break;
        }
        return result;
    }

    if (const auto *multiplication = dyn_cast<SCEVMulExpr>(expression))
    {
        OffsetRange result{true, 1, 1};
        for (const SCEV *operand : multiplication->operands())
        {
            result = multiplyRanges(
                result, evaluateSCEVRange(operand, baseValue, context,
                                          scalarEvolution));
            if (!result.valid)
                break;
        }
        return result;
    }

    if (const auto *recurrence = dyn_cast<SCEVAddRecExpr>(expression))
    {
        if (!recurrence->isAffine())
            return OffsetRange();
        OffsetRange start = evaluateSCEVRange(
            recurrence->getStart(), baseValue, context, scalarEvolution);
        OffsetRange step = evaluateSCEVRange(
            recurrence->getStepRecurrence(scalarEvolution), baseValue,
            context, scalarEvolution);
        if (!start.valid || !step.valid || step.minimum != step.maximum)
            return OffsetRange();

        unsigned tripCount = scalarEvolution.getSmallConstantTripCount(
            recurrence->getLoop());
        if (tripCount == 0)
            tripCount = scalarEvolution.getSmallConstantMaxTripCount(
                recurrence->getLoop());
        if (tripCount == 0)
            return OffsetRange();

        long long finalDelta = 0;
        if (!checkedValue(static_cast<__int128>(step.minimum) *
                              (tripCount - 1),
                          finalDelta))
        {
            return OffsetRange();
        }
        OffsetRange final =
            addRanges(start, {true, finalDelta, finalDelta});
        if (!final.valid)
            return OffsetRange();
        return {true, std::min(start.minimum, final.minimum),
                std::max(start.maximum, final.maximum)};
    }

    return OffsetRange();
}

using PredicateFacts = std::vector<std::pair<const Value *, ConstantRange>>;

void collectPredicateFacts(const Value *predicate, bool truth, PredicateFacts &facts,
                           unsigned depth = 0)
{
    if (!predicate || depth > 64) return;
    if (auto *comparison = dyn_cast<ICmpInst>(predicate))
    {
        const Value *value = comparison->getOperand(0);
        auto *constant = dyn_cast<ConstantInt>(comparison->getOperand(1));
        auto operation = comparison->getPredicate();
        if (!constant) { value = comparison->getOperand(1); constant = dyn_cast<ConstantInt>(comparison->getOperand(0)); operation = CmpInst::getSwappedPredicate(operation); }
        if (!truth) operation = CmpInst::getInversePredicate(operation);
        if (constant) facts.emplace_back(value, ConstantRange::makeExactICmpRegion(operation, constant->getValue()));
    }
    else if (auto *binary = dyn_cast<BinaryOperator>(predicate))
    {
        auto *constant = dyn_cast<ConstantInt>(binary->getOperand(1));
        if (binary->getOpcode() == Instruction::Xor && constant && constant->isOne())
            collectPredicateFacts(binary->getOperand(0), !truth, facts, depth + 1);
        else if ((binary->getOpcode() == Instruction::And && truth) ||
                 (binary->getOpcode() == Instruction::Or && !truth))
            for (const Value *operand : binary->operand_values()) collectPredicateFacts(operand, truth, facts, depth + 1);
    }
    else if (auto *select = dyn_cast<SelectInst>(predicate))
    {
        const auto *yes = dyn_cast<ConstantInt>(select->getTrueValue());
        const auto *no = dyn_cast<ConstantInt>(select->getFalseValue());
        if (no && no->isZero() && truth) {
            collectPredicateFacts(select->getCondition(), true, facts, depth + 1);
            collectPredicateFacts(select->getTrueValue(), true, facts, depth + 1);
        } else if (yes && yes->isZero() && truth) {
            collectPredicateFacts(select->getCondition(), false, facts, depth + 1);
            collectPredicateFacts(select->getFalseValue(), true, facts, depth + 1);
        }
    }
}

OffsetRange contextualIntegerRange(const Value *value, const Value *base,
                                   const HLSBackendContext &context,
                                   ScalarEvolution &evolution, LazyValueInfo &constraints,
                                   Instruction *where, const PredicateFacts &facts, unsigned depth = 0)
{
    if (depth > 32 || !value->getType()->isIntegerTy()) return OffsetRange();
    OffsetRange result = evaluateSCEVRange(evolution.getSCEV(const_cast<Value *>(value)), base, context, evolution);
    auto intersect = [&](OffsetRange range) {
        if (!range.valid) return;
        if (!result.valid) result = range;
        else {
            result.minimum = std::max(result.minimum, range.minimum);
            result.maximum = std::min(result.maximum, range.maximum);
            if (result.minimum > result.maximum) result = OffsetRange();
        }
    };
    auto interval = [](const ConstantRange &range) -> OffsetRange {
        if (range.isEmptySet() || !range.getSignedMin().isSignedIntN(64) ||
            !range.getSignedMax().isSignedIntN(64)) return OffsetRange();
        return {true, range.getSignedMin().getSExtValue(), range.getSignedMax().getSExtValue()};
    };
    intersect(interval(constraints.getConstantRange(const_cast<Value *>(value), where, false)));
    for (const auto &fact : facts) if (fact.first == value) intersect(interval(fact.second));
    if (auto *phi = dyn_cast<PHINode>(value))
    {
        // A PHI is the union of actual incoming edges, not the Cartesian
        // product of a variable start and the maximum trip count of its loop.
        OffsetRange merged;
        for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i)
        {
            OffsetRange edge = interval(constraints.getConstantRangeOnEdge(
                phi->getIncomingValue(i), phi->getIncomingBlock(i), const_cast<BasicBlock *>(phi->getParent()), where));
            if (!edge.valid) { merged = OffsetRange(); break; }
            if (!merged.valid) merged = edge;
            else { merged.minimum = std::min(merged.minimum, edge.minimum);
                   merged.maximum = std::max(merged.maximum, edge.maximum); }
        }
        intersect(merged);
    }
    else if (auto *binary = dyn_cast<BinaryOperator>(value))
    {
        OffsetRange a = contextualIntegerRange(binary->getOperand(0), base, context, evolution, constraints, where, facts, depth + 1);
        OffsetRange b = contextualIntegerRange(binary->getOperand(1), base, context, evolution, constraints, where, facts, depth + 1);
        OffsetRange computed;
        if (binary->getOpcode() == Instruction::Add) computed = addRanges(a, b);
        if (binary->getOpcode() == Instruction::Sub && b.valid && b.minimum != LLONG_MIN && b.maximum != LLONG_MIN)
            computed = addRanges(a, {true, -b.maximum, -b.minimum});
        if (binary->getOpcode() == Instruction::Mul) computed = multiplyRanges(a, b);
        intersect(signedWidthRange(computed, value->getType()->getIntegerBitWidth()));
    }
    else if (auto *cast = dyn_cast<CastInst>(value))
    {
        OffsetRange source = contextualIntegerRange(cast->getOperand(0), base, context, evolution, constraints, where, facts, depth + 1);
        if (source.valid && (isa<SExtInst>(cast) || isa<ZExtInst>(cast) || isa<TruncInst>(cast)))
        {
            source = signedWidthRange(source, cast->getSrcTy()->getIntegerBitWidth());
            if (isa<ZExtInst>(cast) && source.minimum < 0)
            {
                unsigned width = cast->getSrcTy()->getIntegerBitWidth();
                if (width < 63) {
                    long long modulus = 1LL << width;
                    source = source.maximum < 0 ? OffsetRange{true, source.minimum + modulus, source.maximum + modulus} : OffsetRange{true, 0, modulus - 1};
                } else source = OffsetRange();
            }
            if (isa<TruncInst>(cast)) source = signedWidthRange(source, cast->getDestTy()->getIntegerBitWidth());
            intersect(source);
        }
    }
    return result;
}

OffsetRange inferPointerRange(const Value *pointer, const MemoryObject &memory,
                              const HLSBackendContext &context,
                              ScalarEvolution &scalarEvolution,
                              LazyValueInfo &constraints,
                              Instruction *accessInstruction,
                              std::set<const Value *> &visiting)
{
    if (!pointer || !visiting.insert(pointer).second)
        return OffsetRange();

    OffsetRange result;
    if (pointer == memory.base)
    {
        result = {true, 0, 0};
    }
    else if (const auto *intToPtr = dyn_cast<IntToPtrInst>(pointer))
    {
        result = evaluateSCEVRange(
            scalarEvolution.getSCEV(intToPtr->getOperand(0)), memory.base,
            context, scalarEvolution);
    }
    else if (const auto *cast = dyn_cast<BitCastInst>(pointer))
    {
        result = inferPointerRange(cast->getOperand(0), memory, context,
                                   scalarEvolution, constraints, accessInstruction, visiting);
    }
    else if (const auto *gep = dyn_cast<GetElementPtrInst>(pointer))
    {
        result = inferPointerRange(gep->getPointerOperand(), memory, context,
                                   scalarEvolution, constraints, accessInstruction, visiting);
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
            if (scale == 0 || scale >
                                  static_cast<unsigned long long>(
                                      std::numeric_limits<long long>::max()))
            {
                result = OffsetRange();
                break;
            }
            PredicateFacts facts;
            collectPredicateFacts(backendMemoryPredicate(accessInstruction), true, facts);
            OffsetRange indexRange = contextualIntegerRange(indexUse.get(), memory.base,
                context, scalarEvolution, constraints, accessInstruction, facts);
            // A loop-wide interval includes paths where this access is not
            // executed. Intersect it with the actual load/store's CFG guards.
            const ConstantRange constrained = constraints.getConstantRange(
                const_cast<Value *>(indexUse.get()), accessInstruction, false);
            if (!constrained.isEmptySet() &&
                constrained.getSignedMin().isSignedIntN(64) &&
                constrained.getSignedMax().isSignedIntN(64))
            {
                const OffsetRange local{true, constrained.getSignedMin().getSExtValue(),
                    constrained.getSignedMax().getSExtValue()};
                if (!indexRange.valid)
                    indexRange = local;
                else
                {
                    indexRange.minimum = std::max(indexRange.minimum, local.minimum);
                    indexRange.maximum = std::min(indexRange.maximum, local.maximum);
                    if (indexRange.minimum > indexRange.maximum)
                        indexRange = OffsetRange();
                }
            }
            result = addRanges(
                result,
                multiplyRanges(indexRange,
                               {true, static_cast<long long>(scale),
                                static_cast<long long>(scale)}));
            ++indexOrdinal;
        }
    }
    visiting.erase(pointer);
    return result;
}

unsigned long long inferMinimumDepth(const MemoryObject &memory,
                                     const HLSBackendContext &context,
                                     ScalarEvolution &scalarEvolution,
                                     LazyValueInfo &constraints,
                                     unsigned long long &provenMinimumDepth)
{
    DominatorTree dominators(*context.top);
    // Prove endpoint reachability only for an unguarded constant-trip affine
    // loop nest. An inner loop must execute on every enclosing iteration;
    // interval maxima for correlated or conditionally executed loops are not
    // sufficient to reject a declared depth.
    std::function<bool(const SCEV *, const Instruction *, const Loop *)>
        attainedAffineNest = [&](const SCEV *expression,
                                 const Instruction *access,
                                 const Loop *inner) -> bool {
        if (isa<SCEVConstant>(expression))
            return true;
        if (const auto *cast = dyn_cast<SCEVPtrToIntExpr>(expression))
            return attainedAffineNest(cast->getOperand(), access, inner);
        if (const auto *unknown = dyn_cast<SCEVUnknown>(expression))
        {
            const Value *value = unknown->getValue();
            if (const auto *cast = dyn_cast<PtrToIntInst>(value))
                value = cast->getPointerOperand()->stripPointerCasts();
            return value == memory.base;
        }
        if (const auto *sum = dyn_cast<SCEVAddExpr>(expression))
        {
            unsigned origins = 0;
            for (const SCEV *operand : sum->operands())
                if (!isa<SCEVConstant>(operand))
                {
                    if (++origins > 1 ||
                        (!isa<SCEVUnknown>(operand) && !isa<SCEVPtrToIntExpr>(operand)) ||
                        !attainedAffineNest(operand, access, inner))
                        return false;
                }
            return true;
        }
        const auto *recurrence = dyn_cast<SCEVAddRecExpr>(expression);
        if (!recurrence || !recurrence->isAffine() ||
            !isa<SCEVConstant>(recurrence->getStepRecurrence(scalarEvolution)))
            return false;
        const Loop *loop = recurrence->getLoop();
        if (!loop->contains(access->getParent()) ||
            !isa<SCEVConstant>(scalarEvolution.getBackedgeTakenCount(loop)))
            return false;
        if (inner)
        {
            if (inner->getParentLoop() != loop || !loop->getLoopLatch() ||
                !dominators.dominates(inner->getHeader(), loop->getLoopLatch()))
                return false;
        }
        else if (loop->getNumBlocks() != 1 || loop->getHeader() != access->getParent())
            return false;
        return attainedAffineNest(recurrence->getStart(), access, loop);
    };
    bool foundAccess = false;
    bool complete = true;
    long long maximumAddress = -1;
    for (const auto &entry : context.memoryAccesses)
    {
        const MemoryAccessInfo &access = entry.second;
        if (access.memoryBase != memory.base)
            continue;
        foundAccess = true;
        std::set<const Value *> visiting;
        OffsetRange range = inferPointerRange(
            access.pointerOperand, memory, context, scalarEvolution, constraints,
            const_cast<Instruction *>(entry.first), visiting);
        if (!range.valid || range.minimum < 0)
        { complete = false; continue; }
        maximumAddress = std::max(maximumAddress, range.maximum);
        // An interval is an overapproximation, not evidence that its largest
        // address is reachable. Only exact constant/affine unguarded accesses
        // establish a mandatory capacity. Correlated/dynamic inputs retain
        // their declared interface contract and an explicit warning instead.
        const Value *predicate = backendMemoryPredicate(entry.first);
        if (predicate && (!isa<ConstantInt>(predicate) || cast<ConstantInt>(predicate)->isZero())) continue;
        // Frontend lowering expresses addresses as element-index arithmetic
        // followed by inttoptr; SCEV treats inttoptr itself as opaque.
        const Value *address = access.pointerOperand;
        if (const auto *cast = dyn_cast<IntToPtrInst>(address))
            address = cast->getOperand(0);
        const SCEV *offset = scalarEvolution.getSCEV(const_cast<Value *>(address));
        bool attained = attainedAffineNest(offset, entry.first, nullptr);
        if (attained)
            provenMinimumDepth = std::max(provenMinimumDepth,
                static_cast<unsigned long long>(range.maximum) + 1);
    }
    if (!complete || !foundAccess || maximumAddress < 0 ||
        static_cast<unsigned long long>(maximumAddress) ==
            std::numeric_limits<unsigned long long>::max())
    {
        return 0;
    }
    return static_cast<unsigned long long>(maximumAddress) + 1;
}

} // namespace

char HI_MemoryObjectAnalysis::ID = 0;

HI_MemoryObjectAnalysis::HI_MemoryObjectAnalysis(HLSBackendContextPtr context)
    : ModulePass(ID), context(std::move(context))
{
}

bool HI_MemoryObjectAnalysis::runOnModule(Module &module)
{
    if (!context || context->hasErrors() || !context->top)
        return false;

    context->module = &module;
    context->memoryObjects.clear();
    context->memoryAccesses.clear();
    context->addressProvenance.clear();

    std::map<const Value *, ProvenanceSet> provenance;
    unsigned argumentIndex = 0;
    for (Argument &argument : context->top->args())
    {
        if (argument.getType()->isPointerTy() && !argument.use_empty())
        {
            MemoryObject memory;
            memory.base = &argument;
            memory.argument = &argument;
            memory.name = argument.hasName()
                              ? argument.getName().str()
                              : "memory" + std::to_string(argumentIndex);
            memory.portName = sanitizeVerilogIdentifier(
                memory.name, "memory" + std::to_string(argumentIndex));
            const auto dimensions =
                context->configuration.frontendMemoryDimensions.find(
                    memory.name);
            if (dimensions !=
                context->configuration.frontendMemoryDimensions.end())
            {
                memory.dimensions = dimensions->second;
            }
            context->memoryObjects.push_back(std::move(memory));
            provenance[&argument].insert(&argument);
        }
        ++argumentIndex;
    }

    unsigned allocationIndex = 0;
    // Static storage has module lifetime, unlike an automatic stack array.
    // Model its actual initializer rather than recreating it on each ap_start.
    for (GlobalVariable &global : module.globals())
    {
        if (!global.hasInitializer() || global.isThreadLocal() ||
            global.isExternallyInitialized() || global.use_empty()) continue;
        std::function<Type *(Type *)> leafType = [&](Type *type) -> Type * {
            if (auto *array = dyn_cast<ArrayType>(type)) return leafType(array->getElementType());
            if (auto *structure = dyn_cast<StructType>(type))
            {
                Type *leaf = nullptr;
                for (Type *field : structure->elements())
                {
                    Type *next = leafType(field);
                    if (!next || (leaf && leaf != next)) return nullptr;
                    leaf = next;
                }
                return leaf;
            }
            return scalarBitWidth(type) ? type : nullptr;
        };
        Type *element = leafType(global.getValueType());
        if (!element || scalarBitWidth(element) % 8) continue;
        const DataLayout &layout = module.getDataLayout();
        const uint64_t stride = layout.getTypeAllocSize(element);
        const uint64_t bytes = layout.getTypeAllocSize(global.getValueType());
        if (!stride || bytes % stride || bytes > 1048576) continue;
        MemoryObject memory;
        memory.base = &global;
        memory.name = global.getName().str();
        memory.portName = sanitizeVerilogIdentifier(memory.name, "static_memory");
        memory.storage = MemoryStorageKind::LocalRAM;
        memory.depth = bytes / stride;
        memory.depthSource = MemoryDepthSource::Allocation;
        memory.depthExact = true;
        memory.elementWidth = scalarBitWidth(element);
        for (uint64_t index = 0; index < memory.depth; ++index)
        {
            Constant *value = ConstantFoldLoadFromConst(global.getInitializer(), element,
                APInt(layout.getIndexTypeSizeInBits(global.getType()), index * stride), layout);
            APInt bits(memory.elementWidth, 0);
            if (auto *integer = dyn_cast_or_null<ConstantInt>(value)) bits = integer->getValue();
            else if (auto *floating = dyn_cast_or_null<ConstantFP>(value)) bits = floating->getValueAPF().bitcastToAPInt();
            else break;
            SmallString<64> digits;
            bits.toString(digits, 16, false, false);
            memory.initialWords.push_back(digits.str().str());
        }
        if (memory.initialWords.size() != memory.depth) continue;
        const Type *type = global.getValueType();
        while (auto *array = dyn_cast<ArrayType>(type))
        {
            memory.dimensions.push_back(array->getNumElements());
            type = array->getElementType();
        }
        if (!type->isIntegerTy() && !type->isFloatingPointTy())
            memory.dimensions = {memory.depth};
        context->memoryObjects.push_back(std::move(memory));
        provenance[&global].insert(&global);
    }
    for (BasicBlock &block : *context->top)
    {
        for (Instruction &instruction : block)
        {
            auto *allocation = dyn_cast<AllocaInst>(&instruction);
            if (!allocation)
                continue;
            MemoryObject memory;
            memory.base = allocation;
            memory.allocation = allocation;
            memory.storage = MemoryStorageKind::LocalRAM;
            memory.name = allocation->hasName()
                              ? allocation->getName().str()
                              : "local_memory" +
                                    std::to_string(allocationIndex);
            memory.portName = sanitizeVerilogIdentifier(
                memory.name, "local_memory" + std::to_string(allocationIndex));
            memory.depth = allocationDepth(*allocation);
            memory.dimensions = allocationDimensions(*allocation);
            memory.depthSource = MemoryDepthSource::Allocation;
            memory.depthExact = memory.depth != 0;
            context->memoryObjects.push_back(std::move(memory));
            provenance[allocation].insert(allocation);
            ++allocationIndex;
        }
    }

    bool changed = true;
    while (changed)
    {
        changed = false;
        for (BasicBlock &block : *context->top)
        {
            for (Instruction &instruction : block)
            {
                ProvenanceSet next = instructionProvenance(instruction, provenance);
                if (next.empty())
                    continue;
                ProvenanceSet &current = provenance[&instruction];
                const std::size_t oldSize = current.size();
                mergeProvenance(current, next);
                changed |= current.size() != oldSize;
            }
        }
    }

    for (const auto &entry : provenance)
    {
        if (entry.second.size() == 1)
            context->addressProvenance[entry.first] = *entry.second.begin();
    }

    for (BasicBlock &block : *context->top)
    {
        for (Instruction &instruction : block)
        {
            const Value *pointer = nullptr;
            const Value *writeValue = nullptr;
            unsigned width = 0;
            bool isWrite = false;
            if (isBackendLoad(&instruction))
            {
                pointer = backendMemoryPointer(&instruction);
                width = scalarBitWidth(instruction.getType());
            }
            else if (isBackendStore(&instruction))
            {
                pointer = backendMemoryPointer(&instruction);
                writeValue = backendStoredValue(&instruction);
                width = scalarBitWidth(writeValue->getType());
                isWrite = true;
            }
            else
            {
                continue;
            }

            auto source = provenance.find(pointer);
            if (source == provenance.end() || source->second.empty())
            {
                context->addError(
                    "BACKEND_MEMORY_BASE_UNKNOWN",
                    "The memory access cannot be traced to a MemoryObject base.",
                    instructionLocation(instruction));
                continue;
            }
            if (source->second.size() != 1)
            {
                context->addError(
                    "BACKEND_MEMORY_BASE_AMBIGUOUS",
                    "The memory access may refer to more than one MemoryObject.",
                    instructionLocation(instruction));
                continue;
            }

            const Value *memoryBase = *source->second.begin();
            MemoryObject *memory = findMemoryObject(*context, memoryBase);
            if (!memory)
                continue;
            if (memory->elementWidth == 0)
                memory->elementWidth = width;
            else if (memory->elementWidth != width)
            {
                context->addError(
                    "BACKEND_MEMORY_MIXED_WIDTH",
                    "All accesses to one MemoryObject must use one integer data width.",
                    instructionLocation(instruction));
                continue;
            }

            MemoryAccessInfo access;
            access.instruction = &instruction;
            access.memoryBase = memoryBase;
            access.pointerOperand = pointer;
            access.writeValue = writeValue;
            access.memory = memory->name;
            access.dataWidth = width;
            access.isWrite = isWrite;
            const auto *argument = dyn_cast<Argument>(memoryBase);
            access.isFIFO = argument &&
                context->configuration.fifoArguments.count(argument->getArgNo());
            const auto *load = dyn_cast<LoadInst>(&instruction);
            const auto *store = dyn_cast<StoreInst>(&instruction);
            const bool isVolatile = (load && load->isVolatile()) || (store && store->isVolatile());
            if (isVolatile && !access.isFIFO)
            {
                context->addError(
                    "BACKEND_UNSUPPORTED_MEMORY_OPERATION",
                    "Volatile accesses are supported only on verified DATAFLOW FIFO endpoints.",
                    instructionLocation(instruction));
                continue;
            }
            context->memoryAccesses[&instruction] = std::move(access);
        }
    }

    for (MemoryObject &memory : context->memoryObjects)
    {
        bool hasRead = false;
        bool hasWrite = false;
        for (const auto &entry : context->memoryAccesses)
        {
            if (entry.second.memoryBase != memory.base)
                continue;
            hasWrite |= entry.second.isWrite;
            hasRead |= !entry.second.isWrite;
        }
        if (hasRead && hasWrite)
            memory.accessMode = MemoryAccessMode::ReadWrite;
        else if (hasRead)
            memory.accessMode = MemoryAccessMode::ReadOnly;
        else if (hasWrite)
            memory.accessMode = MemoryAccessMode::WriteOnly;
        else
            memory.accessMode = MemoryAccessMode::Unused;
    }

    ScalarEvolution &scalarEvolution =
        getAnalysis<ScalarEvolutionWrapperPass>(*context->top).getSE();
    // Keep both function analyses alive together inside this module pass.
    AssumptionCache assumptions(*context->top);
    TargetLibraryInfoImpl libraryImpl(Triple(module.getTargetTriple()));
    TargetLibraryInfo library(libraryImpl);
    LazyValueInfo constraints(&assumptions, &module.getDataLayout(), &library);
    std::set<std::string> memoryNames;
    for (MemoryObject &memory : context->memoryObjects)
    {
        memoryNames.insert(memory.name);
        if (memory.elementWidth == 0)
        {
            context->addError(
                "BACKEND_MEMORY_WIDTH_UNKNOWN",
                "The data width of MemoryObject '" + memory.name +
                    "' cannot be inferred because it has no supported load or store.",
                memory.name);
            continue;
        }

        auto configuredPorts =
            context->configuration.configuredMemoryPorts.find(memory.name);
        if (configuredPorts !=
            context->configuration.configuredMemoryPorts.end())
        {
            memory.ports = configuredPorts->second;
        }
        memory.portUsage.assign(memory.ports, MemoryPortUsage());
        auto configuredLatency =
            context->configuration.configuredMemoryReadLatencies.find(
                memory.name);
        if (configuredLatency !=
            context->configuration.configuredMemoryReadLatencies.end())
        {
            memory.readLatency = configuredLatency->second;
        }
        if (memory.argument && context->configuration.rtlClockEnable &&
            context->configuration.fifoArguments.count(memory.argument->getArgNo()) &&
            context->configuration.fifoReadOnRequestArguments.count(memory.argument->getArgNo()) &&
            memory.accessMode == MemoryAccessMode::ReadOnly)
        {
            memory.fifoReadOnRequest = true;
            memory.readLatency = 0;
        }

        memory.inferredMinimumDepth =
            inferMinimumDepth(memory, *context, scalarEvolution, constraints, memory.provenMinimumDepth);
        auto frontend =
            context->configuration.frontendMemoryDepths.find(memory.name);
        auto configured =
            context->configuration.configuredMemoryDepths.find(memory.name);
        if (memory.storage == MemoryStorageKind::LocalRAM)
        {
            if (memory.depth == 0)
            {
                context->addError(
                    "BACKEND_LOCAL_MEMORY_DEPTH_UNKNOWN",
                    "The local RAM allocation depth is not a positive compile-time constant.",
                    memory.name);
            }
            if (configured !=
                    context->configuration.configuredMemoryDepths.end() &&
                configured->second != memory.depth)
            {
                context->addWarning(
                    "BACKEND_MEMORY_DEPTH_CONFIG_IGNORED",
                    "Configured depth " + std::to_string(configured->second) +
                        " for local RAM '" + memory.name +
                        "' is ignored because its allocation provides exact depth " +
                        std::to_string(memory.depth) + ".",
                    memory.name);
            }
        }
        else if (frontend != context->configuration.frontendMemoryDepths.end())
        {
            memory.depth = frontend->second;
            memory.depthSource = MemoryDepthSource::Frontend;
            memory.depthExact = true;
            if (configured != context->configuration.configuredMemoryDepths.end() &&
                configured->second != frontend->second)
            {
                context->addWarning(
                    "BACKEND_MEMORY_DEPTH_CONFIG_IGNORED",
                    "Configured depth " + std::to_string(configured->second) +
                        " for memory '" + memory.name +
                        "' is ignored because the source declaration provides depth " +
                        std::to_string(frontend->second) + ".",
                    memory.name);
            }
        }
        else if (configured !=
                 context->configuration.configuredMemoryDepths.end())
        {
            memory.depth = configured->second;
            memory.depthSource = MemoryDepthSource::Configuration;
            memory.depthExact = true;
        }
        else if (memory.inferredMinimumDepth != 0)
        {
            memory.depth = memory.inferredMinimumDepth;
            memory.depthSource = MemoryDepthSource::Inferred;
            memory.depthExact = false;
        }
        else
        {
            context->addWarning(
                "BACKEND_MEMORY_DEPTH_UNKNOWN",
                "Depth is unknown for memory '" + memory.name +
                    "'; the RTL interface uses a 64-bit element address.",
                memory.name);
        }

        if (memory.depth != 0 && memory.depth < memory.provenMinimumDepth)
        {
            context->addError(
                "BACKEND_MEMORY_DEPTH_TOO_SMALL",
                "Declared depth " + std::to_string(memory.depth) +
                    " for memory '" + memory.name +
                    "' is smaller than the proven minimum depth " +
                    std::to_string(memory.provenMinimumDepth) + ".",
                memory.name);
        }
        else if (memory.depth != 0 && (memory.inferredMinimumDepth == 0 ||
                                     memory.depth < memory.inferredMinimumDepth))
            context->addWarning("BACKEND_MEMORY_RANGE_UNPROVEN",
                "Access bounds for memory '" + memory.name +
                "' are not proven within its declared depth; the caller must satisfy the source contract.", memory.name);
        memory.addressWidth = addressWidthForDepth(memory.depth);
        if (memory.dimensions.empty() && memory.depth != 0)
            memory.dimensions.push_back(memory.depth);
        memory.bankDepth = memory.depth;
        memory.bankAddressWidth = memory.addressWidth;
        memory.bankPortUsage.assign(
            memory.banks,
            std::vector<MemoryPortUsage>(memory.ports, MemoryPortUsage()));
    }

    for (const auto &configured :
         context->configuration.configuredMemoryDepths)
    {
        if (!memoryNames.count(configured.first))
        {
            context->addWarning(
                "BACKEND_MEMORY_DEPTH_TARGET_UNKNOWN",
                "Configured memory_depth target '" + configured.first +
                    "' is not a MemoryObject in the top function.",
                configured.first);
        }
    }
    for (const auto &configured :
         context->configuration.configuredMemoryPorts)
    {
        if (!memoryNames.count(configured.first))
        {
            context->addWarning(
                "BACKEND_MEMORY_INTERFACE_TARGET_UNKNOWN",
                "Configured memory_interface target '" + configured.first +
                    "' is not a MemoryObject in the top function.",
                configured.first);
        }
    }
    return false;
}

void HI_MemoryObjectAnalysis::getAnalysisUsage(AnalysisUsage &usage) const
{
    usage.addRequired<ScalarEvolutionWrapperPass>();
    usage.setPreservesAll();
}
