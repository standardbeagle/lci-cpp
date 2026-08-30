#pragma once

#include <condition_variable>
#include <mutex>
#include <string>

#include <lci/analysis/codebase_intelligence.h>
#include <lci/analysis/side_effect_analyzer.h>
#include <lci/core/graph_propagator.h>
#include <lci/core/semantic_annotator.h>
#include <lci/indexing/master_index.h>

namespace lci {

class SearchEngine;

namespace mcp {

class McpServer;

/// The analysis stack behind the MCP tool handlers, plus the warmup that
/// populates it from a freshly built index. Extracted from run_mcp so the
/// persistent index server (`lci server`) can host the same MCP surface —
/// one warmed index shared by every `lci mcp` bridge instead of a full
/// re-index per stdio process.
struct McpRuntime {
    SemanticAnnotator annotator;
    GraphPropagator propagator;
    SideEffectAnalyzer side_effects{"generic"};
    CodebaseIntelligenceEngine ci_engine;

    explicit McpRuntime(MasterIndex& index)
        : propagator(&index.ref_tracker()) {}

    /// Runs the post-index analysis phases: annotation extraction, the
    /// side-effect AST pass, the callee-name heuristic, transitive
    /// propagation, and label seeding. Call exactly once, after the index
    /// is built and before any tool handler runs.
    void warmup(MasterIndex& index);
};

/// One-shot readiness latch between a warmup thread and tool dispatch.
/// wait() blocks until finish() has run, then reports the warmup's
/// outcome; an empty failure string means the index is usable.
class WarmupLatch {
  public:
    void finish(std::string failure) {
        {
            std::lock_guard lock(mu_);
            failure_ = std::move(failure);
            done_ = true;
        }
        cv_.notify_all();
    }

    bool wait(std::string& error) {
        std::unique_lock lock(mu_);
        cv_.wait(lock, [this] { return done_; });
        error = failure_;
        return failure_.empty();
    }

  private:
    std::mutex mu_;
    std::condition_variable cv_;
    bool done_{false};
    std::string failure_;
};

/// Registers every production tool handler set against the runtime's
/// analyzers (the five register_* calls run_mcp used to make inline).
void register_all_handlers(McpServer& server, MasterIndex* index,
                           SearchEngine* search_engine, McpRuntime* runtime);

}  // namespace mcp
}  // namespace lci
