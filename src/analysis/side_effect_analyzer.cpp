#include <lci/analysis/side_effect_analyzer.h>

#include <lci/core/reference_tracker.h>
#include <lci/indexing/master_index.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace lci {

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------

std::vector<std::string> categories_to_strings(uint32_t cat) {
    std::vector<std::string> result;
    if (cat == side_effect::kNone) return result;
    if (cat & side_effect::kParamWrite) result.emplace_back("param_write");
    if (cat & side_effect::kReceiverWrite) result.emplace_back("receiver_write");
    if (cat & side_effect::kGlobalWrite) result.emplace_back("global_write");
    if (cat & side_effect::kClosureWrite) result.emplace_back("closure_write");
    if (cat & side_effect::kFieldWrite) result.emplace_back("field_write");
    if (cat & side_effect::kIO) result.emplace_back("io");
    if (cat & side_effect::kDatabase) result.emplace_back("database");
    if (cat & side_effect::kNetwork) result.emplace_back("network");
    if (cat & side_effect::kThrow) result.emplace_back("throw");
    if (cat & side_effect::kChannel) result.emplace_back("channel");
    if (cat & side_effect::kAsync) result.emplace_back("async");
    if (cat & side_effect::kExternalCall) result.emplace_back("external_call");
    if (cat & side_effect::kDynamicCall) result.emplace_back("dynamic_call");
    if (cat & side_effect::kReflection) result.emplace_back("reflection");
    if (cat & side_effect::kUncertain) result.emplace_back("uncertain");
    return result;
}

AccessPatternType classify_access_sequence(std::string_view seq) {
    if (seq.empty()) return AccessPatternType::Pure;

    bool has_read = seq.find('R') != std::string_view::npos;
    bool has_write = seq.find('W') != std::string_view::npos;

    if (!has_write) return AccessPatternType::Pure;
    if (!has_read) return AccessPatternType::WriteOnly;

    auto first_w = seq.find('W');
    auto first_r = seq.find('R');
    auto last_w = seq.rfind('W');
    auto last_r = seq.rfind('R');

    if (last_r < first_w) return AccessPatternType::ReadThenWrite;
    if (last_w < first_r) return AccessPatternType::WriteThenRead;

    return AccessPatternType::Interleaved;
}

PurityLevel compute_purity_level(uint32_t categories, bool has_unresolved) {
    if (categories == side_effect::kNone) {
        return has_unresolved ? PurityLevel::InternallyPure : PurityLevel::Pure;
    }
    if (categories & (side_effect::kIO | side_effect::kNetwork |
                      side_effect::kDatabase | side_effect::kExternalCall |
                      side_effect::kDynamicCall)) {
        return PurityLevel::ExternalDependency;
    }
    if (categories & side_effect::kGlobalWrite) {
        return PurityLevel::ModuleGlobal;
    }
    if (categories & (side_effect::kReceiverWrite | side_effect::kFieldWrite)) {
        return PurityLevel::ObjectState;
    }
    if (categories & side_effect::kParamWrite) {
        return PurityLevel::ObjectState;
    }
    return PurityLevel::InternallyPure;
}

// ---------------------------------------------------------------------------
// SideEffectAnalyzer
// ---------------------------------------------------------------------------

SideEffectAnalyzer::SideEffectAnalyzer(std::string_view language,
                                       const SideEffectAnalyzerConfig& config)
    : language_(language), config_(config) {}

void SideEffectAnalyzer::begin_function(std::string_view name,
                                        std::string_view file,
                                        int start_line, int end_line) {
    current_func_storage_ = FunctionAnalysisContext{};
    current_func_storage_.name = std::string(name);
    current_func_storage_.file = std::string(file);
    current_func_storage_.start_line = start_line;
    current_func_storage_.end_line = end_line;
    current_func_ = &current_func_storage_;
}

SideEffectInfo SideEffectAnalyzer::end_function() {
    if (!current_func_) return {};

    auto& ctx = *current_func_;
    SideEffectInfo info{};

    info.function_name = ctx.name;
    info.file_path = ctx.file;
    info.start_line = ctx.start_line;
    info.end_line = ctx.end_line;

    info.categories = ctx.side_effects;
    info.external_calls = ctx.external_calls;
    info.unresolved_calls = ctx.unresolved_calls;
    info.throw_sites = ctx.throw_sites;
    info.impurity_reasons = ctx.impurity_reasons;

    // Analyze access patterns
    info.access_pattern = analyze_access_pattern(ctx.accesses);
    info.has_access_pattern = true;

    // Error handling info
    bool can_throw = !ctx.throw_sites.empty() ||
                     (ctx.side_effects & side_effect::kThrow) != 0;
    info.error_handling.can_throw = can_throw;
    info.error_handling.returns_error = ctx.returns_error;
    info.error_handling.exception_neutral =
        ctx.defer_count == 0 && ctx.try_finally_count == 0 && !can_throw;
    info.error_handling.exception_safe =
        ctx.defer_count > 0 || ctx.try_finally_count > 0;
    info.error_handling.defer_count = ctx.defer_count;
    info.error_handling.try_finally_count = ctx.try_finally_count;
    info.error_handling.throw_count = static_cast<int>(ctx.throw_sites.size());
    info.has_error_handling = true;

    // Extract parameter writes
    absl::flat_hash_map<int, bool> param_index_set;
    for (const auto& access : ctx.accesses) {
        if (access.type == AccessType::Write &&
            access.target_type == AccessTarget::Parameter) {
            int idx = 0;
            auto it = ctx.parameters.find(access.base_identifier);
            if (it != ctx.parameters.end()) idx = it->second;

            info.parameter_writes.push_back(ParameterWriteInfo{
                access.base_identifier, idx, access.line, access.column,
                access.field_path, false});
            param_index_set[idx] = true;
        }
    }

    populate_purity_classification(ctx, info, param_index_set);
    info.confidence = determine_confidence(ctx, info);
    compute_purity_score(info);

    // Store
    std::string key = ctx.file + ":" + std::to_string(ctx.start_line) + ":0";
    results_[key] = info;

    current_func_ = nullptr;
    return info;
}

void SideEffectAnalyzer::add_parameter(std::string_view name, int index) {
    if (current_func_)
        current_func_->parameters[std::string(name)] = index;
}

void SideEffectAnalyzer::set_receiver(std::string_view name,
                                      std::string_view receiver_type) {
    if (!current_func_) return;
    current_func_->receiver_name = std::string(name);
    current_func_->receiver_type = std::string(receiver_type);
}

void SideEffectAnalyzer::add_local_variable(std::string_view name, int line) {
    if (current_func_)
        current_func_->local_variables[std::string(name)] = line;
}

void SideEffectAnalyzer::enter_scope() {
    if (!current_func_) return;
    absl::flat_hash_map<std::string, int> outer(
        current_func_->local_variables.begin(),
        current_func_->local_variables.end());
    current_func_->outer_scopes.push_back(std::move(outer));
    current_func_->scope_depth++;
}

void SideEffectAnalyzer::exit_scope() {
    if (!current_func_ || current_func_->outer_scopes.empty()) return;
    current_func_->outer_scopes.pop_back();
    current_func_->scope_depth--;
}

void SideEffectAnalyzer::record_access(
    std::string_view identifier, const std::vector<std::string>& field_path,
    AccessType access_type, int line, int column) {
    if (!current_func_) return;
    if (static_cast<int>(current_func_->accesses.size()) >=
        config_.max_accesses_per_function) {
        return;
    }

    auto target_type = classify_target(identifier);
    auto target = build_target_string(identifier, field_path, target_type);

    FieldAccess access{};
    access.target = target;
    access.target_type = target_type;
    access.type = access_type;
    access.line = line;
    access.column = column;
    access.seq_num = current_func_->seq_num;
    access.base_identifier = std::string(identifier);
    access.field_path = field_path;

    current_func_->accesses.push_back(std::move(access));
    current_func_->seq_num++;

    if (access_type == AccessType::Write) {
        record_write_side_effect(target_type, identifier, line);
    }
}

void SideEffectAnalyzer::record_function_call(
    std::string_view func_name, std::string_view qualifier,
    bool is_method, int line, int column) {
    if (!current_func_) return;

    std::string qualified = std::string(func_name);
    if (!qualifier.empty()) {
        qualified = std::string(qualifier) + "." + std::string(func_name);
    }

    // Unknown function - record for Phase 2 resolution
    current_func_->unresolved_calls.push_back(UnresolvedCallInfo{
        std::string(func_name), std::string(qualifier), is_method, line, column});
}

void SideEffectAnalyzer::record_dynamic_call(std::string_view description,
                                             int line, int column) {
    if (!current_func_) return;

    current_func_->side_effects |= side_effect::kDynamicCall;
    current_func_->external_calls.push_back(ExternalCallInfo{
        std::string(description), line, column, false, {}, {},
        "dynamic dispatch - cannot determine target"});
    current_func_->impurity_reasons.push_back(
        "dynamic call at line " + std::to_string(line) + ": " +
        std::string(description));
}

void SideEffectAnalyzer::record_throw(std::string_view throw_type,
                                      int line, int column) {
    if (!current_func_) return;

    current_func_->side_effects |= side_effect::kThrow;
    current_func_->throw_sites.push_back(
        ThrowSiteInfo{std::string(throw_type), line, column});
}

void SideEffectAnalyzer::record_defer() {
    if (current_func_) current_func_->defer_count++;
}

void SideEffectAnalyzer::record_try_finally() {
    if (current_func_) current_func_->try_finally_count++;
}

void SideEffectAnalyzer::record_error_return() {
    if (current_func_) current_func_->returns_error = true;
}

void SideEffectAnalyzer::record_channel_op(int line) {
    if (!current_func_) return;
    current_func_->side_effects |= side_effect::kChannel;
    current_func_->impurity_reasons.push_back(
        "channel operation at line " + std::to_string(line));
}

const SideEffectInfo* SideEffectAnalyzer::get_result(std::string_view file,
                                                     int line) const {
    std::string key = std::string(file) + ":" + std::to_string(line) + ":0";
    auto it = results_.find(key);
    return it != results_.end() ? &it->second : nullptr;
}

// Forward-decl: anonymous-namespace classifier defined later in file.
namespace {
uint32_t classify_callee_category(std::string_view callee);
}

void SideEffectAnalyzer::add_result(std::string key, SideEffectInfo info) {
    results_[std::move(key)] = std::move(info);
}

void SideEffectAnalyzer::populate_from_index(const MasterIndex& indexer) {
    const auto& ref = indexer.ref_tracker();
    for (FileID fid : indexer.get_all_file_ids()) {
        std::string file_path = indexer.get_file_path(fid);
        for (const auto* es : ref.get_file_enhanced_symbols(fid)) {
            if (!es) continue;
            bool is_callable = es->symbol.type == SymbolType::Function ||
                               es->symbol.type == SymbolType::Method ||
                               es->symbol.type == SymbolType::Constructor;
            if (!is_callable) continue;

            SideEffectInfo info;
            info.function_name = std::string(es->symbol.name);
            info.file_path = file_path;
            info.start_line = es->symbol.line;
            info.end_line = es->symbol.end_line;

            uint32_t cats = side_effect::kNone;
            for (const auto& callee : ref.get_callee_names(es->id)) {
                cats |= classify_callee_category(callee);
            }
            info.categories = cats;
            info.is_pure = (cats == side_effect::kNone);
            info.purity_level = info.is_pure ? PurityLevel::Pure
                                              : PurityLevel::ExternalDependency;
            info.confidence = PurityConfidence::Medium;
            info.purity_score = info.is_pure ? 1.0 : 0.0;
            info.purity_confidence_score = 0.7;

            std::string key = file_path + ":" + std::to_string(info.start_line)
                              + ":0";
            results_[std::move(key)] = std::move(info);
        }
    }
}

namespace {
// Filters which categories propagate to callers. Mirrors Go's
// getCategoriesToPropagate under DefaultSideEffectPropagationConfig (IO,
// throws, global writes all enabled): everything propagates except the
// caller-local-only effects (closure/field writes, async, reflection).
uint32_t categories_to_propagate(uint32_t cat) {
    uint32_t result = 0;
    result |= cat & (side_effect::kParamWrite | side_effect::kReceiverWrite);
    result |= cat & (side_effect::kIO | side_effect::kNetwork |
                     side_effect::kDatabase | side_effect::kChannel);
    result |= cat & side_effect::kThrow;
    result |= cat & side_effect::kGlobalWrite;
    result |= cat & (side_effect::kUncertain | side_effect::kExternalCall |
                     side_effect::kDynamicCall);
    return result;
}
}  // namespace

void SideEffectAnalyzer::propagate_transitive(const MasterIndex& indexer) {
    const auto& ref = indexer.ref_tracker();

    // Map each callable symbol to its (already populated) local SideEffectInfo.
    absl::flat_hash_map<SymbolID, SideEffectInfo*> by_symbol;
    by_symbol.reserve(results_.size());
    for (FileID fid : indexer.get_all_file_ids()) {
        std::string file_path = indexer.get_file_path(fid);
        for (const auto* es : ref.get_file_enhanced_symbols(fid)) {
            if (!es) continue;
            std::string key =
                file_path + ":" + std::to_string(es->symbol.line) + ":0";
            auto it = results_.find(key);
            if (it != results_.end()) by_symbol[es->id] = &it->second;
        }
    }

    // Fixpoint: push each symbol's effects upstream to its callers' transitive
    // set until nothing changes. Bounded iterations guard against cycles.
    constexpr int kMaxIterations = 100;
    bool changed = true;
    for (int iter = 0; changed && iter < kMaxIterations; ++iter) {
        changed = false;
        for (auto& [sid, info] : by_symbol) {
            uint32_t combined = info->categories | info->transitive_categories;
            uint32_t to_propagate = categories_to_propagate(combined);
            if (to_propagate == 0) continue;
            for (SymbolID caller_id : ref.get_caller_symbols(sid)) {
                auto cit = by_symbol.find(caller_id);
                if (cit == by_symbol.end()) continue;
                SideEffectInfo* caller = cit->second;
                uint32_t old = caller->transitive_categories;
                caller->transitive_categories |= to_propagate;
                if (caller->transitive_categories != old) changed = true;
            }
        }
    }

    // Recompute the combined purity assessment (Go updatePurityAssessment).
    for (auto& [sid, info] : by_symbol) {
        (void)sid;
        uint32_t combined = info->categories | info->transitive_categories;
        info->is_pure = (combined == side_effect::kNone);
        if (info->is_pure) {
            info->purity_level = PurityLevel::Pure;
            info->purity_score = 1.0;
        } else {
            if (info->purity_level == PurityLevel::Pure) {
                info->purity_level = PurityLevel::ExternalDependency;
            }
            info->purity_score = 0.0;
        }
    }
}

namespace {
// Cross-language conservative impure-callee classifier. Names matched
// case-insensitively against the bare callee identifier (no qualifier
// prefix). Source: Go's classifyKnownCallee with a few cross-language
// additions for Python/JS/TS coverage on the real-project corpora.
uint32_t classify_callee_category(std::string_view callee) {
    auto lower_starts_with = [&](std::string_view name, std::string_view prefix) {
        if (name.size() < prefix.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i) {
            char a = static_cast<char>(
                std::tolower(static_cast<unsigned char>(name[i])));
            if (a != prefix[i]) return false;
        }
        return true;
    };

    // I/O: print, log, write, read, scan, open, close, fopen, etc.
    static constexpr std::string_view io_prefixes[] = {
        "print", "fprint", "puts",  "fputs", "printf", "fprintf",
        "scanf", "fscanf", "fopen", "fread", "fwrite", "open",
        "close", "log",    "logger"};
    for (auto p : io_prefixes) {
        if (lower_starts_with(callee, p)) return side_effect::kIO;
    }

    // Network: send, recv, fetch, dial, listen, accept, connect,
    // http.Get/Post/etc.
    static constexpr std::string_view net_prefixes[] = {
        "send", "recv", "fetch", "dial", "listen", "accept",
        "connect", "request", "httpget", "httppost"};
    for (auto p : net_prefixes) {
        if (lower_starts_with(callee, p)) return side_effect::kNetwork;
    }

    // Database: query, exec, prepare, execute, transaction
    static constexpr std::string_view db_prefixes[] = {
        "query", "execute", "prepare", "transaction", "commit",
        "rollback"};
    for (auto p : db_prefixes) {
        if (lower_starts_with(callee, p)) return side_effect::kDatabase;
    }

    // Throw/panic/raise
    static constexpr std::string_view throw_prefixes[] = {
        "panic", "raise", "throw", "abort"};
    for (auto p : throw_prefixes) {
        if (lower_starts_with(callee, p)) return side_effect::kThrow;
    }

    // Dynamic / reflective
    static constexpr std::string_view dynamic_prefixes[] = {"eval", "exec"};
    for (auto p : dynamic_prefixes) {
        if (lower_starts_with(callee, p)) return side_effect::kDynamicCall;
    }

    return side_effect::kNone;
}
}  // namespace


// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

AccessTarget SideEffectAnalyzer::classify_target(
    std::string_view identifier) const {
    if (!current_func_) return AccessTarget::Unknown;

    auto& ctx = *current_func_;
    std::string id(identifier);

    if (ctx.parameters.contains(id)) return AccessTarget::Parameter;

    if (id == ctx.receiver_name || id == "this" || id == "self")
        return AccessTarget::Receiver;

    if (ctx.local_variables.contains(id)) return AccessTarget::Local;

    for (const auto& scope : ctx.outer_scopes) {
        if (scope.contains(id)) return AccessTarget::Closure;
    }

    return AccessTarget::Global;
}

std::string SideEffectAnalyzer::build_target_string(
    std::string_view identifier,
    const std::vector<std::string>& field_path,
    AccessTarget target_type) const {
    std::string_view prefix;
    switch (target_type) {
        case AccessTarget::Parameter: prefix = "param:"; break;
        case AccessTarget::Receiver: prefix = "receiver:"; break;
        case AccessTarget::Local: prefix = "local:"; break;
        case AccessTarget::Global: prefix = "global:"; break;
        case AccessTarget::Closure: prefix = "closure:"; break;
        default: prefix = "unknown:"; break;
    }

    std::string target;
    target.reserve(prefix.size() + identifier.size() + field_path.size() * 8);
    target.append(prefix);
    target.append(identifier);
    for (const auto& field : field_path) {
        target.push_back('.');
        target.append(field);
    }
    return target;
}

void SideEffectAnalyzer::record_write_side_effect(AccessTarget target_type,
                                                   std::string_view identifier,
                                                   int line) {
    if (!current_func_) return;
    auto& ctx = *current_func_;
    std::string line_str = std::to_string(line);

    switch (target_type) {
        case AccessTarget::Parameter:
            ctx.side_effects |= side_effect::kParamWrite;
            ctx.impurity_reasons.push_back(
                "writes to parameter '" + std::string(identifier) +
                "' at line " + line_str);
            break;
        case AccessTarget::Receiver:
            ctx.side_effects |= side_effect::kReceiverWrite;
            ctx.impurity_reasons.push_back(
                "writes to receiver at line " + line_str);
            break;
        case AccessTarget::Global:
            ctx.side_effects |= side_effect::kGlobalWrite;
            ctx.impurity_reasons.push_back(
                "writes to global '" + std::string(identifier) +
                "' at line " + line_str);
            break;
        case AccessTarget::Closure:
            ctx.side_effects |= side_effect::kClosureWrite;
            ctx.impurity_reasons.push_back(
                "writes to closure variable '" + std::string(identifier) +
                "' at line " + line_str);
            break;
        default:
            break;
    }
}

AccessPattern SideEffectAnalyzer::analyze_access_pattern(
    const std::vector<FieldAccess>& accesses) const {
    AccessPattern pattern{};

    if (accesses.empty()) {
        pattern.pattern = AccessPatternType::Pure;
        return pattern;
    }

    pattern.accesses = accesses;

    // Group by target
    absl::flat_hash_map<std::string, std::vector<FieldAccess>> by_target;
    for (const auto& access : accesses) {
        by_target[access.target].push_back(access);
    }

    bool has_write = false;
    bool has_interleaved = false;
    bool has_write_then_read = false;

    for (auto& [target, target_accesses] : by_target) {
        auto tap = analyze_target_accesses(target, target_accesses);

        if (tap.write_count > 0) has_write = true;

        if (tap.pattern == AccessPatternType::Interleaved) {
            has_interleaved = true;
            pattern.violations.push_back(PatternViolation{
                ViolationType::InterleavedAccess, target, tap.first_write_line,
                0, 0, "interleaved read/write pattern on " + target, 0.7});
        } else if (tap.pattern == AccessPatternType::WriteThenRead) {
            has_write_then_read = true;
            pattern.violations.push_back(PatternViolation{
                ViolationType::WriteBeforeRead, target, tap.first_write_line,
                tap.first_read_after_write_line, tap.first_write_line,
                "write before read on " + target, 0.8});
        }

        if (tap.write_count > 0 && !target_accesses.empty()) {
            auto tt = target_accesses[0].target_type;
            if (tt == AccessTarget::Parameter) {
                pattern.parameter_writes++;
                pattern.violations.push_back(PatternViolation{
                    ViolationType::MutateParameter, target,
                    tap.first_write_line, 0, 0,
                    "mutation of parameter " + target, 0.9});
            } else if (tt == AccessTarget::Receiver) {
                pattern.receiver_writes++;
                pattern.violations.push_back(PatternViolation{
                    ViolationType::MutateReceiver, target,
                    tap.first_write_line, 0, 0,
                    "mutation of receiver", 0.6});
            } else if (tt == AccessTarget::Global) {
                pattern.global_writes++;
            } else if (tt == AccessTarget::Closure) {
                pattern.closure_writes++;
            }
        }

        pattern.total_reads += tap.read_count;
        pattern.total_writes += tap.write_count;
    }

    pattern.unique_targets = static_cast<int>(by_target.size());

    if (!has_write) {
        pattern.pattern = AccessPatternType::Pure;
    } else if (has_interleaved) {
        pattern.pattern = AccessPatternType::Interleaved;
    } else if (has_write_then_read) {
        pattern.pattern = AccessPatternType::WriteThenRead;
    } else if (pattern.total_reads == 0) {
        pattern.pattern = AccessPatternType::WriteOnly;
    } else {
        pattern.pattern = AccessPatternType::ReadThenWrite;
    }

    return pattern;
}

TargetAccessPattern SideEffectAnalyzer::analyze_target_accesses(
    std::string_view target,
    std::vector<FieldAccess>& accesses) const {
    std::sort(accesses.begin(), accesses.end(),
              [](const FieldAccess& a, const FieldAccess& b) {
                  return a.seq_num < b.seq_num;
              });

    TargetAccessPattern tap{};
    tap.target = std::string(target);
    if (!accesses.empty()) tap.target_type = accesses[0].target_type;

    std::string seq;
    seq.reserve(accesses.size());
    bool first_read_seen = false;
    bool first_write_seen = false;
    bool read_after_write = false;

    for (const auto& access : accesses) {
        if (access.type == AccessType::Read) {
            seq.push_back('R');
            tap.read_count++;
            if (!first_read_seen) {
                tap.first_read_line = access.line;
                first_read_seen = true;
            }
            if (first_write_seen && !read_after_write) {
                tap.first_read_after_write_line = access.line;
                read_after_write = true;
            }
        } else {
            seq.push_back('W');
            tap.write_count++;
            if (!first_write_seen) {
                tap.first_write_line = access.line;
                first_write_seen = true;
            }
        }
    }

    tap.sequence = seq;
    tap.pattern = classify_access_sequence(seq);

    return tap;
}

void SideEffectAnalyzer::populate_purity_classification(
    const FunctionAnalysisContext& ctx, SideEffectInfo& info,
    const absl::flat_hash_map<int, bool>& param_index_set) const {

    auto& pc = info.purity_classification;

    for (const auto& [idx, _] : param_index_set) {
        pc.mutated_parameters.push_back(idx);
    }
    std::sort(pc.mutated_parameters.begin(), pc.mutated_parameters.end());

    pc.mutates_receiver =
        (info.categories & side_effect::kReceiverWrite) != 0;

    // Globals
    absl::flat_hash_map<std::string, bool> global_set;
    for (const auto& gw : info.global_writes) {
        global_set[gw.global_name] = true;
    }
    for (const auto& [name, _] : global_set) {
        pc.mutated_globals.push_back(name);
    }
    std::sort(pc.mutated_globals.begin(), pc.mutated_globals.end());

    // Closures
    absl::flat_hash_map<std::string, bool> closure_set;
    for (const auto& access : ctx.accesses) {
        if (access.type == AccessType::Write &&
            access.target_type == AccessTarget::Closure) {
            closure_set[access.base_identifier] = true;
        }
    }
    for (const auto& [name, _] : closure_set) {
        pc.mutated_closures.push_back(name);
    }
    std::sort(pc.mutated_closures.begin(), pc.mutated_closures.end());

    pc.performs_io = (info.categories & side_effect::kIO) != 0;
    pc.performs_network = (info.categories & side_effect::kNetwork) != 0;
    pc.performs_database = (info.categories & side_effect::kDatabase) != 0;
    pc.can_throw = (info.categories & side_effect::kThrow) != 0 ||
                   !info.throw_sites.empty();
}

PurityConfidence SideEffectAnalyzer::determine_confidence(
    const FunctionAnalysisContext& ctx,
    const SideEffectInfo& info) const {
    if (info.categories & side_effect::kUncertaintyMask)
        return PurityConfidence::Low;

    if (!ctx.external_calls.empty()) {
        return ctx.external_calls.size() > 5 ? PurityConfidence::Low
                                             : PurityConfidence::Medium;
    }

    if (info.categories != side_effect::kNone)
        return PurityConfidence::High;

    return config_.strict_mode ? PurityConfidence::High
                               : PurityConfidence::Medium;
}

void SideEffectAnalyzer::compute_purity_score(SideEffectInfo& info) const {
    bool has_unresolved = !info.unresolved_calls.empty();
    info.purity_level = compute_purity_level(info.categories, has_unresolved);

    if (info.categories == side_effect::kNone && !has_unresolved) {
        info.is_pure = true;
        info.purity_score = 1.0;
        info.purity_confidence_score =
            static_cast<double>(info.confidence) /
            static_cast<double>(PurityConfidence::Proven);
    } else if (info.categories == side_effect::kNone && has_unresolved) {
        info.is_pure = false;
        info.purity_score = 0.8;
        info.purity_confidence_score = 0.6;
    } else {
        info.is_pure = false;
        switch (info.purity_level) {
            case PurityLevel::ObjectState:
                info.purity_score = 0.6;
                info.purity_confidence_score = 0.8;
                break;
            case PurityLevel::ModuleGlobal:
                info.purity_score = 0.3;
                info.purity_confidence_score = 0.8;
                break;
            case PurityLevel::ExternalDependency:
                info.purity_score = 0.0;
                info.purity_confidence_score = 0.9;
                break;
            default:
                info.purity_score = 0.0;
                info.purity_confidence_score = 0.5;
                break;
        }
    }
}

}  // namespace lci
