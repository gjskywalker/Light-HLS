#include "HI_MemoryObjectAnalysis.h"

#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"

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

OffsetRange inferPointerRange(const Value *pointer, const MemoryObject &memory,
                              const HLSBackendContext &context,
                              ScalarEvolution &scalarEvolution,
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
                                   scalarEvolution, visiting);
    }
    else if (const auto *gep = dyn_cast<GetElementPtrInst>(pointer))
    {
        result = inferPointerRange(gep->getPointerOperand(), memory, context,
                                   scalarEvolution, visiting);
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
            OffsetRange indexRange = evaluateSCEVRange(
                scalarEvolution.getSCEV(indexUse.get()), memory.base,
                context, scalarEvolution);
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
                                     ScalarEvolution &scalarEvolution)
{
    bool foundAccess = false;
    long long maximumAddress = -1;
    for (const auto &entry : context.memoryAccesses)
    {
        const MemoryAccessInfo &access = entry.second;
        if (access.memoryBase != memory.base)
            continue;
        foundAccess = true;
        std::set<const Value *> visiting;
        OffsetRange range = inferPointerRange(
            access.pointerOperand, memory, context, scalarEvolution, visiting);
        if (!range.valid || range.minimum < 0)
            return 0;
        maximumAddress = std::max(maximumAddress, range.maximum);
    }
    if (!foundAccess || maximumAddress < 0 ||
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
        if (argument.getType()->isPointerTy())
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
            if (const auto *load = dyn_cast<LoadInst>(&instruction))
            {
                pointer = load->getPointerOperand();
                width = cast<IntegerType>(load->getType())->getBitWidth();
            }
            else if (const auto *store = dyn_cast<StoreInst>(&instruction))
            {
                pointer = store->getPointerOperand();
                writeValue = store->getValueOperand();
                width = cast<IntegerType>(writeValue->getType())->getBitWidth();
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
            const bool isVolatile = isWrite
                ? cast<StoreInst>(instruction).isVolatile()
                : cast<LoadInst>(instruction).isVolatile();
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
            inferMinimumDepth(memory, *context, scalarEvolution);
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

        if (memory.depth != 0 && memory.inferredMinimumDepth != 0 &&
            memory.depth < memory.inferredMinimumDepth)
        {
            context->addError(
                "BACKEND_MEMORY_DEPTH_TOO_SMALL",
                "Declared depth " + std::to_string(memory.depth) +
                    " for memory '" + memory.name +
                    "' is smaller than the inferred minimum depth " +
                    std::to_string(memory.inferredMinimumDepth) + ".",
                memory.name);
        }
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
