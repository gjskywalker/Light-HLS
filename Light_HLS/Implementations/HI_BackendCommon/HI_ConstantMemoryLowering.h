#ifndef HI_CONSTANT_MEMORY_LOWERING_H
#define HI_CONSTANT_MEMORY_LOWERING_H

#include "HI_FloatingPoint.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include <vector>

namespace light_hls { namespace backend {

// The hardware interface has no errno/fenv side channel. Only exact known
// scalar libm signatures are mapped; user-defined functions are never replaced.
inline unsigned lowerFloatingMathCalls(llvm::Module &module)
{
    using namespace llvm;
    std::vector<CallInst *> calls;
    for (Function &function : module)
        for (BasicBlock &block : function)
            for (Instruction &instruction : block)
                if (auto *call = dyn_cast<CallInst>(&instruction))
                    calls.push_back(call);
    unsigned lowered = 0;
    for (CallInst *call : calls)
    {
        Function *callee = call->getCalledFunction();
        if (!callee || !callee->isDeclaration() || callee->isIntrinsic() ||
            callee->isVarArg() || !isSupportedFloat(call->getType()))
            continue;
        const StringRef name = callee->getName();
        Intrinsic::ID id = Intrinsic::not_intrinsic;
        unsigned arguments = 1;
        if (name == "sqrt" || name == "sqrtf" || name == "sqrtf16") id = Intrinsic::sqrt;
        if (name == "fabs" || name == "fabsf" || name == "fabsf16") id = Intrinsic::fabs;
        if (name == "copysign" || name == "copysignf" || name == "copysignf16")
        { id = Intrinsic::copysign; arguments = 2; }
        if (id == Intrinsic::not_intrinsic || call->arg_size() != arguments)
            continue;
        const bool signatureMatches = name.ends_with("f16") ? call->getType()->isHalfTy() :
            name.ends_with("f") ? call->getType()->isFloatTy() : call->getType()->isDoubleTy();
        if (!signatureMatches) continue;
        SmallVector<Value *, 3> operands;
        for (const Use &argument : call->args())
            if (argument->getType() == call->getType()) operands.push_back(argument.get());
        if (operands.size() != arguments) continue;
        IRBuilder<> builder(call);
        builder.SetCurrentDebugLocation(call->getDebugLoc());
        CallInst *replacement = builder.CreateCall(Intrinsic::getDeclaration(&module, id, {call->getType()}), operands);
        replacement->setFastMathFlags(call->getFastMathFlags());
        call->replaceAllUsesWith(replacement);
        call->eraseFromParent();
        ++lowered;
    }
    return lowered;
}

// LLVM fmuladd permits either fused or unfused evaluation. Fix the unfused
// choice for every baseline/candidate; never split the strict llvm.fma intrinsic.
inline unsigned lowerOptionalFloatingContraction(llvm::Module &module)
{
    std::vector<llvm::IntrinsicInst *> calls;
    for (llvm::Function &function : module)
        for (llvm::BasicBlock &block : function)
            for (llvm::Instruction &instruction : block)
                if (auto *call = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction))
                    if (call->getIntrinsicID() == llvm::Intrinsic::fmuladd &&
                        isSupportedFloat(call->getType()))
                        calls.push_back(call);
    for (llvm::IntrinsicInst *call : calls)
    {
        llvm::IRBuilder<> builder(call);
        builder.SetCurrentDebugLocation(call->getDebugLoc());
        llvm::Value *product = builder.CreateFMul(call->getArgOperand(0), call->getArgOperand(1));
        llvm::Value *sum = builder.CreateFAdd(product, call->getArgOperand(2));
        call->replaceAllUsesWith(sum);
        call->eraseFromParent();
    }
    return calls.size();
}

// Lower only fully-known, nonvolatile local initializers. Preserve arbitrary
// memcpy, partial copies, and unknown bytes for explicit backend rejection.
inline unsigned lowerConstantMemoryInitializers(llvm::Module &module)
{
    using namespace llvm;
    std::vector<MemCpyInst *> copies;
    for (Function &function : module)
        for (BasicBlock &block : function)
            for (Instruction &instruction : block)
                if (auto *copy = dyn_cast<MemCpyInst>(&instruction))
                    copies.push_back(copy);
    unsigned lowered = 0;
    const DataLayout &layout = module.getDataLayout();
    for (MemCpyInst *copy : copies)
    {
        auto *allocation = dyn_cast<AllocaInst>(copy->getDest()->stripPointerCasts());
        auto *source = dyn_cast<Constant>(copy->getSource());
        auto *length = dyn_cast<ConstantInt>(copy->getLength());
        if (!allocation || !source || !length || copy->isVolatile() ||
            length->getValue().getActiveBits() > 32 || length->getZExtValue() > 65536)
            continue;
        llvm::Type *element = allocation->getAllocatedType();
        while (auto *array = dyn_cast<llvm::ArrayType>(element))
            element = array->getElementType();
        if (!scalarBitWidth(element) || scalarBitWidth(element) % 8)
            continue;
        const uint64_t stride = layout.getTypeAllocSize(element);
        const uint64_t bytes = length->getZExtValue();
        if (!stride || bytes % stride || bytes != layout.getTypeAllocSize(allocation->getAllocatedType()))
            continue;
        std::vector<Constant *> values;
        for (uint64_t offset = 0; offset < bytes; offset += stride)
        {
            Constant *value = ConstantFoldLoadFromConstPtr(source, element,
                APInt(layout.getIndexTypeSizeInBits(source->getType()), offset), layout);
            if (!value || (!isa<ConstantInt>(value) && !isa<ConstantFP>(value)))
                break;
            values.push_back(value);
        }
        if (values.size() != bytes / stride)
            continue;
        IRBuilder<> builder(copy);
        builder.SetCurrentDebugLocation(copy->getDebugLoc());
        for (unsigned index = 0; index < values.size(); ++index)
        {
            Value *address = builder.CreateGEP(element, allocation, builder.getInt64(index));
            // A scalar store may not inherit the initializer's full alignment.
            builder.CreateAlignedStore(values[index], address, Align(1));
        }
        copy->eraseFromParent();
        ++lowered;
    }
    return lowered;
}

} }
#endif
