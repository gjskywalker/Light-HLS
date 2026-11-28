#ifndef HI_BACKEND_IR_LEGALIZATION_H
#define HI_BACKEND_IR_LEGALIZATION_H

#include "HI_FloatingPoint.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <functional>
#include <map>
#include <set>
#include <vector>

namespace light_hls { namespace backend {

// Clang canonicalizes even simple loop bounds and integer comparisons to
// llvm.{s,u}{min,max}. Map the scalar intrinsics to the RTL backend's cmp/mux.
inline unsigned lowerIntegerMinMax(llvm::Module &module)
{
    using namespace llvm;
    std::vector<std::pair<IntrinsicInst *, CmpInst::Predicate>> calls;
    for (Function &function : module)
        for (BasicBlock &block : function)
            for (Instruction &inst : block)
            {
                auto *call = dyn_cast<IntrinsicInst>(&inst);
                if (!call || !call->getType()->isIntegerTy())
                    continue;
                switch (call->getIntrinsicID())
                {
                case Intrinsic::smin: calls.emplace_back(call, CmpInst::ICMP_SLT); break;
                case Intrinsic::smax: calls.emplace_back(call, CmpInst::ICMP_SGT); break;
                case Intrinsic::umin: calls.emplace_back(call, CmpInst::ICMP_ULT); break;
                case Intrinsic::umax: calls.emplace_back(call, CmpInst::ICMP_UGT); break;
                default: break;
                }
            }
    for (auto [call, predicate] : calls)
    {
        IRBuilder<> builder(call);
        builder.SetCurrentDebugLocation(call->getDebugLoc());
        Value *a = call->getArgOperand(0), *b = call->getArgOperand(1);
        call->replaceAllUsesWith(builder.CreateSelect(builder.CreateICmp(predicate, a, b), a, b));
        call->eraseFromParent();
    }
    return calls.size();
}

inline unsigned inlineDirectBackendCalls(llvm::Module &module)
{
    using namespace llvm;
    unsigned count = 0;
    bool progress = true;
    while (progress && count < 256)
    {
        progress = false;
        for (Function &function : module)
        {
            if (function.isDeclaration()) continue;
            for (BasicBlock &block : function)
            {
                for (Instruction &instruction : block)
                {
                    auto *call = dyn_cast<CallBase>(&instruction);
                    Function *callee = call ? call->getCalledFunction() : nullptr;
                    if (!callee || callee->isDeclaration() || callee == &function || callee->isVarArg()) continue;
                    std::set<Function *> visited;
                    std::function<bool(Function *)> recursive = [&](Function *current) {
                        if (!visited.insert(current).second) return false;
                        for (BasicBlock &nested : *current)
                            for (Instruction &operation : nested)
                                if (auto *child = dyn_cast<CallBase>(&operation))
                                    if (Function *target = child->getCalledFunction())
                                    {
                                        if (target == callee || target == &function) return true;
                                        if (!target->isDeclaration() && recursive(target)) return true;
                                    }
                        return false;
                    };
                    if (recursive(callee)) continue;
                    InlineFunctionInfo info;
                    if (InlineFunction(*call, info).isSuccess())
                    { ++count; progress = true; break; }
                }
                if (progress) break;
            }
            if (progress) break;
        }
    }
    return count;
}

inline unsigned materializeBackendConstantExpressions(llvm::Module &module)
{
    std::vector<llvm::Instruction *> users;
    for (llvm::Function &function : module)
        for (llvm::BasicBlock &block : function)
            for (llvm::Instruction &instruction : block)
                users.push_back(&instruction);
    unsigned count = 0;
    std::function<llvm::Value *(llvm::Value *, llvm::Instruction *)> materialize =
        [&](llvm::Value *value, llvm::Instruction *where) -> llvm::Value * {
            auto *expression = llvm::dyn_cast<llvm::ConstantExpr>(value);
            if (!expression) return value;
            llvm::Instruction *instruction = expression->getAsInstruction();
            for (unsigned i = 0; i < instruction->getNumOperands(); ++i)
                instruction->setOperand(i, materialize(instruction->getOperand(i), where));
            instruction->insertBefore(where);
            ++count;
            return instruction;
        };
    for (llvm::Instruction *instruction : users)
        for (unsigned i = 0; i < instruction->getNumOperands(); ++i)
        {
            // PHI operands are evaluated on incoming edges, not in the PHI block.
            llvm::Instruction *where = instruction;
            if (auto *phi = llvm::dyn_cast<llvm::PHINode>(instruction))
                where = phi->getIncomingBlock(i)->getTerminator();
            instruction->setOperand(i, materialize(instruction->getOperand(i), where));
        }
    return count;
}

inline llvm::Type *scalarArrayElement(llvm::Type *type)
{
    while (auto *array = llvm::dyn_cast<llvm::ArrayType>(type))
        type = array->getElementType();
    return scalarBitWidth(type) ? type : nullptr;
}

inline uint64_t arrayElementCount(llvm::Type *type)
{
    uint64_t count = 1;
    while (auto *array = llvm::dyn_cast<llvm::ArrayType>(type))
    {
        if (!array->getNumElements() || count > UINT64_MAX / array->getNumElements())
            return 0;
        count *= array->getNumElements();
        type = array->getElementType();
    }
    return scalarBitWidth(type) ? count : 0;
}

// Replace a same-object pointer PHI with an integer element-offset PHI plus
// a real typed GEP. No fake inttoptr addresses, byte/element unit mixing, or
// merging of distinct external memories is permitted.
inline unsigned lowerSameBasePointerPhis(llvm::Module &module)
{
    using namespace llvm;
    std::vector<PHINode *> phis;
    for (Function &function : module)
        for (BasicBlock &block : function)
            for (PHINode &phi : block.phis())
                if (phi.getType()->isPointerTy()) phis.push_back(&phi);
    unsigned lowered = 0;
    for (PHINode *phi : phis)
    {
        Value *base = nullptr;
        llvm::Type *element = nullptr;
        std::set<Value *> visited;
        bool valid = true;
        std::function<void(Value *)> inspect = [&](Value *value) {
            if (!valid || !visited.insert(value).second) return;
            if (value == phi) return;
            if (auto *gep = dyn_cast<GetElementPtrInst>(value))
            {
                llvm::Type *leaf = scalarArrayElement(gep->getSourceElementType());
                if (!leaf || (element && element != leaf)) { valid = false; return; }
                element = leaf;
                inspect(gep->getPointerOperand());
            }
            else if (auto *cast = dyn_cast<BitCastInst>(value)) inspect(cast->getOperand(0));
            else if (isa<Argument>(value) || isa<AllocaInst>(value))
            {
                if (base && base != value) valid = false;
                base = value;
            }
            else valid = false;
        };
        for (Value *incoming : phi->incoming_values()) inspect(incoming);
        if (!valid || !base || !element) continue;
        llvm::Type *indexType = module.getDataLayout().getIndexType(phi->getType());
        PHINode *offset = PHINode::Create(indexType, phi->getNumIncomingValues(), phi->getName() + ".offset", phi);
        std::function<Value *(Value *, IRBuilder<> &)> build = [&](Value *value, IRBuilder<> &builder) -> Value * {
            if (value == base) return ConstantInt::get(indexType, 0);
            if (value == phi) return offset;
            if (auto *cast = dyn_cast<BitCastInst>(value)) return build(cast->getOperand(0), builder);
            auto *gep = cast<GetElementPtrInst>(value);
            Value *sum = build(gep->getPointerOperand(), builder);
            llvm::Type *indexed = gep->getSourceElementType();
            unsigned ordinal = 0;
            for (Value *index : gep->indices())
            {
                if (ordinal++) indexed = cast<llvm::ArrayType>(indexed)->getElementType();
                Value *term = builder.CreateSExtOrTrunc(index, indexType);
                term = builder.CreateMul(term, ConstantInt::get(indexType, arrayElementCount(indexed)));
                sum = builder.CreateAdd(sum, term);
            }
            return sum;
        };
        for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i)
        {
            BasicBlock *incoming = phi->getIncomingBlock(i);
            IRBuilder<> builder(incoming->getTerminator());
            offset->addIncoming(build(phi->getIncomingValue(i), builder), incoming);
        }
        IRBuilder<> replacement(&*phi->getParent()->getFirstInsertionPt());
        Value *pointer = replacement.CreateGEP(element, base, offset, phi->getName() + ".address");
        phi->replaceAllUsesWith(pointer);
        phi->eraseFromParent();
        ++lowered;
    }
    return lowered;
}

} }
#endif
