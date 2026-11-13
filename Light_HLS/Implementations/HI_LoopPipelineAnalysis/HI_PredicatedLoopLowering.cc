#include "HI_LoopPipelineAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include <algorithm>
#include <map>
#include <set>
#include <vector>

using namespace llvm;
using namespace light_hls::backend;

namespace {
bool lowerLoop(Loop &loop)
{
    BasicBlock *header = loop.getHeader(), *latch = loop.getLoopLatch();
    BasicBlock *exit = loop.getUniqueExitBlock();
    if (loop.getNumBlocks() <= 1 || !loop.getSubLoops().empty() || !latch ||
        !exit || !loop.getLoopPredecessor()) return false;
    auto *latchBranch = dyn_cast<BranchInst>(latch->getTerminator());
    if (!latchBranch || !latchBranch->isConditional() ||
        !((latchBranch->getSuccessor(0) == header && latchBranch->getSuccessor(1) == exit) ||
          (latchBranch->getSuccessor(1) == header && latchBranch->getSuccessor(0) == exit))) return false;
    // The intra-iteration CFG must be a DAG with one latch exit. Early exits,
    // irreducible flow, atomics and stream side effects remain explicit rejects.
    for (BasicBlock *block : loop.blocks())
    {
        auto *branch = dyn_cast<BranchInst>(block->getTerminator());
        if (!branch) return false;
        for (BasicBlock *successor : successors(block))
            if ((!loop.contains(successor) && block != latch) ||
                (successor == header && block != latch)) return false;
        if (block != header)
            for (BasicBlock *predecessor : predecessors(block))
                if (!loop.contains(predecessor)) return false;
        for (Instruction &instruction : *block)
        {
            if (auto *load = dyn_cast<LoadInst>(&instruction))
            { if (load->isVolatile() || load->isAtomic() || !scalarBitWidth(load->getType())) return false; }
            else if (auto *store = dyn_cast<StoreInst>(&instruction))
            { if (store->isVolatile() || store->isAtomic() || !scalarBitWidth(store->getValueOperand()->getType())) return false; }
            else if (auto *call = dyn_cast<CallBase>(&instruction))
            {
                const auto *intrinsic = dyn_cast<IntrinsicInst>(call);
                const bool lifetime = intrinsic && (intrinsic->getIntrinsicID() == Intrinsic::lifetime_start ||
                                                    intrinsic->getIntrinsicID() == Intrinsic::lifetime_end);
                if (!isa<DbgInfoIntrinsic>(call) && !lifetime &&
                    (!intrinsic || !isSafeToSpeculativelyExecute(call))) return false;
            }
            else if (instruction.mayReadOrWriteMemory()) return false;
            else if (block != header && !isa<PHINode>(instruction) && !instruction.isTerminator() &&
                     !isSafeToSpeculativelyExecute(&instruction)) return false;
            if (block != header && isa<PHINode>(instruction) && !scalarBitWidth(instruction.getType())) return false;
        }
    }
    std::vector<BasicBlock *> order;
    std::set<BasicBlock *> pending(loop.block_begin(), loop.block_end());
    while (!pending.empty())
    {
        BasicBlock *next = nullptr;
        for (BasicBlock *block : loop.blocks())
        {
            if (!pending.count(block)) continue;
            bool ready = block == header;
            if (!ready) ready = std::none_of(pred_begin(block), pred_end(block),
                [&](BasicBlock *predecessor) { return pending.count(predecessor); });
            if (ready) { next = block; break; }
        }
        if (!next) return false;
        order.push_back(next); pending.erase(next);
    }
    Instruction *insertion = header->getTerminator();
    IRBuilder<> builder(insertion);
    std::map<std::pair<BasicBlock *, BasicBlock *>, Value *> edges;
    std::map<BasicBlock *, Value *> predicates;
    predicates[header] = builder.getTrue();
    for (BasicBlock *block : order)
    {
        Value *predicate = predicates[block];
        if (block != header)
        {
            predicate = builder.getFalse();
            for (BasicBlock *predecessor : predecessors(block))
                predicate = builder.CreateOr(predicate, edges.at({predecessor, block}), "path.active");
            predicates[block] = predicate;
            while (!block->phis().empty())
            {
                PHINode *phi = &*block->phis().begin();
                Value *selected = UndefValue::get(phi->getType());
                for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i)
                    selected = builder.CreateSelect(edges.at({phi->getIncomingBlock(i), block}),
                        phi->getIncomingValue(i), selected, phi->getName() + ".path");
                phi->replaceAllUsesWith(selected); phi->eraseFromParent();
            }
            for (auto iterator = block->begin(); iterator != block->end();)
            {
                Instruction *instruction = &*iterator++;
                if (instruction->isTerminator()) continue;
                builder.SetCurrentDebugLocation(instruction->getDebugLoc());
                if (auto *load = dyn_cast<LoadInst>(instruction))
                {
                    auto *type = FixedVectorType::get(load->getType(), 1);
                    Value *mask = builder.CreateInsertElement(PoisonValue::get(FixedVectorType::get(builder.getInt1Ty(), 1)), predicate, builder.getInt32(0));
                    Value *loaded = builder.CreateMaskedLoad(type, load->getPointerOperand(), load->getAlign(), mask, UndefValue::get(type), load->getName() + ".guarded");
                    load->replaceAllUsesWith(builder.CreateExtractElement(loaded, builder.getInt32(0), load->getName()));
                    load->eraseFromParent();
                }
                else if (auto *store = dyn_cast<StoreInst>(instruction))
                {
                    auto *type = FixedVectorType::get(store->getValueOperand()->getType(), 1);
                    Value *mask = builder.CreateInsertElement(PoisonValue::get(FixedVectorType::get(builder.getInt1Ty(), 1)), predicate, builder.getInt32(0));
                    Value *value = builder.CreateInsertElement(PoisonValue::get(type), store->getValueOperand(), builder.getInt32(0));
                    builder.CreateMaskedStore(value, store->getPointerOperand(), store->getAlign(), mask);
                    store->eraseFromParent();
                }
                else instruction->moveBefore(insertion);
            }
        }
        if (block == latch) continue;
        auto *branch = cast<BranchInst>(block->getTerminator());
        if (branch->isUnconditional()) edges[{block, branch->getSuccessor(0)}] = predicate;
        else
        {
            // Select provides short-circuit poison semantics for inactive paths.
            edges[{block, branch->getSuccessor(0)}] = builder.CreateSelect(predicate, branch->getCondition(), builder.getFalse(), "edge.true");
            edges[{block, branch->getSuccessor(1)}] = builder.CreateSelect(predicate, builder.CreateNot(branch->getCondition()), builder.getFalse(), "edge.false");
        }
    }
    BranchInst *replacement = BranchInst::Create(latchBranch->getSuccessor(0), latchBranch->getSuccessor(1), latchBranch->getCondition(), insertion);
    replacement->setMetadata(LLVMContext::MD_loop, latchBranch->getMetadata(LLVMContext::MD_loop));
    replacement->setDebugLoc(latchBranch->getDebugLoc());
    insertion->eraseFromParent();
    for (PHINode &phi : header->phis()) phi.replaceIncomingBlockWith(latch, header);
    for (PHINode &phi : exit->phis()) phi.replaceIncomingBlockWith(latch, header);
    for (BasicBlock *block : order) if (block != header) block->dropAllReferences();
    for (BasicBlock *block : order) if (block != header) block->eraseFromParent();
    return true;
}
}

char HI_PredicatedLoopLowering::ID = 0;
HI_PredicatedLoopLowering::HI_PredicatedLoopLowering(HLSBackendContextPtr context)
    : ModulePass(ID), context(std::move(context)) {}

bool HI_PredicatedLoopLowering::runOnModule(Module &module)
{
    if (!context || !context->top || context->hasErrors() ||
        context->configuration.pragmas.pipeline.empty()) return false;
    bool changed = false, progress = true;
    while (progress)
    {
        progress = false;
        DominatorTree dominators(*context->top);
        LoopInfo loops(dominators);
        for (Loop *loop : loops.getLoopsInPreorder())
        {
            const std::string key = context->top->getName().str() + "-" + loop->getHeader()->getName().str();
            auto label = context->configuration.irLoopLabels.find(key);
            if (label == context->configuration.irLoopLabels.end()) continue;
            bool requested = std::any_of(context->configuration.pragmas.pipeline.begin(),
                context->configuration.pragmas.pipeline.end(), [&](const auto &entry) { return entry.first == label->second; });
            if (requested && lowerLoop(*loop))
            {
                ++context->predicatedPipelineLoops;
                progress = changed = true;
                break;
            }
        }
    }
    if (changed && verifyModule(module, &errs()))
        context->addError("BACKEND_PREDICATED_IR_INVALID", "Predicated loop conversion failed LLVM verification.");
    return changed;
}
