#include "HI_RTLGeneration.h"
#include "HI_FloatingPointRTL.h"
#include "HI_PipelineValueLifetime.h"

#include "llvm/ADT/SmallString.h"
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

unsigned integerWidth(const Type *type)
{
    return scalarBitWidth(type);
}

std::string rangeForWidth(unsigned width)
{
    if (width <= 1)
        return "";
    return "[" + std::to_string(width - 1) + ":0] ";
}

unsigned stateWidth(unsigned stateCount)
{
    unsigned width = 0;
    unsigned maximumState = stateCount > 0 ? stateCount - 1 : 0;
    while (maximumState)
    {
        ++width;
        maximumState >>= 1;
    }
    return std::max(1U, width);
}

unsigned countWidth(unsigned maximumValue)
{
    unsigned width = 0;
    do
    {
        ++width;
        maximumValue >>= 1;
    } while (maximumValue);
    return width;
}

std::string constantExpression(const ConstantInt &constant)
{
    SmallString<64> digits;
    constant.getValue().toString(digits, 16, false, false);
    return std::to_string(constant.getBitWidth()) + "'h" + digits.str().str();
}

std::string constantExpression(const ConstantFP &constant)
{
    const APInt bits = constant.getValueAPF().bitcastToAPInt();
    SmallString<64> digits;
    bits.toString(digits, 16, false, false);
    return std::to_string(bits.getBitWidth()) + "'h" + digits.str().str();
}

std::string valueExpression(const Value *value,
                            const HLSBackendContext &context)
{
    if (const auto *constant = dyn_cast<Constant>(value))
        if (const auto *type = dyn_cast<FixedVectorType>(constant->getType()))
            if (type->getNumElements() == 1)
                if (Constant *lane = constant->getAggregateElement(0U))
                    return valueExpression(lane, context);
    const auto folded = context.constantValues.find(value);
    if (folded != context.constantValues.end())
        return constantExpression(*folded->second);
    if (const auto *constant = dyn_cast<ConstantInt>(value))
        return constantExpression(*constant);
    if (const auto *constant = dyn_cast<ConstantFP>(value))
        return constantExpression(*constant);
    if (isa<UndefValue>(value) || isa<PoisonValue>(value))
        return std::to_string(std::max(1U, integerWidth(value->getType()))) + "'d0";
    const auto alias = context.ordinaryRegisterAliases.find(dyn_cast<Instruction>(value));
    if (alias != context.ordinaryRegisterAliases.end())
        return context.valueNames.at(alias->second);
    auto name = context.valueNames.find(value);
    if (name != context.valueNames.end())
        return name->second;
    return "1'b0";
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

const MemoryObject *findMemoryObject(const HLSBackendContext &context,
                                     const std::string &name)
{
    for (const MemoryObject &memory : context.memoryObjects)
    {
        if (memory.name == name)
            return &memory;
    }
    return nullptr;
}

bool memoryPortHasRead(const MemoryObject &memory, unsigned port)
{
    if (memory.storage == MemoryStorageKind::LocalRAM)
        return true;
    return port < memory.portUsage.size() && memory.portUsage[port].hasRead;
}

bool memoryPortHasWrite(const MemoryObject &memory, unsigned port)
{
    if (memory.storage == MemoryStorageKind::LocalRAM)
        return true;
    return port < memory.portUsage.size() && memory.portUsage[port].hasWrite;
}

bool memoryPortIsUsed(const MemoryObject &memory, unsigned port)
{
    return memoryPortHasRead(memory, port) || memoryPortHasWrite(memory, port);
}

const MemoryPortUsage *memoryBankPortUsage(const MemoryObject &memory,
                                           unsigned bank, unsigned port)
{
    if (bank >= memory.bankPortUsage.size() ||
        port >= memory.bankPortUsage[bank].size())
    {
        return nullptr;
    }
    return &memory.bankPortUsage[bank][port];
}

bool memoryBankPortHasRead(const MemoryObject &memory, unsigned bank,
                           unsigned port)
{
    if (memory.storage == MemoryStorageKind::LocalRAM)
        return true;
    const MemoryPortUsage *usage = memoryBankPortUsage(memory, bank, port);
    return usage && usage->hasRead;
}

bool memoryBankPortHasWrite(const MemoryObject &memory, unsigned bank,
                            unsigned port)
{
    if (memory.storage == MemoryStorageKind::LocalRAM)
        return true;
    const MemoryPortUsage *usage = memoryBankPortUsage(memory, bank, port);
    return usage && usage->hasWrite;
}

bool memoryBankPortIsUsed(const MemoryObject &memory, unsigned bank,
                          unsigned port)
{
    return memoryBankPortHasRead(memory, bank, port) ||
           memoryBankPortHasWrite(memory, bank, port);
}

bool memoryUsesScalarBanks(const MemoryObject &memory)
{
    return memory.storage == MemoryStorageKind::ExternalAPMemory &&
           memory.banks > 1 && memory.bankDepth == 1 &&
           memory.banks == memory.depth;
}

bool memoryBankDirectWriteData(const HLSBackendContext &context, const MemoryObject &memory,
                               unsigned bank, unsigned port)
{
    return std::any_of(context.binding.memoryAccesses.begin(), context.binding.memoryAccesses.end(),
        [&](const MemoryAccessBinding &binding) {
            return binding.memory == memory.name && binding.port == port &&
                std::find(binding.directWriteDataBanks.begin(), binding.directWriteDataBanks.end(), bank) !=
                    binding.directWriteDataBanks.end();
        });
}

std::string memoryStyleAttribute(const MemoryObject &memory)
{
    switch (memory.implementation)
    {
    case MemoryImplementation::BlockRAM:
        return "(* ram_style = \"block\" *) ";
    case MemoryImplementation::UltraRAM:
        return "(* ram_style = \"ultra\" *) ";
    case MemoryImplementation::DistributedRAM:
        return "(* ram_style = \"distributed\" *) ";
    case MemoryImplementation::Automatic:
        return "";
    }
    return "";
}

bool memoryScalarBankHasRead(const MemoryObject &memory, unsigned bank)
{
    for (unsigned port = 0; port < memory.ports; ++port)
    {
        if (memoryBankPortHasRead(memory, bank, port))
            return true;
    }
    return false;
}

bool memoryScalarBankHasWrite(const MemoryObject &memory, unsigned bank)
{
    for (unsigned port = 0; port < memory.ports; ++port)
    {
        if (memoryBankPortHasWrite(memory, bank, port))
            return true;
    }
    return false;
}

std::string memoryScalarBankName(const MemoryObject &memory, unsigned bank)
{
    return memory.portName + "_" + std::to_string(bank);
}

std::string memoryBankReadSignal(const MemoryObject &memory, unsigned bank,
                                 unsigned port)
{
    if (!memoryUsesScalarBanks(memory))
    {
        const std::string prefix =
            memory.banks <= 1
                ? memory.portName + "_"
                : memory.portName + "_" + std::to_string(bank) + "_";
        return prefix + "q" + std::to_string(port);
    }
    const std::string scalar = memoryScalarBankName(memory, bank);
    return memoryScalarBankHasWrite(memory, bank) ? scalar + "_i" : scalar;
}

std::string memoryBankPrefix(const MemoryObject &memory, unsigned bank)
{
    if (memory.banks <= 1)
        return memory.portName + "_";
    return memory.portName + "_" + std::to_string(bank) + "_";
}

std::string externalMemoryBaseName(const MemoryObject &memory)
{
    if (memory.banks <= 1)
        return sanitizeVerilogPortIdentifier(memory.name, "memory");
    const std::string marker = "__partition_bank";
    std::string result = sanitizeVerilogPortIdentifier(
        memory.name + marker, "memory" + marker);
    if (result.size() >= marker.size() &&
        result.compare(result.size() - marker.size(), marker.size(), marker) ==
            0)
    {
        result.erase(result.size() - marker.size());
    }
    return result;
}

const MemoryPartitionDimension *findPartitionDimension(
    const MemoryObject &memory, unsigned dimension)
{
    for (const MemoryPartitionDimension &partition :
         memory.partitionDimensions)
    {
        if (partition.dimension == dimension)
            return &partition;
    }
    return nullptr;
}

std::string partitionBankExpression(const MemoryObject &memory,
                                    const std::string &flatAddress)
{
    if (memory.banks <= 1 || memory.dimensions.empty())
        return "0";
    std::string expression = "0";
    unsigned bankStride = 1;
    unsigned long long coordinateStride = 1;
    for (std::size_t reverse = memory.dimensions.size(); reverse > 0;
         --reverse)
    {
        const unsigned dimension = static_cast<unsigned>(reverse - 1);
        const unsigned long long extent = memory.dimensions[dimension];
        const MemoryPartitionDimension *partition =
            findPartitionDimension(memory, dimension);
        if (partition)
        {
            std::string coordinate = "((" + flatAddress + " / " +
                                     std::to_string(coordinateStride) + ") % " +
                                     std::to_string(extent) + ")";
            std::string component;
            if (partition->kind == PartitionKind::Block)
            {
                component = "(" + coordinate + " / " +
                            std::to_string(partition->localExtent) + ")";
            }
            else
            {
                component = "(" + coordinate + " % " +
                            std::to_string(partition->factor) + ")";
            }
            if (bankStride != 1)
                component = "(" + component + " * " +
                            std::to_string(bankStride) + ")";
            expression = "(" + expression + " + " + component + ")";
            bankStride *= partition->factor;
        }
        coordinateStride *= extent;
    }
    return expression;
}

std::string partitionLocalAddressExpression(
    const MemoryObject &memory, const std::string &flatAddress)
{
    if (memory.banks <= 1 || memory.dimensions.empty())
        return flatAddress;
    std::string expression = "0";
    unsigned long long localStride = 1;
    unsigned long long coordinateStride = 1;
    for (std::size_t reverse = memory.dimensions.size(); reverse > 0;
         --reverse)
    {
        const unsigned dimension = static_cast<unsigned>(reverse - 1);
        const unsigned long long extent = memory.dimensions[dimension];
        const MemoryPartitionDimension *partition =
            findPartitionDimension(memory, dimension);
        std::string coordinate = "((" + flatAddress + " / " +
                                 std::to_string(coordinateStride) + ") % " +
                                 std::to_string(extent) + ")";
        unsigned long long localExtent = extent;
        if (partition)
        {
            localExtent = partition->localExtent;
            if (partition->kind == PartitionKind::Block)
            {
                coordinate = "(" + coordinate + " % " +
                             std::to_string(partition->localExtent) + ")";
            }
            else
            {
                coordinate = "(" + coordinate + " / " +
                             std::to_string(partition->factor) + ")";
            }
        }
        if (localStride != 1)
            coordinate = "(" + coordinate + " * " +
                         std::to_string(localStride) + ")";
        expression = "(" + expression + " + " + coordinate + ")";
        localStride *= localExtent;
        coordinateStride *= extent;
    }
    return expression;
}

std::string partitionedReadExpression(
    const MemoryObject &memory, const MemoryAccessBinding &binding,
    const std::string &flatAddress)
{
    if (binding.possibleBanks.empty())
        return std::to_string(memory.elementWidth) + "'d0";
    if (binding.possibleBanks.size() == 1)
    {
        return memoryBankReadSignal(memory, binding.possibleBanks.front(),
                                    binding.port);
    }
    const std::string bankExpression =
        partitionBankExpression(memory, flatAddress);
    std::string result = std::to_string(memory.elementWidth) + "'d0";
    for (auto bank = binding.possibleBanks.rbegin();
         bank != binding.possibleBanks.rend(); ++bank)
    {
        result = "((" + bankExpression + " == " + std::to_string(*bank) +
                 ") ? " + memoryBankReadSignal(memory, *bank, binding.port) +
                 " : " + result + ")";
    }
    return result;
}

void emitPartitionedMemoryControl(
    std::ostringstream &output, const std::string &indent,
    const MemoryObject &memory, const MemoryAccessBinding &binding,
    const std::string &flatAddress, bool isWrite,
    const std::string &writeValue)
{
    const std::string bankExpression =
        partitionBankExpression(memory, flatAddress);
    const std::string localAddress =
        partitionLocalAddressExpression(memory, flatAddress);
    const bool dynamicBank = binding.possibleBanks.size() > 1;
    for (unsigned bank : binding.possibleBanks)
    {
        if (memoryUsesScalarBanks(memory))
        {
            if (!isWrite)
                continue;
            const std::string scalar = memoryScalarBankName(memory, bank);
            if (dynamicBank)
            {
                output << indent << "if (" << bankExpression << " == " << bank
                       << ") begin\n";
            }
            const std::string bodyIndent =
                dynamicBank ? indent + "  " : indent;
            const std::string outputName =
                memoryScalarBankHasRead(memory, bank) ? scalar + "_o"
                                                       : scalar;
            output << bodyIndent << outputName << " = " << writeValue << ";\n"
                   << bodyIndent << outputName << "_ap_vld = 1'b1;\n";
            if (dynamicBank)
                output << indent << "end\n";
            continue;
        }
        const std::string prefix = memoryBankPrefix(memory, bank);
        const std::string ordinal = std::to_string(binding.port);
        if (dynamicBank)
        {
            output << indent << "if (" << bankExpression << " == " << bank
                   << ") begin\n";
        }
        const std::string bodyIndent =
            dynamicBank ? indent + "  " : indent;
        output << bodyIndent << prefix << "address" << ordinal << " = "
               << localAddress << ";\n"
               << bodyIndent << prefix << "ce" << ordinal << " = 1'b1;\n";
        if (isWrite)
        {
            output << bodyIndent << prefix << "we" << ordinal << " = 1'b1;\n";
            if (std::find(binding.directWriteDataBanks.begin(),
                          binding.directWriteDataBanks.end(), bank) ==
                binding.directWriteDataBanks.end())
                output << bodyIndent << prefix << "d" << ordinal << " = "
                       << writeValue << ";\n";
        }
        if (dynamicBank)
            output << indent << "end\n";
    }
}

unsigned long long scalarElementCount(const Type *type);

const LoopPipelineInfo *findPipeline(const HLSBackendContext &context,
                                     const BasicBlock *header)
{
    for (const LoopPipelineInfo &pipeline : context.pipelineRegions)
    {
        if (pipeline.header == header)
            return &pipeline;
    }
    return nullptr;
}

bool pipelineStoresValue(const LoopPipelineInfo &pipeline, const Value *value)
{
    return pipeline.contextValues.count(value) != 0;
}

std::string pipelineStorageExpression(const HLSBackendContext &context,
                                      const LoopPipelineInfo &pipeline,
                                      const Value *value, const std::string &slot)
{
    const unsigned slots = pipelineValueStorageSlots(pipeline, value);
    const auto alias = pipeline.storageAliases.find(value);
    const std::string &name = context.valueNames.at(
        alias == pipeline.storageAliases.end() ? value : alias->second);
    if (slots == 1)
        return name + "_stage";
    const std::string index = slots == pipeline.contextSlots ? slot :
        "(" + slot + " % " + std::to_string(slots) + ")";
    return name + "_ctx[" + index + "]";
}

bool needsGlobalRTLRegister(const HLSBackendContext &context,
                            const Value *value)
{
    const auto *instruction = dyn_cast_or_null<Instruction>(value);
    if (!instruction)
        return true;
    if (context.combinationalValues.count(instruction) || context.forwardedLoads.count(instruction) ||
        context.constantValues.count(instruction) || context.ordinaryRegisterAliases.count(instruction))
        return false;
    const LoopPipelineInfo *pipeline =
        findPipeline(context, instruction->getParent());
    return !pipeline || pipeline->liveOutValues.count(value) != 0;
}

std::string pipelinePrefix(const LoopPipelineInfo &pipeline)
{
    return "pipe_" + sanitizeVerilogIdentifier(pipeline.label, "loop");
}

std::string affinePhiExpression(const PipelinePhiInfo &phiInfo,
                                const std::string &iterationExpression,
                                const HLSBackendContext &context,
                                const LoopPipelineInfo &pipeline);

const PipelinePhiInfo *findPipelinePhi(const LoopPipelineInfo &pipeline,
                                       const Value *value)
{
    for (const PipelinePhiInfo &phiInfo : pipeline.phis)
    {
        if (phiInfo.phi == value)
            return &phiInfo;
    }
    return nullptr;
}

std::string pipelineInstructionExpression(
    const Instruction &instruction, const std::string &slotExpression,
    const HLSBackendContext &context, const LoopPipelineInfo &pipeline,
    int samplingCycle);

std::string pipelinePointerOffsetExpression(
    const Value *value, const MemoryObject &memory,
    const HLSBackendContext &context, const LoopPipelineInfo &pipeline,
    const std::string &slotExpression, int samplingCycle);

std::string pipelineValueExpression(const Value *value,
                                    const std::string &slotExpression,
                                    const HLSBackendContext &context,
                                    const LoopPipelineInfo &pipeline,
                                    int samplingCycle = -1)
{
    const auto folded = context.constantValues.find(value);
    if (folded != context.constantValues.end())
        return constantExpression(*folded->second);
    if (const auto *constant = dyn_cast<ConstantInt>(value))
        return constantExpression(*constant);
    if (isa<UndefValue>(value) || isa<PoisonValue>(value))
        return std::to_string(std::max(1U, integerWidth(value->getType()))) +
               "'d0";
    const auto *instruction = dyn_cast<Instruction>(value);
    auto name = context.valueNames.find(value);
    if (instruction && pipeline.forwardedLoads.count(instruction))
    {
        const auto access = context.memoryAccesses.find(instruction);
        if (access != context.memoryAccesses.end())
            for (const MemoryAccessBinding &binding : context.binding.memoryAccesses)
                if (binding.instruction == instruction)
                    for (const MemoryObject &memory : context.memoryObjects)
                        if (memory.name == access->second.memory)
                            return partitionedReadExpression(memory, binding,
                                binding.possibleBanks.size() > 1 ? pipelinePointerOffsetExpression(
                                    access->second.pointerOperand, memory, context, pipeline,
                                    slotExpression, samplingCycle) : "0");
    }
    if (instruction && instruction->getParent() == pipeline.header &&
        context.combinationalValues.count(instruction))
        return "$unsigned(" + std::to_string(integerWidth(value->getType())) +
               "'(" + pipelineInstructionExpression(*instruction, slotExpression,
                                                     context, pipeline, samplingCycle) + "))";
    if (instruction && instruction->getParent() == pipeline.header &&
        pipeline.derivedValues.count(value))
    {
        const PipelinePhiInfo *phiInfo = findPipelinePhi(pipeline, value);
        if (phiInfo)
        {
            return affinePhiExpression(
                *phiInfo,
                pipeline.stageIndexedController ?
                    pipelinePrefix(pipeline) + "_iteration_age[" +
                        std::to_string(samplingCycle) + "]" :
                    pipelinePrefix(pipeline) + "_iteration_ctx[" + slotExpression + "]",
                context, pipeline);
        }
    }
    if (instruction && instruction->getParent() == pipeline.header &&
        name != context.valueNames.end() &&
        pipelineStoresValue(pipeline, value))
    {
        return pipelineStorageExpression(context, pipeline, value, slotExpression);
    }
    return valueExpression(value, context);
}

std::string pipelinePointerOffsetExpression(
    const Value *value, const MemoryObject &memory,
    const HLSBackendContext &context, const LoopPipelineInfo &pipeline,
    const std::string &slotExpression, std::set<const Value *> &visiting,
    int samplingCycle)
{
    if (!value || !visiting.insert(value).second)
        return std::to_string(memory.addressWidth) + "'d0";

    std::string result = std::to_string(memory.addressWidth) + "'d0";
    if (value == memory.base || isa<ConstantPointerNull>(value))
    {
        result = std::to_string(memory.addressWidth) + "'d0";
    }
    else if (const auto *intToPtr = dyn_cast<IntToPtrInst>(value))
    {
        result = pipelineValueExpression(intToPtr->getOperand(0),
                                         slotExpression, context, pipeline, samplingCycle);
    }
    else if (const auto *cast = dyn_cast<BitCastInst>(value))
    {
        result = pipelinePointerOffsetExpression(
            cast->getOperand(0), memory, context, pipeline, slotExpression,
            visiting, samplingCycle);
    }
    else if (const auto *gep = dyn_cast<GetElementPtrInst>(value))
    {
        result = pipelinePointerOffsetExpression(
            gep->getPointerOperand(), memory, context, pipeline,
            slotExpression, visiting, samplingCycle);
        Type *indexedType = gep->getSourceElementType();
        unsigned indexOrdinal = 0;
        for (const Use &indexUse : gep->indices())
        {
            const Value *index = indexUse.get();
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
            std::string term = pipelineValueExpression(
                index, slotExpression, context, pipeline, samplingCycle);
            if (scale != 1)
                term = "(" + term + " * " + std::to_string(scale) + ")";
            result = "(" + result + " + " + term + ")";
            ++indexOrdinal;
        }
    }
    visiting.erase(value);
    return result;
}

std::string pipelinePointerOffsetExpression(
    const Value *value, const MemoryObject &memory,
    const HLSBackendContext &context, const LoopPipelineInfo &pipeline,
    const std::string &slotExpression, int samplingCycle)
{
    std::set<const Value *> visiting;
    return pipelinePointerOffsetExpression(value, memory, context, pipeline,
                                           slotExpression, visiting, samplingCycle);
}

unsigned long long scalarElementCount(const Type *type)
{
    if (const auto *array = dyn_cast<ArrayType>(type))
    {
        return array->getNumElements() *
               scalarElementCount(array->getElementType());
    }
    return 1;
}

std::string pointerOffsetExpression(const Value *value,
                                    const MemoryObject &memory,
                                    const HLSBackendContext &context,
                                    std::set<const Value *> &visiting)
{
    if (!value || !visiting.insert(value).second)
        return std::to_string(memory.addressWidth) + "'d0";

    std::string result = std::to_string(memory.addressWidth) + "'d0";
    if (value == memory.base || isa<ConstantPointerNull>(value))
    {
        result = std::to_string(memory.addressWidth) + "'d0";
    }
    else if (const auto *intToPtr = dyn_cast<IntToPtrInst>(value))
    {
        result = valueExpression(intToPtr->getOperand(0), context);
    }
    else if (const auto *cast = dyn_cast<BitCastInst>(value))
    {
        result = pointerOffsetExpression(cast->getOperand(0), memory, context,
                                         visiting);
    }
    else if (const auto *gep = dyn_cast<GetElementPtrInst>(value))
    {
        result = pointerOffsetExpression(gep->getPointerOperand(), memory,
                                         context, visiting);
        Type *indexedType = gep->getSourceElementType();
        unsigned indexOrdinal = 0;
        for (const Use &indexUse : gep->indices())
        {
            const Value *index = indexUse.get();
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
            std::string term = valueExpression(index, context);
            if (scale != 1)
                term = "(" + term + " * " + std::to_string(scale) + ")";
            result = "(" + result + " + " + term + ")";
            ++indexOrdinal;
        }
    }
    visiting.erase(value);
    return result;
}

std::string pointerOffsetExpression(const Value *value,
                                    const MemoryObject &memory,
                                    const HLSBackendContext &context)
{
    std::set<const Value *> visiting;
    return pointerOffsetExpression(value, memory, context, visiting);
}

std::string signedExpression(const std::string &expression)
{
    return "$signed(" + expression + ")";
}

std::string binaryExpression(const BinaryOperator &binary,
                             const HLSBackendContext &context)
{
    const std::string lhs = valueExpression(binary.getOperand(0), context);
    const std::string rhs = valueExpression(binary.getOperand(1), context);
    switch (binary.getOpcode())
    {
    case Instruction::Add:
        return lhs + " + " + rhs;
    case Instruction::Sub:
        return lhs + " - " + rhs;
    case Instruction::Mul:
        return "$unsigned(" + std::to_string(unsignedArithmeticWidth(binary.getOperand(0),
                   binary.getModule()->getDataLayout())) + "'(" + lhs + ")) * $unsigned(" +
               std::to_string(unsignedArithmeticWidth(binary.getOperand(1),
                   binary.getModule()->getDataLayout())) + "'(" + rhs + "))";
    case Instruction::UDiv:
        return lhs + " / " + rhs;
    case Instruction::SDiv:
        return signedExpression(lhs) + " / " + signedExpression(rhs);
    case Instruction::URem:
        return lhs + " % " + rhs;
    case Instruction::SRem:
        return signedExpression(lhs) + " % " + signedExpression(rhs);
    case Instruction::Shl:
        return lhs + " << " + rhs;
    case Instruction::LShr:
        return lhs + " >> " + rhs;
    case Instruction::AShr:
        return signedExpression(lhs) + " >>> " + rhs;
    case Instruction::And:
        return lhs + " & " + rhs;
    case Instruction::Or:
        return lhs + " | " + rhs;
    case Instruction::Xor:
        return lhs + " ^ " + rhs;
    default:
        return "1'b0";
    }
}

std::string comparisonExpression(const ICmpInst &comparison,
                                 const HLSBackendContext &context)
{
    std::string lhs = valueExpression(comparison.getOperand(0), context);
    std::string rhs = valueExpression(comparison.getOperand(1), context);
    const bool signedPredicate = comparison.isSigned();
    if (signedPredicate)
    {
        lhs = signedExpression(lhs);
        rhs = signedExpression(rhs);
    }
    std::string operation;
    switch (comparison.getPredicate())
    {
    case CmpInst::ICMP_EQ:
        operation = "==";
        break;
    case CmpInst::ICMP_NE:
        operation = "!=";
        break;
    case CmpInst::ICMP_UGT:
    case CmpInst::ICMP_SGT:
        operation = ">";
        break;
    case CmpInst::ICMP_UGE:
    case CmpInst::ICMP_SGE:
        operation = ">=";
        break;
    case CmpInst::ICMP_ULT:
    case CmpInst::ICMP_SLT:
        operation = "<";
        break;
    case CmpInst::ICMP_ULE:
    case CmpInst::ICMP_SLE:
        operation = "<=";
        break;
    default:
        operation = "==";
        break;
    }
    return lhs + " " + operation + " " + rhs;
}

std::string castExpression(const CastInst &cast,
                           const HLSBackendContext &context)
{
    const std::string source = valueExpression(cast.getOperand(0), context);
    const unsigned sourceWidth = integerWidth(cast.getSrcTy());
    const unsigned destinationWidth = integerWidth(cast.getDestTy());
    if (isa<SExtInst>(cast) && destinationWidth > sourceWidth)
    {
        return "{{" + std::to_string(destinationWidth - sourceWidth) + "{" +
               source + "[" + std::to_string(sourceWidth - 1) + "]}}, " + source +
               "}";
    }
    return source;
}

std::string instructionExpression(const Instruction &instruction,
                                  const HLSBackendContext &context)
{
    if (const auto *binary = dyn_cast<BinaryOperator>(&instruction))
        return binaryExpression(*binary, context);
    if (const auto *comparison = dyn_cast<ICmpInst>(&instruction))
        return comparisonExpression(*comparison, context);
    if (const auto *select = dyn_cast<SelectInst>(&instruction))
    {
        return valueExpression(select->getCondition(), context) + " ? " +
               valueExpression(select->getTrueValue(), context) + " : " +
               valueExpression(select->getFalseValue(), context);
    }
    if (const auto *cast = dyn_cast<CastInst>(&instruction))
    {
        if (const auto *ptrToInt = dyn_cast<PtrToIntInst>(cast))
        {
            auto provenance = context.addressProvenance.find(ptrToInt);
            if (provenance != context.addressProvenance.end())
            {
                const MemoryObject *memory =
                    findMemoryObject(context, provenance->second);
                if (memory)
                {
                    return pointerOffsetExpression(
                        ptrToInt->getPointerOperand(), *memory, context);
                }
            }
            return "64'd0";
        }
        return castExpression(*cast, context);
    }
    if (const auto *freeze = dyn_cast<FreezeInst>(&instruction))
        return valueExpression(freeze->getOperand(0), context);
    return "1'b0";
}

std::string boundInstructionExpression(
    const Instruction &instruction, const std::vector<std::string> &operands,
    bool narrowMultiply = true)
{
    if (isa<ExtractElementInst>(instruction)) return operands.at(0);
    if (isa<InsertElementInst>(instruction)) return operands.at(1);
    if (const auto *binary = dyn_cast<BinaryOperator>(&instruction))
    {
        // Inlined pipeline values can be compound expressions, not RTL names.
        // Keep each LLVM operand grouped before composing its consumer.
        const std::string lhs = "(" + operands.at(0) + ")";
        const std::string rhs = "(" + operands.at(1) + ")";
        switch (binary->getOpcode())
        {
        case Instruction::Add:
            return lhs + " + " + rhs;
        case Instruction::Sub:
            return lhs + " - " + rhs;
        case Instruction::Mul:
            if (!narrowMultiply)
                return lhs + " * " + rhs;
            return "$unsigned(" + std::to_string(unsignedArithmeticWidth(binary->getOperand(0),
                       binary->getModule()->getDataLayout())) + "'(" + lhs + ")) * $unsigned(" +
                   std::to_string(unsignedArithmeticWidth(binary->getOperand(1),
                       binary->getModule()->getDataLayout())) + "'(" + rhs + "))";
        case Instruction::UDiv:
            return lhs + " / " + rhs;
        case Instruction::SDiv:
            return signedExpression(lhs) + " / " + signedExpression(rhs);
        case Instruction::URem:
            return lhs + " % " + rhs;
        case Instruction::SRem:
            return signedExpression(lhs) + " % " + signedExpression(rhs);
        case Instruction::Shl:
            return lhs + " << " + rhs;
        case Instruction::LShr:
            return lhs + " >> " + rhs;
        case Instruction::AShr:
            return signedExpression(lhs) + " >>> " + rhs;
        case Instruction::And:
            return lhs + " & " + rhs;
        case Instruction::Or:
            return lhs + " | " + rhs;
        case Instruction::Xor:
            return lhs + " ^ " + rhs;
        default:
            return "1'b0";
        }
    }
    if (const auto *comparison = dyn_cast<ICmpInst>(&instruction))
    {
        std::string lhs = "(" + operands.at(0) + ")";
        std::string rhs = "(" + operands.at(1) + ")";
        if (comparison->isSigned())
        {
            lhs = signedExpression(lhs);
            rhs = signedExpression(rhs);
        }
        std::string operation;
        switch (comparison->getPredicate())
        {
        case CmpInst::ICMP_EQ:
            operation = "==";
            break;
        case CmpInst::ICMP_NE:
            operation = "!=";
            break;
        case CmpInst::ICMP_UGT:
        case CmpInst::ICMP_SGT:
            operation = ">";
            break;
        case CmpInst::ICMP_UGE:
        case CmpInst::ICMP_SGE:
            operation = ">=";
            break;
        case CmpInst::ICMP_ULT:
        case CmpInst::ICMP_SLT:
            operation = "<";
            break;
        case CmpInst::ICMP_ULE:
        case CmpInst::ICMP_SLE:
            operation = "<=";
            break;
        default:
            operation = "==";
            break;
        }
        return lhs + " " + operation + " " + rhs;
    }
    if (isa<SelectInst>(instruction))
        return "(" + operands.at(0) + ") ? (" + operands.at(1) + ") : (" +
               operands.at(2) + ")";
    if (const auto *cast = dyn_cast<CastInst>(&instruction))
    {
        const unsigned sourceWidth = integerWidth(cast->getSrcTy());
        const unsigned destinationWidth = integerWidth(cast->getDestTy());
        if (isa<SExtInst>(cast) && destinationWidth > sourceWidth)
        {
            return "$signed(" + operands.at(0) + ")";
        }
        return operands.at(0);
    }
    if (isa<FreezeInst>(instruction))
        return operands.at(0);
    return "1'b0";
}

std::string pipelineInstructionExpression(
    const Instruction &instruction, const std::string &slotExpression,
    const HLSBackendContext &context, const LoopPipelineInfo &pipeline,
    int samplingCycle)
{
    if (const auto *ptrToInt = dyn_cast<PtrToIntInst>(&instruction))
    {
        const auto provenance = context.addressProvenance.find(ptrToInt);
        if (provenance != context.addressProvenance.end())
        {
            const MemoryObject *memory =
                findMemoryObject(context, provenance->second);
            if (memory)
            {
                return pipelinePointerOffsetExpression(
                    ptrToInt->getPointerOperand(), *memory, context, pipeline,
                    slotExpression, samplingCycle);
            }
        }
        return "64'd0";
    }

    std::vector<std::string> operands;
    for (const Use &operandUse : instruction.operands())
    {
        if (!scalarBitWidth(operandUse->getType()))
            continue;
        operands.push_back(pipelineValueExpression(
            operandUse.get(), slotExpression, context, pipeline, samplingCycle));
    }
    if (!operands.empty())
        return boundInstructionExpression(instruction, operands);
    return "1'b0";
}

std::string affinePhiExpression(const PipelinePhiInfo &phiInfo,
                                const std::string &iterationExpression,
                                const HLSBackendContext &context,
                                const LoopPipelineInfo &pipeline)
{
    const unsigned width = integerWidth(phiInfo.phi->getType());
    const std::string initial = pipelineValueExpression(
        phiInfo.initialValue, "0", context, pipeline);
    if (phiInfo.affineStep == 0)
        return "$unsigned(" + std::to_string(width) + "'(" + initial + "))";
    const unsigned long long magnitude =
        phiInfo.affineStep < 0
            ? static_cast<unsigned long long>(-(phiInfo.affineStep + 1)) + 1
            : static_cast<unsigned long long>(phiInfo.affineStep);
    const std::string delta = "(" + iterationExpression + " * " +
                              std::to_string(width) + "'d" +
                              std::to_string(magnitude) + ")";
    // A derived PHI must retain its LLVM bit width even in a wider consumer
    // (e.g. GEP or sign extension). Signed consumers add an explicit $signed.
    return "$unsigned(" + std::to_string(width) + "'((" + initial + ")" +
           (phiInfo.affineStep < 0 ? " - " : " + ") + delta + "))";
}

const ScalarRecurrenceDependence *findScalarRecurrence(
    const LoopPipelineInfo &pipeline, const PHINode *phi)
{
    for (const ScalarRecurrenceDependence &dependence :
         pipeline.scalarRecurrences)
    {
        if (dependence.phi == phi)
            return &dependence;
    }
    return nullptr;
}

std::string feedbackSlotExpression(const std::string &sourceSlot,
                                   unsigned distance,
                                   unsigned contextSlots)
{
    if (contextSlots == 0)
        return "0";
    distance %= contextSlots;
    if (distance == 0)
        return sourceSlot;
    if (distance == 1)
    {
        return "((" + sourceSlot + " == " +
               std::to_string(contextSlots - 1) + ") ? 0 : " + sourceSlot +
               " + 1'b1)";
    }
    return "((" + sourceSlot + " + " + std::to_string(distance) +
           ") % " + std::to_string(contextSlots) + ")";
}

std::string previousSlotExpression(const std::string &nextSlot,
                                   unsigned contextSlots)
{
    if (contextSlots <= 1)
        return "0";
    return "((" + nextSlot + " == 0) ? " +
           std::to_string(contextSlots - 1) + " : " + nextSlot +
           " - 1'b1)";
}

const BlockSchedule *findBlockSchedule(const HLSBackendContext &context,
                                       const BasicBlock *block)
{
    for (const BlockSchedule &schedule : context.schedule.blocks)
    {
        if (schedule.block == block)
            return &schedule;
    }
    return nullptr;
}

void emitPhiAssignments(std::ostringstream &output,
                        const BasicBlock &predecessor,
                        const BasicBlock &successor,
                        const HLSBackendContext &context,
                        const std::string &indent)
{
    const LoopPipelineInfo *pipeline = findPipeline(context, &successor);
    for (const Instruction &instruction : successor)
    {
        const auto *phi = dyn_cast<PHINode>(&instruction);
        if (!phi)
            break;
        if (pipeline)
            continue;
        const Value *incoming = phi->getIncomingValueForBlock(&predecessor);
        if (!incoming)
            continue;
        const auto storageAlias = context.ordinaryRegisterAliases.find(dyn_cast<Instruction>(incoming));
        if (&predecessor == &successor && storageAlias != context.ordinaryRegisterAliases.end() &&
            storageAlias->second == phi)
            continue; // The unconditional issue-edge write also covers loop exit.
        auto destination = context.valueNames.find(phi);
        if (destination == context.valueNames.end())
            continue;
        std::string expression = valueExpression(incoming, context);
        const auto bypass = context.ordinaryBackedgeBypasses.find(phi);
        if (&predecessor == &successor && bypass != context.ordinaryBackedgeBypasses.end())
        {
            expression = instructionExpression(*bypass->second, context);
            for (const OperationBinding &binding : context.binding.operations)
                if (binding.instruction == bypass->second)
                {
                    expression = binding.instance + "_out";
                    break;
                }
        }
        for (const OrdinaryReadPhiBypass &readBypass : context.ordinaryReadPhiBypasses)
            if (readBypass.predecessor == &predecessor && readBypass.phi == phi)
            {
                const MemoryAccessInfo &access = context.memoryAccesses.at(readBypass.load);
                const MemoryObject *memory = findMemoryObject(context, access.memoryBase);
                for (const MemoryAccessBinding &binding : context.binding.memoryAccesses)
                    if (binding.instruction == readBypass.load && memory)
                    {
                        expression = partitionedReadExpression(*memory, binding,
                            pointerOffsetExpression(access.pointerOperand, *memory, context));
                        break;
                    }
            }
        output << indent << destination->second << " <= " << expression << ";\n";
    }
}

void emitBranchDestination(std::ostringstream &output, const BasicBlock &successor,
                           const HLSBackendContext &context, const std::string &indent)
{
    if (context.ordinaryReturnExits.count(&successor))
    {
        if (const Value *value = cast<ReturnInst>(successor.getTerminator())->getReturnValue())
            output << indent << "ap_return <= " << valueExpression(value, context) << ";\n";
        output << indent << "ap_done <= 1'b1;\n"
               << indent << "ap_idle <= 1'b1;\n"
               << indent << "state <= STATE_IDLE;\n";
    }
    else
        output << indent << "state <= " << findBlockSchedule(context, &successor)->firstState << ";\n";
}

void emitTransition(std::ostringstream &output, const BasicBlock &block,
                    const HLSBackendContext &context)
{
    const Instruction *terminator = block.getTerminator();
    if (const auto *branch = dyn_cast<BranchInst>(terminator))
    {
        if (branch->isUnconditional())
        {
            const BasicBlock *successor = branch->getSuccessor(0);
            emitPhiAssignments(output, block, *successor, context, "          ");
            emitBranchDestination(output, *successor, context, "          ");
            return;
        }

        const BasicBlock *trueSuccessor = branch->getSuccessor(0);
        const BasicBlock *falseSuccessor = branch->getSuccessor(1);
        output << "          if ("
               << valueExpression(branch->getCondition(), context)
               << ") begin\n";
        emitPhiAssignments(output, block, *trueSuccessor, context, "            ");
        emitBranchDestination(output, *trueSuccessor, context, "            ");
        output << "          end else begin\n";
        emitPhiAssignments(output, block, *falseSuccessor, context, "            ");
        emitBranchDestination(output, *falseSuccessor, context, "            ");
        output << "          end\n";
        return;
    }

    const auto *returnInstruction = cast<ReturnInst>(terminator);
    if (returnInstruction->getNumOperands() == 1)
    {
        output << "          ap_return <= "
               << valueExpression(returnInstruction->getReturnValue(), context)
               << ";\n";
    }
    output << "          ap_done <= 1'b1;\n"
           << "          ap_idle <= 1'b1;\n"
           << "          state <= STATE_IDLE;\n";
}

void emitPipelineExitPhiAssignments(
    std::ostringstream &output, const LoopPipelineInfo &pipeline,
    const HLSBackendContext &context, const std::string &slotExpression,
    const std::string &indent)
{
    if (!pipeline.header || !pipeline.exit)
        return;
    // A successor pipeline initializes its own context PHIs after the current
    // region retires and exports its live-outs. They are not global registers.
    if (findPipeline(context, pipeline.exit))
        return;
    for (const Instruction &instruction : *pipeline.exit)
    {
        const auto *phi = dyn_cast<PHINode>(&instruction);
        if (!phi)
            break;
        const Value *incoming =
            phi->getIncomingValueForBlock(pipeline.header);
        if (!incoming)
            continue;
        const auto destination = context.valueNames.find(phi);
        if (destination == context.valueNames.end())
            continue;
        output << indent << destination->second << " <= "
               << pipelineValueExpression(incoming, slotExpression, context,
                                          pipeline, pipeline.pipelineLatency)
               << ";\n";
    }
}

const ReturnInst *pipelineExitReturn(const LoopPipelineInfo &pipeline,
                                     const BlockSchedule *exitSchedule)
{
    if (!pipeline.exit || !exitSchedule || exitSchedule->stateCount != 0)
        return nullptr;
    return dyn_cast<ReturnInst>(pipeline.exit->getTerminator());
}

std::string pipelineReturnExpression(const ReturnInst &returnInstruction,
                                     const LoopPipelineInfo &pipeline,
                                     const HLSBackendContext &context,
                                     const std::string &slotExpression)
{
    const Value *value = returnInstruction.getReturnValue();
    if (const auto *phi = dyn_cast_or_null<PHINode>(value))
    {
        if (phi->getParent() == pipeline.exit)
        {
            const Value *incoming =
                phi->getIncomingValueForBlock(pipeline.header);
            if (incoming)
                value = incoming;
        }
    }
    if (value == pipeline.forwardedReturnValue)
    {
        for (const OperationBinding &binding : context.binding.operations)
            if (binding.instruction == value)
                return binding.instance + "_out";
        return pipelineInstructionExpression(*pipeline.forwardedReturnValue,
            slotExpression, context, pipeline, pipeline.pipelineLatency);
    }
    return pipelineValueExpression(value, slotExpression, context, pipeline, pipeline.pipelineLatency);
}

void emitPipelineInitialization(std::ostringstream &output, const LoopPipelineInfo &pipeline,
                                const HLSBackendContext &context, const std::string &indent,
                                const std::map<const Argument *, std::string> *entryPorts = nullptr)
{
    const std::string prefix = pipelinePrefix(pipeline);
    output << indent << prefix << "_valid <= " << (pipeline.pipelineLatency + 1) << "'d1;\n"
           << indent << prefix << "_launch_count <= 1;\n";
    if (!pipeline.stageIndexedController)
        output << indent << prefix << "_slot_age[0] <= 0;\n"
               << indent << prefix << "_next_slot <= " << (pipeline.contextSlots > 1 ? 1 : 0) << ";\n";
    if (pipeline.stageIndexedController)
    {
        if (pipeline.iterationTagLastStage >= 0)
            output << indent << prefix << "_iteration_age[0] <= 0;\n";
        if (pipeline.lastTokenController)
            output << indent << prefix << "_last <= " << (pipeline.pipelineLatency + 1)
                   << "'d" << (pipeline.tripCount == 1 ? 1 : 0) << ";\n";
    }
    else if (!pipeline.derivedValues.empty())
        output << indent << prefix << "_iteration_ctx[0] <= 0;\n";
    if (pipeline.achievedII > 1)
        output << indent << prefix << "_phase <= 0;\n";
    for (const PipelinePhiInfo &phiInfo : pipeline.phis)
        if (context.valueNames.count(phiInfo.phi) && pipelineStoresValue(pipeline, phiInfo.phi))
        {
            const auto *argument = dyn_cast<Argument>(phiInfo.initialValue);
            // The accepted start edge samples live arguments, not the old
            // captured argument registers from the preceding transaction.
            output << indent << pipelineStorageExpression(context, pipeline, phiInfo.phi, "0")
                   << " <= " << (entryPorts && argument ? entryPorts->at(argument) :
                       pipelineValueExpression(phiInfo.initialValue, "0", context, pipeline))
                   << ";\n";
        }
}

void emitPipelineState(
    std::ostringstream &output, const LoopPipelineInfo &pipeline,
    const BlockSchedule &blockSchedule, const HLSBackendContext &context,
    const std::map<const Instruction *, const OperationBinding *> &operationBindings,
    const std::map<const Instruction *, const MemoryAccessBinding *> &memoryBindings,
    unsigned fsmWidth)
{
    const std::string prefix = pipelinePrefix(pipeline);
    const BlockSchedule *exitSchedule =
        findBlockSchedule(context, pipeline.exit);
    output << "        " << fsmWidth << "'d" << blockSchedule.firstState
           << ": begin\n";
    if (pipeline.initializedAtEntry)
        output << "          begin\n";
    else
    {
        output << "          if (" << prefix << "_launch_count == 0) begin\n";
        emitPipelineInitialization(output, pipeline, context, "            ");
        output << "          end else begin\n";
    }

    for (const InstructionSchedule &schedule : blockSchedule.instructions)
    {
        if (!schedule.instruction || schedule.resultWidth == 0 ||
            isBackendLoad(schedule.instruction))
        {
            continue;
        }
        const auto destination = context.valueNames.find(schedule.instruction);
        if (destination == context.valueNames.end() ||
            !pipelineStoresValue(pipeline, schedule.instruction))
            continue;
        const int captureCycle = schedule.startCycle +
            (isIterativeFloating(*schedule.instruction) ? schedule.latency - 1 : 0);
        const std::string slotExpression =
            prefix + "_slot_age[" + std::to_string(captureCycle) + "]";
        std::string resultExpression;
        const auto binding = operationBindings.find(schedule.instruction);
        if (binding != operationBindings.end())
            resultExpression = binding->second->instance + "_out";
        else
            resultExpression = pipelineInstructionExpression(
                *schedule.instruction, slotExpression, context, pipeline, schedule.startCycle);
        if (!pipeline.storageAliases.count(schedule.instruction))
            output << "            if (" << prefix << "_valid["
               << captureCycle << "])\n"
               << "              " << pipelineStorageExpression(
                      context, pipeline, schedule.instruction, slotExpression)
               << " <= " << resultExpression << ";\n";
        for (const ScalarRecurrenceDependence &dependence :
             pipeline.scalarRecurrences)
        {
            if (!dependence.exact ||
                dependence.source != schedule.instruction || !dependence.phi)
            {
                continue;
            }
            const auto phiName = context.valueNames.find(dependence.phi);
            if (phiName == context.valueNames.end())
                continue;
            output << "            if (" << prefix << "_valid["
                   << captureCycle << "])\n"
                   << "              " << pipelineStorageExpression(context, pipeline, dependence.phi,
                        feedbackSlotExpression(slotExpression, dependence.distance, pipeline.contextSlots))
                   << " <= " << resultExpression << ";\n";
        }
    }

    for (const InstructionSchedule &schedule : blockSchedule.instructions)
    {
        const Instruction *load = schedule.instruction;
        if (!isBackendLoad(load) || pipeline.forwardedLoads.count(load))
            continue;
        const auto access = context.memoryAccesses.find(load);
        const auto binding = memoryBindings.find(load);
        if (access == context.memoryAccesses.end() ||
            binding == memoryBindings.end())
        {
            continue;
        }
        const MemoryObject *memory =
            findMemoryObject(context, access->second.memory);
        if (!memory)
            continue;
        const int captureCycle =
            schedule.startCycle + static_cast<int>(memory->readLatency);
        if (captureCycle < 0 ||
            captureCycle > static_cast<int>(pipeline.pipelineLatency))
        {
            continue;
        }
        const auto destination = context.valueNames.find(load);
        if (destination == context.valueNames.end())
            continue;
        const std::string captureSlot =
            prefix + "_slot_age[" + std::to_string(captureCycle) + "]";
        const std::string flatAddress = pipelinePointerOffsetExpression(
            access->second.pointerOperand, *memory, context, pipeline,
            captureSlot, captureCycle);
        const std::string readExpression = partitionedReadExpression(
            *memory, *binding->second, flatAddress);
        if (!pipeline.storageAliases.count(load))
            output << "            if (" << prefix << "_valid[" << captureCycle
               << "])\n"
               << "              " << pipelineStorageExpression(
                      context, pipeline, load, captureSlot) << " <= "
               << readExpression << ";\n";
        for (const ScalarRecurrenceDependence &dependence :
             pipeline.scalarRecurrences)
        {
            if (!dependence.exact || dependence.source != load ||
                !dependence.phi)
            {
                continue;
            }
            const auto phiName = context.valueNames.find(dependence.phi);
            if (phiName == context.valueNames.end())
                continue;
            const std::string sourceSlot = captureSlot;
            output << "            if (" << prefix << "_valid["
                   << captureCycle << "])\n"
                   << "              " << pipelineStorageExpression(context, pipeline, dependence.phi,
                        feedbackSlotExpression(sourceSlot, dependence.distance, pipeline.contextSlots))
                   << " <= " << readExpression << ";\n";
        }
    }

    for (unsigned age = pipeline.pipelineLatency; age > 0; --age)
    {
        output << "            " << prefix << "_valid[" << age << "] <= "
               << prefix << "_valid[" << (age - 1) << "];\n";
        const std::string tag = pipeline.stageIndexedController ? "_iteration_age" : "_slot_age";
        if (!pipeline.stageIndexedController || static_cast<int>(age) <= pipeline.iterationTagLastStage)
            output << "            " << prefix << tag << '[' << age << "] <= "
                   << prefix << tag << '[' << (age - 1) << "];\n";
        if (pipeline.lastTokenController)
            output << "            " << prefix << "_last[" << age << "] <= "
                   << prefix << "_last[" << (age - 1) << "];\n";
    }
    output << "            " << prefix << "_valid[0] <= 1'b0;\n";
    if (pipeline.lastTokenController)
        output << "            " << prefix << "_last[0] <= 1'b0;\n";

    if (pipeline.achievedII > 1)
    {
        output << "            if (" << prefix << "_phase == "
               << (pipeline.achievedII - 1) << ") begin\n"
               << "              " << prefix << "_phase <= 0;\n";
    }
    output << (pipeline.achievedII > 1 ? "              " : "            ")
           << "if (" << prefix << "_launch_count < "
           << pipeline.tripCount << ") begin\n"
           << (pipeline.achievedII > 1 ? "                " : "              ")
           << prefix << "_valid[0] <= 1'b1;\n";
    if (!pipeline.stageIndexedController)
        output << (pipeline.achievedII > 1 ? "                " : "              ")
               << prefix << "_slot_age[0] <= " << prefix << "_next_slot;\n";
    if (pipeline.stageIndexedController)
    {
        if (pipeline.iterationTagLastStage >= 0)
            output << "              " << prefix << "_iteration_age[0] <= "
                   << prefix << "_launch_count;\n";
        if (pipeline.lastTokenController)
            output << "              " << prefix << "_last[0] <= (" << prefix
                   << "_launch_count == " << (pipeline.tripCount - 1) << ");\n";
    }
    else if (!pipeline.derivedValues.empty())
    {
        output << (pipeline.achievedII > 1 ? "                "
                                          : "              ")
               << prefix << "_iteration_ctx[" << prefix
               << "_next_slot] <= " << prefix << "_launch_count;\n";
    }
    for (const PipelinePhiInfo &phiInfo : pipeline.phis)
    {
        const auto destination = context.valueNames.find(phiInfo.phi);
        if (destination == context.valueNames.end() ||
            !pipelineStoresValue(pipeline, phiInfo.phi))
            continue;
        const ScalarRecurrenceDependence *dependence =
            findScalarRecurrence(pipeline, phiInfo.phi);
        if (!phiInfo.affine && dependence && dependence->exact)
            continue;
        output << (pipeline.achievedII > 1 ? "                " : "              ")
               << pipelineStorageExpression(context, pipeline, phiInfo.phi, prefix + "_next_slot")
               << " <= ";
        if (phiInfo.affine)
        {
            output << affinePhiExpression(phiInfo, prefix + "_launch_count",
                                          context, pipeline);
        }
        else
        {
            output << pipelineValueExpression(phiInfo.recurrenceValue,
                                              previousSlotExpression(
                                                  prefix + "_next_slot",
                                                  pipeline.contextSlots),
                                              context, pipeline);
        }
        output << ";\n";
    }
    const std::string launchIndent =
        pipeline.achievedII > 1 ? "                " : "              ";
    if (!pipeline.stageIndexedController)
        output << launchIndent << prefix << "_next_slot <= (" << prefix
               << "_next_slot == " << (pipeline.contextSlots - 1)
               << ") ? 0 : " << prefix << "_next_slot + 1'b1;\n";
    output << launchIndent << prefix << "_launch_count <= " << prefix
           << "_launch_count + 1'b1;\n"
           << (pipeline.achievedII > 1 ? "              end\n"
                                       : "            end\n");
    if (pipeline.achievedII > 1)
    {
        output << "            end else begin\n"
               << "              " << prefix << "_phase <= " << prefix
               << "_phase + 1'b1;\n"
               << "            end\n";
    }

    const unsigned lastSlot =
        (pipeline.tripCount - 1) % pipeline.contextSlots;
    const std::string finalSlot =
        prefix + "_slot_age[" +
        std::to_string(pipeline.pipelineLatency) + "]";
    output << "            if (" << prefix << "_valid["
           << pipeline.pipelineLatency << "] && "
           << (pipeline.lastTokenController ?
                   prefix + "_last[" + std::to_string(pipeline.pipelineLatency) + "]" :
                   prefix + "_launch_count == " + std::to_string(pipeline.tripCount) +
                       " && " + (pipeline.stageIndexedController ?
                           prefix + "_iteration_age[" + std::to_string(pipeline.pipelineLatency) + "]" : finalSlot) +
                       " == " + std::to_string(pipeline.stageIndexedController ? pipeline.tripCount - 1 : lastSlot)) << ") begin\n"
           << "              " << prefix << "_launch_count <= 0;\n";
    const ReturnInst *terminalReturn =
        pipelineExitReturn(pipeline, exitSchedule);
    if (terminalReturn)
    {
        if (terminalReturn->getReturnValue())
            output << "              ap_return <= "
                   << pipelineReturnExpression(*terminalReturn, pipeline,
                                               context, finalSlot)
                   << ";\n";
        output << "              ap_done <= 1'b1;\n"
               << "              ap_idle <= 1'b1;\n"
               << "              state <= STATE_IDLE;\n";
    }
    if (pipeline.header)
    {
        for (const Instruction &instruction : *pipeline.header)
        {
            const unsigned width = integerWidth(instruction.getType());
            const auto name = context.valueNames.find(&instruction);
            if (terminalReturn || !width || name == context.valueNames.end() ||
                !pipeline.liveOutValues.count(&instruction))
                continue;
            output << "              " << name->second << " <= "
                   << pipelineValueExpression(&instruction, finalSlot,
                                              context, pipeline, pipeline.pipelineLatency)
                   << ";\n";
        }
    }
    if (!terminalReturn)
    {
        emitPipelineExitPhiAssignments(output, pipeline, context,
                                       finalSlot, "              ");
        output << "              state <= "
               << (exitSchedule ? exitSchedule->firstState : 0) << ";\n";
    }
    output
           << "            end\n"
           << "          end\n"
           << "        end\n";
}

} // namespace

char HI_RTLGeneration::ID = 0;

HI_RTLGeneration::HI_RTLGeneration(HLSBackendContextPtr context)
    : ModulePass(ID), context(std::move(context))
{
}

bool HI_RTLGeneration::runOnModule(Module &module)
{
    if (!context || context->hasErrors() || !context->schedule.valid ||
        !context->binding.valid || !context->top)
    {
        return false;
    }

    context->module = &module;
    context->rtl = RTLDesign();
    context->rtl.moduleName = sanitizeVerilogIdentifier(
        context->configuration.rtlModuleName.empty()
            ? context->configuration.topFunction
            : context->configuration.rtlModuleName,
        "light_hls_top");
    context->rtl.interfaces = {RTLInterfaceProtocol::APControlHS,
                               RTLInterfaceProtocol::Scalar};
    const bool hasExternalMemory = std::any_of(
        context->memoryObjects.begin(), context->memoryObjects.end(),
        [](const MemoryObject &memory) {
            return memory.storage == MemoryStorageKind::ExternalAPMemory;
        });
    const bool hasLocalMemory = std::any_of(
        context->memoryObjects.begin(), context->memoryObjects.end(),
        [](const MemoryObject &memory) {
            return memory.storage == MemoryStorageKind::LocalRAM;
        });
    if (hasExternalMemory)
        context->rtl.interfaces.push_back(RTLInterfaceProtocol::APMemory);
    if (hasLocalMemory)
        context->rtl.interfaces.push_back(RTLInterfaceProtocol::BRAM);

    std::map<const Instruction *, const OperationBinding *> operationBindings;
    std::map<std::string, const Instruction *> representativeInstructions;
    for (const OperationBinding &binding : context->binding.operations)
    {
        operationBindings[binding.instruction] = &binding;
        if (!representativeInstructions.count(binding.instance))
            representativeInstructions[binding.instance] = binding.instruction;
    }
    std::set<std::string> directOperators;
    std::map<std::string, std::map<unsigned, const Value *>> invariantOperands;
    for (const OperatorInstance &instance : context->binding.operators)
    {
        invariantOperands[instance.name] = instance.invariantOperands;
        if (instance.operations.size() == 1)
            directOperators.insert(instance.name);
    }
    std::map<const Instruction *, const MemoryAccessBinding *> memoryBindings;
    for (const MemoryAccessBinding &binding : context->binding.memoryAccesses)
        memoryBindings[binding.instruction] = &binding;

    Function &top = *context->top;
    std::map<const Argument *, std::string> inputPorts;
    std::set<std::string> usedPorts = {
        "ap_clk", "ap_rst", "ap_ce", "ap_start", "ap_done", "ap_idle",
        "ap_ready", "ap_return"};
    unsigned argumentIndex = 0;
    for (const Argument &argument : top.args())
    {
        if (!scalarBitWidth(argument.getType()))
        {
            ++argumentIndex;
            continue;
        }
        const std::string sourceName = argument.hasName()
                                           ? argument.getName().str()
                                           : "arg" + std::to_string(argumentIndex);
        std::string port = sanitizeVerilogPortIdentifier(
            sourceName, "arg" + std::to_string(argumentIndex));
        if (!usedPorts.insert(port).second)
        {
            port += "_in";
            unsigned suffix = 0;
            while (!usedPorts.insert(port).second)
                port += "_" + std::to_string(++suffix);
        }
        inputPorts[&argument] = port;
        ++argumentIndex;
    }

    for (MemoryObject &memory : context->memoryObjects)
    {
        const std::string base =
            memory.storage == MemoryStorageKind::ExternalAPMemory
                ? externalMemoryBaseName(memory)
                : sanitizeVerilogIdentifier(memory.name, "local_memory");
        std::string candidate = base;
        unsigned suffix = 0;
        auto collides = [&](const std::string &name) {
            for (unsigned bank = 0; bank < memory.banks; ++bank)
            {
                if (memoryUsesScalarBanks(memory))
                {
                    bool hasRead = false;
                    bool hasWrite = false;
                    for (unsigned port = 0; port < memory.ports; ++port)
                    {
                        hasRead |= memoryBankPortHasRead(memory, bank, port);
                        hasWrite |= memoryBankPortHasWrite(memory, bank, port);
                    }
                    const std::string scalar =
                        name + "_" + std::to_string(bank);
                    if ((hasRead && !hasWrite && usedPorts.count(scalar)) ||
                        (hasWrite && !hasRead &&
                         (usedPorts.count(scalar) ||
                          usedPorts.count(scalar + "_ap_vld"))) ||
                        (hasRead && hasWrite &&
                         (usedPorts.count(scalar + "_i") ||
                          usedPorts.count(scalar + "_o") ||
                          usedPorts.count(scalar + "_o_ap_vld"))))
                    {
                        return true;
                    }
                    continue;
                }
                const std::string prefix =
                    name + (memory.banks > 1
                                ? "_" + std::to_string(bank) + "_"
                                : "_");
                for (unsigned port = 0; port < memory.ports; ++port)
                {
                    if (!memoryBankPortIsUsed(memory, bank, port))
                        continue;
                    const std::string ordinal = std::to_string(port);
                    if (usedPorts.count(prefix + "address" + ordinal) ||
                        usedPorts.count(prefix + "ce" + ordinal) ||
                        (memoryBankPortHasWrite(memory, bank, port) &&
                         (usedPorts.count(prefix + "we" + ordinal) ||
                          usedPorts.count(prefix + "d" + ordinal))) ||
                        (memoryBankPortHasRead(memory, bank, port) &&
                         usedPorts.count(prefix + "q" + ordinal)))
                    {
                        return true;
                    }
                }
            }
            return false;
        };
        while (collides(candidate))
            candidate = base + "_mem" + std::to_string(++suffix);
        memory.portName = candidate;
        for (unsigned bank = 0; bank < memory.banks; ++bank)
        {
            if (memoryUsesScalarBanks(memory))
            {
                bool hasRead = false;
                bool hasWrite = false;
                for (unsigned port = 0; port < memory.ports; ++port)
                {
                    hasRead |= memoryBankPortHasRead(memory, bank, port);
                    hasWrite |= memoryBankPortHasWrite(memory, bank, port);
                }
                const std::string scalar =
                    memoryScalarBankName(memory, bank);
                if (hasRead && !hasWrite)
                    usedPorts.insert(scalar);
                else if (hasWrite && !hasRead)
                {
                    usedPorts.insert(scalar);
                    usedPorts.insert(scalar + "_ap_vld");
                }
                else if (hasRead && hasWrite)
                {
                    usedPorts.insert(scalar + "_i");
                    usedPorts.insert(scalar + "_o");
                    usedPorts.insert(scalar + "_o_ap_vld");
                }
                continue;
            }
            const std::string prefix = memoryBankPrefix(memory, bank);
            for (unsigned port = 0; port < memory.ports; ++port)
            {
                if (!memoryBankPortIsUsed(memory, bank, port))
                    continue;
                const std::string ordinal = std::to_string(port);
                usedPorts.insert(prefix + "address" + ordinal);
                usedPorts.insert(prefix + "ce" + ordinal);
                if (memoryBankPortHasWrite(memory, bank, port))
                {
                    usedPorts.insert(prefix + "we" + ordinal);
                    usedPorts.insert(prefix + "d" + ordinal);
                }
                if (memoryBankPortHasRead(memory, bank, port))
                    usedPorts.insert(prefix + "q" + ordinal);
            }
        }
    }
    context->rtl.memories = context->memoryObjects;
    context->rtl.memoryBindings = context->binding.memoryAccesses;
    context->rtl.pipelines = context->pipelineRegions;

    std::vector<std::string> ports = {
        "input wire ap_clk", "input wire ap_rst"};
    if (context->configuration.rtlClockEnable)
        ports.push_back("input wire ap_ce");
    ports.insert(ports.end(),
                 {"input wire ap_start", "output reg ap_done",
                  "output reg ap_idle", "output wire ap_ready"});
    for (const Argument &argument : top.args())
    {
        if (scalarBitWidth(argument.getType()))
        {
            ports.push_back("input wire " +
                            rangeForWidth(integerWidth(argument.getType())) +
                            inputPorts.at(&argument));
            continue;
        }
        const MemoryObject *memory = findMemoryObject(*context, &argument);
        if (!memory ||
            memory->storage != MemoryStorageKind::ExternalAPMemory)
            continue;
        for (unsigned bank = 0; bank < memory->banks; ++bank)
        {
            if (memoryUsesScalarBanks(*memory))
            {
                bool hasRead = false;
                bool hasWrite = false;
                for (unsigned port = 0; port < memory->ports; ++port)
                {
                    hasRead |=
                        memoryBankPortHasRead(*memory, bank, port);
                    hasWrite |=
                        memoryBankPortHasWrite(*memory, bank, port);
                }
                const std::string scalar =
                    memoryScalarBankName(*memory, bank);
                if (hasRead && !hasWrite)
                {
                    ports.push_back("input wire " +
                                    rangeForWidth(memory->elementWidth) +
                                    scalar);
                }
                else if (hasWrite && !hasRead)
                {
                    ports.push_back("output reg " +
                                    rangeForWidth(memory->elementWidth) +
                                    scalar);
                    ports.push_back("output reg " + scalar + "_ap_vld");
                }
                else if (hasRead && hasWrite)
                {
                    ports.push_back("input wire " +
                                    rangeForWidth(memory->elementWidth) +
                                    scalar + "_i");
                    ports.push_back("output reg " +
                                    rangeForWidth(memory->elementWidth) +
                                    scalar + "_o");
                    ports.push_back("output reg " + scalar + "_o_ap_vld");
                }
                continue;
            }
            const std::string prefix = memoryBankPrefix(*memory, bank);
            for (unsigned port = 0; port < memory->ports; ++port)
            {
                if (!memoryBankPortIsUsed(*memory, bank, port))
                    continue;
                const std::string ordinal = std::to_string(port);
                ports.push_back(
                    "output reg " + rangeForWidth(memory->bankAddressWidth) +
                    prefix + "address" + ordinal);
                ports.push_back("output reg " + prefix + "ce" + ordinal);
                if (memoryBankPortHasWrite(*memory, bank, port))
                {
                    ports.push_back("output reg " + prefix + "we" + ordinal);
                    ports.push_back(std::string(memoryBankDirectWriteData(*context, *memory, bank, port) ?
                                        "output wire " : "output reg ") +
                                    rangeForWidth(memory->elementWidth) + prefix +
                                    "d" + ordinal);
                }
                if (memoryBankPortHasRead(*memory, bank, port))
                {
                    ports.push_back("input wire " +
                                    rangeForWidth(memory->elementWidth) + prefix +
                                    "q" + ordinal);
                }
            }
        }
    }
    const unsigned returnWidth = integerWidth(top.getReturnType());
    if (returnWidth)
        ports.push_back("output reg " + rangeForWidth(returnWidth) + "ap_return");

    // Render payload expressions only after final interface names are resolved.
    std::map<std::string, std::string> directWriteData;
    for (const BlockSchedule &block : context->schedule.blocks)
    {
        const LoopPipelineInfo *pipeline = findPipeline(*context, block.block);
        for (const InstructionSchedule &schedule : block.instructions)
        {
            const auto binding = memoryBindings.find(schedule.instruction);
            if (binding == memoryBindings.end() || binding->second->directWriteDataBanks.empty())
                continue;
            const MemoryAccessInfo &access = context->memoryAccesses.at(schedule.instruction);
            const MemoryObject *memory = findMemoryObject(*context, access.memoryBase);
            const std::string data = pipeline ?
                pipelineValueExpression(access.writeValue,
                    pipelinePrefix(*pipeline) + "_slot_age[" + std::to_string(schedule.startCycle) + "]",
                    *context, *pipeline, schedule.startCycle) :
                valueExpression(access.writeValue, *context);
            for (unsigned bank : binding->second->directWriteDataBanks)
                directWriteData[memoryBankPrefix(*memory, bank) + "d" +
                    std::to_string(binding->second->port)] = data;
        }
    }

    std::ostringstream output;
    output << "`timescale 1ns/1ps\n"
           << "// Generated by the Light-HLS experimental RTL backend.\n"
           << "// Scalar integer/IEEE FP16/FP32/FP64 datapaths, local RAM, and direction-aware ap_memory interfaces.\n"
           << "module " << context->rtl.moduleName << " (\n";
    for (std::size_t index = 0; index < ports.size(); ++index)
    {
        output << "  " << ports[index];
        if (index + 1 != ports.size())
            output << ',';
        output << '\n';
    }
    output << ");\n\n";

    const unsigned fsmWidth = stateWidth(context->schedule.stateCount);
    output << "  localparam " << rangeForWidth(fsmWidth)
           << "STATE_IDLE = " << fsmWidth << "'d0;\n"
           << "  reg " << rangeForWidth(fsmWidth) << "state;\n"
           << "  // Scalar capture and request acknowledgement share one enabled edge.\n"
           << "  assign ap_ready = !ap_rst && (state == STATE_IDLE)";
    if (context->configuration.rtlClockEnable)
        output << " && ap_ce";
    output << ";\n";

    for (const MemoryObject &memory : context->memoryObjects)
    {
        if (memory.storage != MemoryStorageKind::LocalRAM)
            continue;
        for (unsigned bank = 0; bank < memory.banks; ++bank)
        {
            const std::string prefix = memoryBankPrefix(memory, bank);
            output << "  " << memoryStyleAttribute(memory) << "reg "
                   << rangeForWidth(memory.elementWidth)
                   << prefix << "storage [0:" << (memory.bankDepth - 1)
                   << "];\n";
            if (!memory.initialWords.empty())
            {
                output << "  initial begin\n";
                for (unsigned long long index = 0; index < memory.initialWords.size(); ++index)
                    if (partitionBankForAddress(memory, index) == bank)
                        output << "    " << prefix << "storage["
                               << partitionLocalAddressForAddress(memory, index) << "] = "
                               << memory.elementWidth << "'h" << memory.initialWords[index] << ";\n";
                output << "  end\n";
            }
            for (unsigned port = 0; port < memory.ports; ++port)
            {
                const std::string ordinal = std::to_string(port);
                output << "  reg " << rangeForWidth(memory.bankAddressWidth)
                       << prefix << "address" << ordinal << ";\n"
                       << "  reg " << prefix << "ce" << ordinal << ";\n"
                       << "  reg " << prefix << "we" << ordinal << ";\n"
                       << (directWriteData.count(prefix + "d" + ordinal) ? "  wire " : "  reg ")
                       << rangeForWidth(memory.elementWidth)
                       << prefix << "d" << ordinal << ";\n"
                       << "  reg " << rangeForWidth(memory.elementWidth)
                       << prefix << "q" << ordinal << ";\n";
                for (unsigned stage = 0;
                     stage + 1 < memory.readLatency; ++stage)
                {
                    output << "  reg "
                           << rangeForWidth(memory.bankAddressWidth) << prefix
                           << "read_address_pipe" << ordinal << "_" << stage
                           << ";\n";
                }
            }
        }
    }

    for (const Argument &argument : top.args())
    {
        if (!scalarBitWidth(argument.getType()))
            continue;
        output << "  reg " << rangeForWidth(integerWidth(argument.getType()))
               << context->valueNames.at(&argument) << ";\n";
    }
    std::map<const Value *, std::string> entryExpressions;
    if (context->ordinaryEntrySuccessor)
    {
        for (const auto &port : inputPorts)
            entryExpressions[port.first] = port.second;
        for (const Instruction &instruction : top.getEntryBlock())
        {
            if (!scalarBitWidth(instruction.getType()) || context->constantValues.count(&instruction))
                continue;
            std::vector<std::string> operands;
            for (const Use &operand : instruction.operands())
            {
                const auto incoming = entryExpressions.find(operand.get());
                operands.push_back(incoming == entryExpressions.end() ?
                    valueExpression(operand.get(), *context) : incoming->second);
            }
            const std::string name = "entry_" + context->valueNames.at(&instruction);
            entryExpressions[&instruction] = name;
            output << "  wire " << rangeForWidth(integerWidth(instruction.getType())) << name
                   << " = " << boundInstructionExpression(instruction, operands) << ";\n";
        }
    }
    for (const BasicBlock &block : top)
    {
        for (const Instruction &instruction : block)
        {
            auto name = context->valueNames.find(&instruction);
            if (name == context->valueNames.end() ||
                !needsGlobalRTLRegister(*context, &instruction))
                continue;
            output << "  reg " << rangeForWidth(integerWidth(instruction.getType()))
                   << name->second << ";\n";
        }
    }
    for (const BasicBlock &block : top)
    {
        if (findPipeline(*context, &block))
            continue;
        for (const Instruction &instruction : block)
        {
            if (context->constantValues.count(&instruction))
                continue;
            if (!context->combinationalValues.count(&instruction) &&
                !context->forwardedLoads.count(&instruction))
                continue;
            const auto name = context->valueNames.find(&instruction);
            if (name != context->valueNames.end())
            {
                std::string expression = instructionExpression(instruction, *context);
                if (context->forwardedLoads.count(&instruction))
                {
                    const MemoryAccessInfo &access = context->memoryAccesses.at(&instruction);
                    const MemoryObject *memory = findMemoryObject(*context, access.memory);
                    expression = partitionedReadExpression(*memory, *memoryBindings.at(&instruction),
                        memoryUsesScalarBanks(*memory) ? pointerOffsetExpression(
                            access.pointerOperand, *memory, *context) : "0");
                }
                output << "  wire " << rangeForWidth(integerWidth(instruction.getType()))
                       << name->second << " = "
                       << expression << ";\n";
            }
        }
    }
    for (const LoopPipelineInfo &pipeline : context->pipelineRegions)
    {
        const std::string prefix = pipelinePrefix(pipeline);
        const unsigned slotWidth = countWidth(pipeline.contextSlots - 1);
        const unsigned launchWidth = countWidth(pipeline.tripCount);
        const unsigned iterationWidth = countWidth(pipeline.tripCount - 1);
        const unsigned phaseWidth = countWidth(
            static_cast<unsigned>(std::max(1, pipeline.achievedII) - 1));
        output << "  // Pipeline controller for " << pipeline.label << ".\n"
               << "  reg [" << pipeline.pipelineLatency << ":0] " << prefix
               << "_valid;\n";
        if (!pipeline.stageIndexedController)
            output << "  reg " << rangeForWidth(slotWidth) << prefix
                   << "_slot_age [0:" << pipeline.pipelineLatency << "];\n"
                   << "  reg " << rangeForWidth(slotWidth) << prefix << "_next_slot;\n";
        output
               << "  reg " << rangeForWidth(launchWidth) << prefix
               << "_launch_count;\n";
        if (pipeline.stageIndexedController)
        {
            if (pipeline.iterationTagLastStage >= 0)
                output << "  reg " << rangeForWidth(iterationWidth) << prefix
                       << "_iteration_age [0:" << pipeline.iterationTagLastStage << "];\n";
            if (pipeline.lastTokenController)
                output << "  reg [" << pipeline.pipelineLatency << ":0] " << prefix << "_last;\n";
        }
        else if (!pipeline.derivedValues.empty())
            output << "  reg " << rangeForWidth(iterationWidth) << prefix
                   << "_iteration_ctx [0:" << (pipeline.contextSlots - 1)
                   << "];\n";
        if (pipeline.achievedII > 1)
            output << "  reg " << rangeForWidth(phaseWidth) << prefix
                   << "_phase;\n";
        if (pipeline.header)
        {
            for (const Instruction &instruction : *pipeline.header)
            {
                const unsigned width = integerWidth(instruction.getType());
                const auto name = context->valueNames.find(&instruction);
                if (!width || name == context->valueNames.end() ||
                    !pipelineStoresValue(pipeline, &instruction) ||
                    pipeline.storageAliases.count(&instruction))
                    continue;
                const unsigned slots = pipelineValueStorageSlots(pipeline, &instruction);
                output << "  reg " << rangeForWidth(width) << name->second;
                if (slots == 1)
                    output << "_stage;\n";
                else
                    output << "_ctx [0:" << (slots - 1) << "];\n";
            }
        }
    }
    for (const OperatorInstance &instance : context->binding.operators)
    {
        const Instruction *instruction = representativeInstructions.at(instance.name);
        const bool direct = directOperators.count(instance.name);
        std::vector<const Value *> integerOperands;
        for (const Value *operand : instruction->operand_values())
            if (scalarBitWidth(operand->getType()))
                integerOperands.push_back(operand);
        for (std::size_t operandIndex = 0;
             operandIndex < instance.operandWidths.size(); ++operandIndex)
        {
            const auto invariant = instance.invariantOperands.find(operandIndex);
            output << (direct || invariant != instance.invariantOperands.end() ? "  wire " : "  reg ")
                   << rangeForWidth(instance.operandWidths[operandIndex])
                   << instance.name << "_in" << operandIndex;
            if (direct)
            {
                const LoopPipelineInfo *pipeline = findPipeline(*context, instruction->getParent());
                const int cycle = context->schedule.instructionMap.at(instruction)->startCycle;
                output << " = " << (pipeline ?
                    pipelineValueExpression(integerOperands.at(operandIndex),
                        pipelinePrefix(*pipeline) + "_slot_age[" + std::to_string(cycle) + "]",
                        *context, *pipeline, cycle) :
                    valueExpression(integerOperands.at(operandIndex), *context));
            }
            else if (invariant != instance.invariantOperands.end())
                output << " = " << valueExpression(invariant->second, *context);
            output << ";\n";
        }
        // A final-edge return and recurrence register sample the same FU.
        // Preserve this bound result net so synthesis does not clone its
        // arithmetic cone for their different register enables.
        const bool sharedRetirementResult = direct && std::any_of(
            context->pipelineRegions.begin(), context->pipelineRegions.end(),
            [&](const LoopPipelineInfo &pipeline) {
                return pipeline.forwardedReturnValue == instruction &&
                    std::any_of(pipeline.scalarRecurrences.begin(), pipeline.scalarRecurrences.end(),
                        [&](const ScalarRecurrenceDependence &dependence) {
                            return dependence.exact && dependence.source == instruction;
                        });
            });
        output << (sharedRetirementResult ? "  (* keep = \"true\" *) wire " : "  wire ")
               << rangeForWidth(instance.bitWidth) << instance.name
               << "_out;\n";
        std::vector<std::string> operandNames;
        for (std::size_t operandIndex = 0;
             operandIndex < instance.operandWidths.size(); ++operandIndex)
        {
            operandNames.push_back(instance.name + "_in" +
                                   std::to_string(operandIndex));
        }
        const InstructionSchedule *fpSchedule = context->schedule.instructionMap.at(instruction);
        unsigned fpIssueState = 0;
        for (const BlockSchedule &block : context->schedule.blocks)
            if (block.block == instruction->getParent())
                fpIssueState = block.firstState + fpSchedule->startCycle;
        std::string fpStart = "(state == " + std::to_string(fpIssueState) + ")";
        if (const LoopPipelineInfo *pipeline = findPipeline(*context, instruction->getParent()))
        {
            const BlockSchedule *block = findBlockSchedule(*context, instruction->getParent());
            fpStart = "(state == " + std::to_string(block->firstState) + " && " +
                pipelinePrefix(*pipeline) + "_valid[" + std::to_string(fpSchedule->startCycle) + "])";
        }
        if (emitFloatingOperator(output, *instruction, instance.name, fpStart,
                context->configuration.rtlClockEnable ? "ap_ce" : "1'b1"))
            continue;
        output << "  assign " << instance.name << "_out = "
               << boundInstructionExpression(
                      *representativeInstructions.at(instance.name), operandNames, false)
               << ";\n";
    }
    output << '\n';

    if (!directWriteData.empty())
    {
        output << "  // Single-writer local payloads; CE/WE still guard every write edge.\n";
        for (const auto &entry : directWriteData)
            output << "  assign " << entry.first << " = " << entry.second << ";\n";
        output << '\n';
    }

    for (const MemoryObject &memory : context->memoryObjects)
    {
        if (memory.storage != MemoryStorageKind::LocalRAM)
            continue;
        for (unsigned bank = 0; bank < memory.banks; ++bank)
        {
            const std::string prefix = memoryBankPrefix(memory, bank);
            output << "  // Synchronous local RAM bank " << bank
                   << " for MemoryObject " << memory.name << ".\n"
                   << "  always @(posedge ap_clk) begin\n";
            if (context->configuration.rtlClockEnable)
                output << "    if (ap_ce) begin\n";
            const std::string ramIndent =
                context->configuration.rtlClockEnable ? "      " : "    ";
            for (unsigned port = 0; port < memory.ports; ++port)
            {
                const std::string ordinal = std::to_string(port);
                if (memory.readLatency > 1)
                {
                    output << ramIndent << prefix << "q" << ordinal << " <= "
                           << prefix << "storage[" << prefix
                           << "read_address_pipe" << ordinal << "_"
                           << (memory.readLatency - 2) << "];\n";
                    for (unsigned stage = memory.readLatency - 1; stage > 1;
                         --stage)
                    {
                        output << ramIndent << prefix << "read_address_pipe"
                               << ordinal << "_" << (stage - 1) << " <= "
                               << prefix << "read_address_pipe" << ordinal
                               << "_" << (stage - 2) << ";\n";
                    }
                }
                output << ramIndent << "if (" << prefix << "ce" << ordinal
                       << ") begin\n"
                       << ramIndent << "  if (" << prefix << "we" << ordinal
                       << ")\n"
                       << ramIndent << "    " << prefix << "storage[" << prefix
                       << "address" << ordinal << "] <= " << prefix << "d"
                       << ordinal << ";\n"
                       << ramIndent << "  else\n";
                if (memory.readLatency == 1)
                {
                    output << ramIndent << "    " << prefix << "q" << ordinal
                           << " <= "
                           << prefix << "storage[" << prefix << "address"
                           << ordinal << "];\n";
                }
                else
                {
                    output << ramIndent << "    " << prefix
                           << "read_address_pipe"
                           << ordinal << "_0 <= " << prefix << "address"
                           << ordinal << ";\n";
                }
                output << ramIndent << "end\n";
            }
            if (context->configuration.rtlClockEnable)
                output << "    end\n";
            output << "  end\n\n";
        }
    }

    const BlockSchedule *entrySchedule =
        findBlockSchedule(*context, &top.getEntryBlock());

    if (context->binding.operators.size() > directOperators.size())
    {
        std::map<unsigned, std::vector<const OperationBinding *>> bindingsByState;
        for (const BlockSchedule &blockSchedule : context->schedule.blocks)
        {
            if (blockSchedule.pipelined)
                continue;
            for (const InstructionSchedule &schedule : blockSchedule.instructions)
            {
                auto binding = operationBindings.find(schedule.instruction);
                if (binding == operationBindings.end() || directOperators.count(binding->second->instance))
                    continue;
                const unsigned state =
                    blockSchedule.firstState + schedule.startCycle;
                bindingsByState[state].push_back(binding->second);
            }
        }

        output << "  // Operand multiplexers selected by the scheduled FSM state.\n"
               << "  always @* begin\n";
        for (const OperatorInstance &instance : context->binding.operators)
        {
            if (directOperators.count(instance.name))
                continue;
            for (std::size_t operandIndex = 0;
                 operandIndex < instance.operandWidths.size(); ++operandIndex)
            {
                if (instance.invariantOperands.count(operandIndex))
                    continue;
                output << "    " << instance.name << "_in" << operandIndex << " = "
                       << instance.operandWidths[operandIndex] << "'d0;\n";
            }
        }
        output << "    case (state)\n";
        for (const auto &stateEntry : bindingsByState)
        {
            output << "      " << fsmWidth << "'d" << stateEntry.first
                   << ": begin\n";
            for (const OperationBinding *binding : stateEntry.second)
            {
                const Instruction &instruction = *binding->instruction;
                std::size_t operandIndex = 0;
                for (const Use &operandUse : instruction.operands())
                {
                    if (!scalarBitWidth(operandUse->getType()))
                        continue;
                    if (invariantOperands.at(binding->instance).count(operandIndex))
                    {
                        ++operandIndex;
                        continue;
                    }
                    output << "        " << binding->instance << "_in"
                           << operandIndex << " = "
                           << valueExpression(operandUse.get(), *context) << ";\n";
                    ++operandIndex;
                }
            }
            output << "      end\n";
        }
        output << "      default: begin end\n"
               << "    endcase\n";
        for (const BlockSchedule &blockSchedule : context->schedule.blocks)
        {
            const LoopPipelineInfo *pipeline =
                findPipeline(*context, blockSchedule.block);
            if (!pipeline)
                continue;
            const std::string prefix = pipelinePrefix(*pipeline);
            for (const InstructionSchedule &schedule :
                 blockSchedule.instructions)
            {
                const auto binding = operationBindings.find(schedule.instruction);
                if (binding == operationBindings.end() || directOperators.count(binding->second->instance))
                    continue;
                output << "    if (state == " << fsmWidth << "'d"
                       << blockSchedule.firstState << " && " << prefix
                       << "_valid[" << schedule.startCycle << "]) begin\n";
                std::size_t operandIndex = 0;
                for (const Use &operandUse : schedule.instruction->operands())
                {
                    if (!scalarBitWidth(operandUse->getType()))
                        continue;
                    if (invariantOperands.at(binding->second->instance).count(operandIndex))
                    {
                        ++operandIndex;
                        continue;
                    }
                    output << "      " << binding->second->instance << "_in"
                           << operandIndex << " = "
                           << pipelineValueExpression(
                                  operandUse.get(),
                                  prefix + "_slot_age[" +
                                      std::to_string(schedule.startCycle) + "]",
                                  *context, *pipeline, schedule.startCycle)
                           << ";\n";
                    ++operandIndex;
                }
                output << "    end\n";
            }
        }
        output << "  end\n\n";
    }

    if (!context->memoryObjects.empty())
    {
        std::map<unsigned, std::vector<const InstructionSchedule *>>
            memoryAccessesByState;
        for (const BlockSchedule &blockSchedule : context->schedule.blocks)
        {
            if (blockSchedule.pipelined)
                continue;
            for (const InstructionSchedule &schedule : blockSchedule.instructions)
            {
                if (!context->memoryAccesses.count(schedule.instruction))
                    continue;
                const unsigned state =
                    blockSchedule.firstState + schedule.startCycle;
                memoryAccessesByState[state].push_back(&schedule);
            }
        }

        output << "  // Memory-port controls selected by the scheduled FSM state.\n"
               << "  always @* begin\n";
        for (const MemoryObject &memory : context->memoryObjects)
        {
            for (unsigned bank = 0; bank < memory.banks; ++bank)
            {
                if (memoryUsesScalarBanks(memory))
                {
                    bool hasRead = false;
                    bool hasWrite = false;
                    for (unsigned port = 0; port < memory.ports; ++port)
                    {
                        hasRead |=
                            memoryBankPortHasRead(memory, bank, port);
                        hasWrite |=
                            memoryBankPortHasWrite(memory, bank, port);
                    }
                    if (hasWrite)
                    {
                        const std::string scalar =
                            memoryScalarBankName(memory, bank);
                        const std::string outputName =
                            hasRead ? scalar + "_o" : scalar;
                        output << "    " << outputName << " = "
                               << memory.elementWidth << "'d0;\n"
                               << "    " << outputName
                               << "_ap_vld = 1'b0;\n";
                    }
                    continue;
                }
                const std::string prefix = memoryBankPrefix(memory, bank);
                for (unsigned port = 0; port < memory.ports; ++port)
                {
                    if (!memoryBankPortIsUsed(memory, bank, port))
                        continue;
                    const std::string ordinal = std::to_string(port);
                    output << "    " << prefix << "address" << ordinal
                           << " = " << memory.bankAddressWidth << "'d0;\n"
                           << "    " << prefix << "ce" << ordinal
                           << " = 1'b0;\n";
                    if (memoryBankPortHasWrite(memory, bank, port))
                    {
                        output << "    " << prefix << "we" << ordinal
                               << " = 1'b0;\n";
                        if (!directWriteData.count(prefix + "d" + ordinal))
                            output << "    " << prefix << "d" << ordinal << " = "
                                   << memory.elementWidth << "'d0;\n";
                    }
                }
            }
        }
        // Suppress memory effects during reset. DATAFLOW children expose raw
        // requests here: the parent derives ap_ce from them and gates commits.
        // Gating these requests by ap_ce would create a combinational loop.
        output << "    if (!ap_rst) begin\n"
               << "    case (state)\n";
        for (const auto &stateEntry : memoryAccessesByState)
        {
            output << "      " << fsmWidth << "'d" << stateEntry.first
                   << ": begin\n";
            for (const InstructionSchedule *schedule : stateEntry.second)
            {
                const MemoryAccessInfo &access =
                    context->memoryAccesses.at(schedule->instruction);
                const MemoryObject *memory =
                    findMemoryObject(*context, access.memoryBase);
                if (!memory)
                    continue;
                auto binding = memoryBindings.find(schedule->instruction);
                if (binding == memoryBindings.end())
                    continue;
                const std::string flatAddress = pointerOffsetExpression(
                    access.pointerOperand, *memory, *context);
                emitPartitionedMemoryControl(
                    output, "        ", *memory, *binding->second,
                    flatAddress, access.isWrite,
                    access.isWrite
                        ? valueExpression(access.writeValue, *context)
                        : std::string());
            }
            output << "      end\n";
        }
        output << "      default: begin end\n"
               << "    endcase\n";
        for (const BlockSchedule &blockSchedule : context->schedule.blocks)
        {
            const LoopPipelineInfo *pipeline =
                findPipeline(*context, blockSchedule.block);
            if (!pipeline)
                continue;
            const std::string pipelineName = pipelinePrefix(*pipeline);
            for (const InstructionSchedule &schedule :
                 blockSchedule.instructions)
            {
                const auto access =
                    context->memoryAccesses.find(schedule.instruction);
                if (access == context->memoryAccesses.end())
                    continue;
                const MemoryObject *memory =
                    findMemoryObject(*context, access->second.memory);
                const auto binding = memoryBindings.find(schedule.instruction);
                if (!memory || binding == memoryBindings.end())
                    continue;
                const std::string slotExpression =
                    pipelineName + "_slot_age[" +
                    std::to_string(schedule.startCycle) + "]";
                const Value *predicate = backendMemoryPredicate(schedule.instruction);
                output << "    if (state == " << fsmWidth << "'d"
                       << blockSchedule.firstState << " && " << pipelineName
                       << "_valid[" << schedule.startCycle << "]";
                if (predicate)
                    output << " && (" << pipelineValueExpression(predicate, slotExpression,
                        *context, *pipeline, schedule.startCycle) << ")";
                output << ") begin\n";
                const std::string flatAddress =
                    pipelinePointerOffsetExpression(
                        access->second.pointerOperand, *memory, *context,
                        *pipeline, slotExpression, schedule.startCycle);
                emitPartitionedMemoryControl(
                    output, "      ", *memory, *binding->second, flatAddress,
                    access->second.isWrite,
                    access->second.isWrite
                        ? pipelineValueExpression(
                              access->second.writeValue, slotExpression,
                              *context, *pipeline, schedule.startCycle)
                        : std::string());
                output << "    end\n";
            }
        }
        output << "    end\n"
               << "  end\n\n";
    }

    output << "  always @(posedge ap_clk) begin\n"
           << "    if (ap_rst) begin\n"
           << "      state <= STATE_IDLE;\n"
           << "      ap_done <= 1'b0;\n"
           << "      ap_idle <= 1'b1;\n";
    if (returnWidth)
        output << "      ap_return <= " << returnWidth << "'d0;\n";
    // Use RTL names rather than allocation addresses for reproducible output.
    std::map<std::string, const Value *> resetValues;
    for (const auto &valueName : context->valueNames)
        resetValues[valueName.second] = valueName.first;
    for (const auto &valueName : resetValues)
    {
        const unsigned width = integerWidth(valueName.second->getType());
        if (width && needsGlobalRTLRegister(*context, valueName.second))
            output << "      " << valueName.first << " <= " << width << "'d0;\n";
    }
    for (const LoopPipelineInfo &pipeline : context->pipelineRegions)
    {
        const std::string prefix = pipelinePrefix(pipeline);
        output << "      " << prefix << "_valid <= "
               << (pipeline.pipelineLatency + 1) << "'d0;\n"
               << "      " << prefix << "_launch_count <= 0;\n";
        if (!pipeline.stageIndexedController)
            output << "      " << prefix << "_next_slot <= 0;\n";
        if (pipeline.lastTokenController)
            output << "      " << prefix << "_last <= 0;\n";
        if (pipeline.achievedII > 1)
            output << "      " << prefix << "_phase <= 0;\n";
    }
    output << "    end else";
    if (context->configuration.rtlClockEnable)
        output << " if (ap_ce)";
    output << " begin\n"
           << "      ap_done <= 1'b0;\n"
           << "      case (state)\n"
           << "        STATE_IDLE: begin\n"
           << "          ap_idle <= 1'b1;\n"
           << "          if (ap_start) begin\n"
           << "            ap_idle <= 1'b0;\n";
    for (const Argument &argument : top.args())
    {
        if (!scalarBitWidth(argument.getType()))
            continue;
        output << "            " << context->valueNames.at(&argument) << " <= "
               << inputPorts.at(&argument) << ";\n";
    }
    for (const LoopPipelineInfo &pipeline : context->pipelineRegions)
        if (pipeline.initializedAtEntry)
            emitPipelineInitialization(output, pipeline, *context, "            ", &inputPorts);
    if (context->ordinaryEntrySuccessor)
        for (const PHINode &phi : context->ordinaryEntrySuccessor->phis())
        {
            const Value *incoming = phi.getIncomingValueForBlock(&top.getEntryBlock());
            const auto expression = entryExpressions.find(incoming);
            output << "            " << context->valueNames.at(&phi) << " <= "
                   << (expression == entryExpressions.end() ? valueExpression(incoming, *context) : expression->second)
                   << ";\n";
        }
    output << "            state <= " << entrySchedule->firstState << ";\n"
           << "          end\n"
           << "        end\n";

    for (const BlockSchedule &blockSchedule : context->schedule.blocks)
    {
        if (const LoopPipelineInfo *pipeline =
                findPipeline(*context, blockSchedule.block))
        {
            emitPipelineState(output, *pipeline, blockSchedule, *context,
                              operationBindings, memoryBindings, fsmWidth);
            continue;
        }
        const unsigned controlCycle = blockSchedule.stateCount - 1;
        for (unsigned cycle = 0; cycle < blockSchedule.stateCount; ++cycle)
        {
            const unsigned stateNumber = blockSchedule.firstState + cycle;
            output << "        " << fsmWidth << "'d" << stateNumber
                   << ": begin\n";
            for (const InstructionSchedule &schedule :
                 blockSchedule.instructions)
            {
                const int captureCycle = schedule.startCycle +
                    (schedule.instruction && isIterativeFloating(*schedule.instruction)
                         ? schedule.latency - 1 : 0);
                if (captureCycle != static_cast<int>(cycle) ||
                    !schedule.instruction || schedule.resultWidth == 0)
                {
                    continue;
                }
                if (isBackendLoad(schedule.instruction))
                    continue;
                if (context->combinationalValues.count(schedule.instruction))
                    continue;
                auto binding = operationBindings.find(schedule.instruction);
                if (binding != operationBindings.end())
                {
                    output << "          " << valueExpression(schedule.instruction, *context) << " <= "
                           << binding->second->instance << "_out;\n";
                }
                else
                {
                    output << "          " << valueExpression(schedule.instruction, *context) << " <= "
                           << instructionExpression(*schedule.instruction, *context)
                           << ";\n";
                }
            }
            for (const InstructionSchedule &schedule :
                 blockSchedule.instructions)
            {
                if (!schedule.instruction ||
                    !isBackendLoad(schedule.instruction) ||
                    context->forwardedLoads.count(schedule.instruction))
                {
                    continue;
                }
                const MemoryAccessInfo &access =
                    context->memoryAccesses.at(schedule.instruction);
                const MemoryObject *memory =
                    findMemoryObject(*context, access.memoryBase);
                if (!memory ||
                    schedule.startCycle +
                            static_cast<int>(memory->readLatency) !=
                        static_cast<int>(cycle))
                    continue;
                auto binding = memoryBindings.find(schedule.instruction);
                if (binding == memoryBindings.end())
                    continue;
                const std::string flatAddress = pointerOffsetExpression(
                    access.pointerOperand, *memory, *context);
                output << "          " << schedule.name << " <= "
                       << partitionedReadExpression(
                              *memory, *binding->second, flatAddress)
                       << ";\n";
            }
            if (cycle == controlCycle)
            {
                emitTransition(output, *blockSchedule.block, *context);
            }
            else
            {
                output << "          state <= " << fsmWidth << "'d"
                       << (stateNumber + 1) << ";\n";
            }
            output << "        end\n";
        }
    }

    output << "        default: begin\n"
           << "          state <= STATE_IDLE;\n"
           << "          ap_idle <= 1'b1;\n"
           << "        end\n"
           << "      endcase\n"
           << "    end\n"
           << "  end\n"
           << "endmodule\n";

    if (context->requiredFeatures.floatingPoint)
        emitHardFloatLibrary(output);
    context->rtl.verilog = output.str();
    context->rtl.valid = true;
    return false;
}

void HI_RTLGeneration::getAnalysisUsage(AnalysisUsage &usage) const
{
    usage.setPreservesAll();
}
