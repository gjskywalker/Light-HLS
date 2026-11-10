#ifndef HI_SCHEDULED_LATENCY_H
#define HI_SCHEDULED_LATENCY_H

#include "HI_BackendContext.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/CFG.h"
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

    // Collapse child loops and pipelines; stop before the current loop header
    // on backedges, and before its unique exit on outgoing edges. Thus the
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
            next.push_back(child->getExitBlock());
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
                if (successor != scope->getExitBlock())
                    throw std::runtime_error("Multiple loop exit destinations");
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
        if (!loop->getLoopLatch() || !loop->getExitBlock())
            throw std::runtime_error("Loop requires a unique latch and exit: " + loop->getName().str());
        const auto *count = llvm::dyn_cast<llvm::SCEVConstant>(evolution.getBackedgeTakenCount(loop));
        if (!count || count->getAPInt().getActiveBits() > 63)
            throw std::runtime_error("Nonconstant or unrepresentable backedge count: " + loop->getName().str());
        const long long backedges = count->getAPInt().getZExtValue();
        std::map<const llvm::BasicBlock *, Paths> memo;
        std::set<const llvm::BasicBlock *> active;
        const Paths paths = walk(loop->getHeader(), loop, memo, active);
        if (!paths.exit.present || (backedges && !paths.repeat.present))
            throw std::runtime_error("Missing loop exit or repeat path");
        Range total = paths.exit;
        if (backedges)
            total = sum({multiply(backedges, paths.repeat.minimum),
                         multiply(backedges, paths.repeat.maximum), true}, total);
        result.loops.push_back({loop->getName().str(), backedges, paths.repeat, paths.exit, total});
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
