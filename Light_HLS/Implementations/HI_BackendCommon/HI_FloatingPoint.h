#ifndef HI_FLOATING_POINT_H
#define HI_FLOATING_POINT_H

#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"

namespace light_hls { namespace backend {

inline bool isSupportedFloat(const llvm::Type *type)
{
    return type->isHalfTy() || type->isFloatTy() || type->isDoubleTy();
}

inline unsigned scalarBitWidth(const llvm::Type *type)
{
    // A single-lane masked memory operation is a scalar datapath with a guard.
    if (const auto *vector = llvm::dyn_cast<llvm::FixedVectorType>(type))
        return vector->getNumElements() == 1 ? scalarBitWidth(vector->getElementType()) : 0;
    if (type->isIntegerTy())
        return type->getIntegerBitWidth();
    if (type->isHalfTy())
        return 16;
    if (type->isFloatTy())
        return 32;
    if (type->isDoubleTy())
        return 64;
    return 0;
}

inline bool isFloatingOperator(const llvm::Instruction &instruction)
{
    if (llvm::isa<llvm::ExtractElementInst, llvm::InsertElementInst>(instruction)) return false;
    if (llvm::isa<llvm::LoadInst, llvm::StoreInst, llvm::PHINode>(instruction) ||
        instruction.isTerminator())
        return false;
    if (isSupportedFloat(instruction.getType()))
        return true;
    for (const llvm::Use &operand : instruction.operands())
        if (isSupportedFloat(operand->getType()))
            return true;
    return false;
}

// Arithmetic uses round-to-nearest, ties-to-even. FP-to-int truncates toward
// zero, as required by LLVM. These latencies describe the emitted hardware,
// not a clock-frequency characterization of the combinational arithmetic.
inline bool isIterativeFloating(const llvm::Instruction &instruction)
{
    const auto *intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction);
    return instruction.getOpcode() == llvm::Instruction::FDiv ||
        (intrinsic && intrinsic->getIntrinsicID() == llvm::Intrinsic::sqrt);
}

inline bool isSupportedFloatingIntrinsic(llvm::Intrinsic::ID id)
{
    return id == llvm::Intrinsic::fma || id == llvm::Intrinsic::sqrt ||
           id == llvm::Intrinsic::fabs || id == llvm::Intrinsic::copysign;
}

inline int floatingLatency(const llvm::Instruction &instruction)
{
    if (isIterativeFloating(instruction))
        return instruction.getType()->isHalfTy() ? 16 :
               instruction.getType()->isDoubleTy() ? 58 : 29;
    return 1;
}

} }
#endif
