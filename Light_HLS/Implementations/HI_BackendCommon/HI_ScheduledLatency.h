#ifndef HI_SCHEDULED_LATENCY_H
#define HI_SCHEDULED_LATENCY_H

#include "HI_BackendContext.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/Dominators.h"
#include "llvm/TargetParser/Triple.h"
#include <limits>
#include <stdexcept>

namespace light_hls { namespace backend {

// A schedule-derived envelope, not a prediction of a particular input trace.
// Equal bounds imply a constant cycle count under the backend memory contract.
struct ScheduledLatencyRange {
    long long minimum = 0;
    long long maximum = 0;
    bool present = false;
};

struct ScheduledLoopLatency {
    std::string header;
    long long backedges = 0;
    ScheduledLatencyRange repeat;
    ScheduledLatencyRange exit;
    ScheduledLatencyRange total;
    long long minimumBackedges = 0;
    bool constantBackedges = true;
    std::string backedgeBoundSource;
};

struct ScheduledLatencyResult {
    bool valid = false;
    ScheduledLatencyRange cycles;
    std::string reason;
    std::vector<ScheduledLoopLatency> loops;
};

class ScheduledLatencyAnalysis {
    using Range = ScheduledLatencyRange;
    struct Paths { Range repeat; Range exit; };
    const HLSBackendContext &context;
    llvm::Function &function;
    llvm::DominatorTree dominators;
    llvm::LoopInfo loopInfo;
    llvm::TargetLibraryInfoImpl libraryImpl;
    llvm::TargetLibraryInfo library;
    llvm::AssumptionCache assumptions;
    llvm::ScalarEvolution evolution;
    std::map<const llvm::BasicBlock *, const BlockSchedule *> blocks;
    std::map<const llvm::BasicBlock *, const LoopPipelineInfo *> pipelines;
    std::map<const llvm::Loop *, Range> loopMemo;
    ScheduledLatencyResult result;

    static long long add(long long a, long long b) {
        if (a < 0 || b < 0 || a > std::numeric_limits<long long>::max() - b)
            throw std::runtime_error("Scheduled latency overflow or negative block cost");
        return a + b;
    }
    static long long multiply(long long a, long long b) {
        if (a < 0 || b < 0 || (a && b > std::numeric_limits<long long>::max() / a))
            throw std::runtime_error("Scheduled latency overflow");
        return a * b;
    }
    static Range sum(Range a, Range b) {
        if (!a.present || !b.present) return {};
        return {add(a.minimum, b.minimum), add(a.maximum, b.maximum), true};
    }
    static void merge(Range &target, Range value) {
        if (!value.present) return;
        if (!target.present) { target = value; return; }
        target.minimum = std::min(target.minimum, value.minimum);
        target.maximum = std::max(target.maximum, value.maximum);
    }
    llvm::Loop *childAt(const llvm::BasicBlock *block, const llvm::Loop *scope) {
        llvm::Loop *child = loopInfo.getLoopFor(block);
        if (!child || child == scope) return nullptr;
        while (child->getParentLoop() != scope) {
            child = child->getParentLoop();
            if (!child) throw std::runtime_error("Unexpected entry into a loop scope");
        }
        if (child->getHeader() != block)
            throw std::runtime_error("Non-header entry into a nested loop");
        return child;
    }

    // Prove a ranking bound for i.next = (i | mask) + 1. This is not an
    // exact trip count: the mask may skip values. Require a nonnegative,
    // nonwrapping domain and a latch comparison against a constant limit.
    bool monotoneOrBackedgeBound(llvm::Loop *loop, long long &bound) {
        using namespace llvm;
        BasicBlock *latch = loop->getLoopLatch(), *preheader = loop->getLoopPreheader();
        if (!latch || !preheader) return false;
        auto *branch = dyn_cast<BranchInst>(latch->getTerminator());
        auto *compare = branch && branch->isConditional() ? dyn_cast<ICmpInst>(branch->getCondition()) : nullptr;
        if (!compare || branch->getSuccessor(0) != loop->getHeader() ||
            loop->contains(branch->getSuccessor(1)) ||
            (compare->getPredicate() != ICmpInst::ICMP_SLT && compare->getPredicate() != ICmpInst::ICMP_ULT)) return false;
        auto *limit = dyn_cast<ConstantInt>(compare->getOperand(1));
        auto *increment = dyn_cast<llvm::BinaryOperator>(compare->getOperand(0));
        if (!limit || !increment || increment->getOpcode() != Instruction::Add ||
            !isa<ConstantInt>(increment->getOperand(1)) ||
            !cast<ConstantInt>(increment->getOperand(1))->isOne()) return false;
        auto *combine = dyn_cast<llvm::BinaryOperator>(increment->getOperand(0));
        if (!combine || combine->getOpcode() != Instruction::Or) return false;
        PHINode *phi = dyn_cast<PHINode>(combine->getOperand(0));
        Value *mask = combine->getOperand(1);
        if (!phi || phi->getParent() != loop->getHeader()) {
            phi = dyn_cast<PHINode>(combine->getOperand(1)); mask = combine->getOperand(0);
        }
        if (!phi || phi->getParent() != loop->getHeader() || phi->getNumIncomingValues() != 2 ||
            phi->getIncomingValueForBlock(latch) != increment) return false;
        auto *start = dyn_cast_or_null<ConstantInt>(phi->getIncomingValueForBlock(preheader));
        if (!start || start->isNegative() || limit->isNegative() ||
            start->getValue().getActiveBits() > 62 || limit->getValue().getActiveBits() > 62) return false;
        const uint64_t initial = start->getZExtValue(), end = limit->getZExtValue();
        if (end <= initial) return false;
        APInt maskMaximum = evolution.getUnsignedRange(evolution.getSCEV(mask)).getUnsignedMax();
        if (auto *maskPhi = dyn_cast<PHINode>(mask)) {
            // A constant-seeded PHI with only self right-shifts preserves the
            // unsigned seed upper bound, including across enclosing loops.
            APInt seed(maskMaximum.getBitWidth(), 0);
            bool valid = true, seeded = false;
            for (Value *incoming : maskPhi->incoming_values()) {
                if (auto *constant = dyn_cast<ConstantInt>(incoming)) {
                    if (constant->isNegative()) { valid = false; break; }
                    seed = APIntOps::umax(seed, constant->getValue()); seeded = true;
                } else {
                    auto *shift = dyn_cast<llvm::BinaryOperator>(incoming);
                    auto *amount = shift ? dyn_cast<ConstantInt>(shift->getOperand(1)) : nullptr;
                    if (!shift || (shift->getOpcode() != Instruction::LShr && shift->getOpcode() != Instruction::AShr) ||
                        shift->getOperand(0) != maskPhi || !amount || amount->getValue().uge(maskMaximum.getBitWidth()))
                        { valid = false; break; }
                }
            }
            if (valid && seeded) maskMaximum = seed;
        }
        const unsigned bits = phi->getType()->getIntegerBitWidth();
        const APInt largest = APIntOps::umax(maskMaximum, APInt(bits, std::max(initial, end - 1)));
        // Leave a spare sign bit even for unsigned comparisons, so +1 cannot
        // wrap and the monotonic ranking is valid without relying on poison.
        if (largest.getActiveBits() >= bits - 1) return false;
        bound = static_cast<long long>(end - initial - 1);
        return true;
    }

    // Collapse child loops and pipelines; stop before the current loop header
    // on backedges, and before any loop exit on outgoing edges. Thus the
    // terminating header visit is charged separately from repeated iterations.
    Paths walk(const llvm::BasicBlock *block, const llvm::Loop *scope,
               std::map<const llvm::BasicBlock *, Paths> &memo,
               std::set<const llvm::BasicBlock *> &active) {
        auto cached = memo.find(block);
        if (cached != memo.end()) return cached->second;
        if (!active.insert(block).second)
            throw std::runtime_error("Unsupported irreducible or uncollapsed cycle");
        auto scheduled = blocks.find(block);
        if (scheduled == blocks.end()) throw std::runtime_error("Missing scheduled block");
        Range cost{scheduled->second->executionLatency, scheduled->second->executionLatency, true};
        if (cost.minimum < 0) throw std::runtime_error("Negative scheduled block cost");
        std::vector<const llvm::BasicBlock *> next;
        auto pipeline = pipelines.find(block);
        if (pipeline != pipelines.end()) {
            if (!pipeline->second->valid || !pipeline->second->exit)
                throw std::runtime_error("Invalid pipeline summary");
            next.push_back(pipeline->second->exit);
        } else if (llvm::Loop *child = childAt(block, scope)) {
            cost = loopCost(child);
            llvm::SmallVector<llvm::BasicBlock *, 4> exits;
            child->getExitBlocks(exits);
            next.insert(next.end(), exits.begin(), exits.end());
        } else {
            const auto *branch = llvm::dyn_cast<llvm::BranchInst>(block->getTerminator());
            const auto *condition = branch && branch->isConditional() ?
                llvm::dyn_cast<llvm::ConstantInt>(branch->getCondition()) : nullptr;
            if (condition)
                next.push_back(branch->getSuccessor(condition->isZero() ? 1 : 0));
            else
                for (const llvm::BasicBlock *successor : llvm::successors(block))
                    next.push_back(successor);
        }
        Paths paths;
        if (next.empty()) {
            if (scope || !llvm::isa<llvm::ReturnInst>(block->getTerminator()))
                throw std::runtime_error("Unsupported terminating control path");
            paths.exit = cost;
        }
        for (const llvm::BasicBlock *successor : next) {
            if (scope && successor == scope->getHeader()) {
                merge(paths.repeat, cost);
            } else if (scope && !scope->contains(successor)) {
                merge(paths.exit, cost);
            } else {
                const Paths suffix = walk(successor, scope, memo, active);
                merge(paths.repeat, sum(cost, suffix.repeat));
                merge(paths.exit, sum(cost, suffix.exit));
            }
        }
        active.erase(block);
        memo[block] = paths;
        return paths;
    }

    Range loopCost(llvm::Loop *loop) {
        auto cached = loopMemo.find(loop);
        if (cached != loopMemo.end()) return cached->second;
        llvm::SmallVector<llvm::BasicBlock *, 4> exits;
        loop->getExitBlocks(exits);
        if (exits.empty())
            throw std::runtime_error("Loop has no terminating exit: " + loop->getName().str());
        const auto *exact = llvm::dyn_cast<llvm::SCEVConstant>(evolution.getBackedgeTakenCount(loop));
        const auto *count = exact ? exact : llvm::dyn_cast<llvm::SCEVConstant>(evolution.getConstantMaxBackedgeTakenCount(loop));
        long long backedges = 0;
        std::string boundSource = exact ? "scalar-evolution-exact" : "scalar-evolution-upper-bound";
        if (count && count->getAPInt().getActiveBits() <= 63)
            backedges = count->getAPInt().getZExtValue();
        else if (monotoneOrBackedgeBound(loop, backedges))
            boundSource = "monotone-or-ranking";
        else
            throw std::runtime_error("Nonconstant or unrepresentable backedge count: " + loop->getName().str());
        const long long minimumBackedges = exact ? backedges : 0;
        std::map<const llvm::BasicBlock *, Paths> memo;
        std::set<const llvm::BasicBlock *> active;
        const Paths paths = walk(loop->getHeader(), loop, memo, active);
        if (!paths.exit.present || (backedges && !paths.repeat.present))
            throw std::runtime_error("Missing loop exit or repeat path");
        Range total = paths.exit;
        if (backedges)
            total = sum({multiply(minimumBackedges, paths.repeat.minimum),
                         multiply(backedges, paths.repeat.maximum), true}, total);
        result.loops.push_back({loop->getName().str(), backedges, paths.repeat, paths.exit, total, minimumBackedges, exact != nullptr, boundSource});
        loopMemo[loop] = total;
        return total;
    }

public:
    ScheduledLatencyAnalysis(const HLSBackendContext &backend, llvm::Function &top)
        : context(backend), function(top), dominators(top), loopInfo(dominators),
          libraryImpl(llvm::Triple(top.getParent()->getTargetTriple())), library(libraryImpl),
          assumptions(top), evolution(top, library, assumptions, dominators, loopInfo) {
        for (const auto &block : context.schedule.blocks) blocks[block.block] = &block;
        for (const auto &pipeline : context.schedule.pipelines) pipelines[pipeline.header] = &pipeline;
    }
    ScheduledLatencyResult run() {
        try {
            if (!context.schedule.valid || context.hasErrors())
                throw std::runtime_error("Backend schedule is invalid");
            for (const auto &access : context.memoryAccesses)
                if (access.second.isFIFO)
                    throw std::runtime_error("FIFO backpressure is not statically bounded");
            std::map<const llvm::BasicBlock *, Paths> memo;
            std::set<const llvm::BasicBlock *> active;
            result.cycles = walk(&function.getEntryBlock(), nullptr, memo, active).exit;
            if (!result.cycles.present || result.cycles.maximum <= 0)
                throw std::runtime_error("No positive terminating latency");
            result.valid = true;
            result.reason = result.cycles.minimum == result.cycles.maximum ?
                "Constant schedule-derived transaction cycles" :
                "Control-path envelope; branch correlations and input distribution are not modeled";
        } catch (const std::exception &error) {
            result.valid = false;
            result.cycles = {};
            result.reason = error.what();
        }
        return result;
    }
};

}} // namespace light_hls::backend
#endif
