#ifndef HI_MEMORY_OPERATION_H
#define HI_MEMORY_OPERATION_H
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Constants.h"

namespace light_hls { namespace backend {
inline bool isMaskedMemory(const llvm::Instruction *instruction, llvm::Intrinsic::ID id)
{
    const auto *call = llvm::dyn_cast_or_null<llvm::IntrinsicInst>(instruction);
    return call && call->getIntrinsicID() == id;
}
inline bool isBackendLoad(const llvm::Instruction *instruction)
{ return llvm::isa_and_nonnull<llvm::LoadInst>(instruction) || isMaskedMemory(instruction, llvm::Intrinsic::masked_load); }
inline bool isBackendStore(const llvm::Instruction *instruction)
{ return llvm::isa_and_nonnull<llvm::StoreInst>(instruction) || isMaskedMemory(instruction, llvm::Intrinsic::masked_store); }
inline const llvm::Value *backendMemoryPointer(const llvm::Instruction *instruction)
{
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(instruction)) return load->getPointerOperand();
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(instruction)) return store->getPointerOperand();
    const auto *call = llvm::cast<llvm::IntrinsicInst>(instruction);
    return call->getArgOperand(isBackendLoad(instruction) ? 0 : 1);
}
inline const llvm::Value *backendStoredValue(const llvm::Instruction *instruction)
{
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(instruction)) return store->getValueOperand();
    return llvm::cast<llvm::IntrinsicInst>(instruction)->getArgOperand(0);
}
inline const llvm::Value *backendMemoryPredicate(const llvm::Instruction *instruction)
{
    if (llvm::isa<llvm::LoadInst, llvm::StoreInst>(instruction)) return nullptr;
    const auto *call = llvm::cast<llvm::IntrinsicInst>(instruction);
    const auto *mask = call->getArgOperand(isBackendLoad(instruction) ? 2 : 3);
    if (const auto *insert = llvm::dyn_cast<llvm::InsertElementInst>(mask))
        if (const auto *index = llvm::dyn_cast<llvm::ConstantInt>(insert->getOperand(2)))
            if (index->isZero()) return insert->getOperand(1);
    if (const auto *constant = llvm::dyn_cast<llvm::Constant>(mask)) return constant->getAggregateElement(0U);
    return nullptr;
}
} }
#endif
