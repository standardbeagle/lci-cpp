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

namespace {
// Case-insensitive ASCII prefix match shared by the resource tables.
bool iprefix(std::string_view name, std::string_view prefix) {
    if (name.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        char a = static_cast<char>(
            std::tolower(static_cast<unsigned char>(name[i])));
        if (a != prefix[i]) return false;
    }
    return true;
}
}  // namespace

ResourceOpKind classify_resource_callee(std::string_view callee) {
    if (callee.empty()) return ResourceOpKind::None;
    // Release first: "close" must not be shadowed by any acquire prefix.
    static constexpr std::string_view release_prefixes[] = {
        "close",  "fclose",     "unlock",   "release", "free",
        "munmap", "disconnect", "shutdown", "dispose"};
    for (auto p : release_prefixes) {
        if (iprefix(callee, p)) return ResourceOpKind::Release;
    }
    static constexpr std::string_view acquire_prefixes[] = {
        "open",   "fopen",  "connect", "dial",  "lock",
        "acquire", "malloc", "calloc",  "mmap",  "socket"};
    for (auto p : acquire_prefixes) {
        if (iprefix(callee, p)) return ResourceOpKind::Acquire;
    }
    return ResourceOpKind::None;
}

// A word standing ALONE, with no domain noun attached. Bare, these are as
// likely a local collection call or a builder as they are durable work:
// `history.insert(0, r)` is a list operation, and zod's `z.email()` builds a
// validator rather than sending mail. Compounded — `insertChild`,
// `updateInventory`, `sendEmail`, `publishEvent` — they name something that
// lives past the call, which is what an error would have to undo.
bool is_bare_ambiguous_verb(std::string_view callee) {
    for (std::string_view v :
         // Collection and assignment verbs.
         {"update", "add", "insert", "put", "post", "set", "remove", "delete",
          "append", "push", "pop", "create", "write", "store", "save",
          // A stream controller's enqueue and a DI container's register are
          // in-memory (trpc's SSE producer: five enqueues around a throw).
          "enqueue", "register",
          // Words that are as often a noun or a builder as an action.
          "email", "notify", "emit", "dispatch", "transfer", "publish",
          "settle", "send", "upload", "register", "reserve", "grant"}) {
        if (callee.size() == v.size() && iprefix(callee, v)) return true;
    }
    return false;
}

// A receiver that names something state lives in. `db.begin()`,
// `session.flush()`, `repo.createOrder()` are durable; `line.begin()` is an
// iterator, `std::cout.flush()` drains a stream, `crypto.createHash()` is a
// factory, `app.add_option()` is a builder. The verb is the same; only the
// receiver tells them apart, so the ambiguous verbs require one.
bool looks_like_persistence_receiver(std::string_view q) {
    if (q.empty()) return false;
    std::string low(q);
    for (auto& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (std::string_view n :
         {"db", "database", "repo", "repository", "store", "dao", "orm",
          "prisma", "knex", "sequelize", "mongo", "sql", "session", "tx",
          "txn", "transaction", "conn", "connection", "client", "api",
          "service", "collection", "table", "model", "entity", "entities",
          "em", "unitofwork", "uow", "queue", "cache", "bucket", "storage"}) {
        if (low == n) return true;
        // `userRepo`, `orderService`, `dbConn`: the noun at either end.
        if (low.size() > n.size() &&
            (low.compare(low.size() - n.size(), n.size(), n) == 0 ||
             low.compare(0, n.size(), n) == 0)) {
            return true;
        }
    }
    return false;
}

// Verbs that are durable only on a persistence receiver. Elsewhere they are
// iterators, streams, factories and builders.
bool verb_needs_a_store(std::string_view callee) {
    for (std::string_view v : {"flush", "create", "add_", "register"}) {
        if (iprefix(callee, v)) return true;
    }
    return false;
}

WorkKind classify_work_callee(std::string_view callee) {
    if (callee.empty()) return WorkKind::None;

    // Compensation first — "rollback" must not be shadowed by any other
    // prefix, and an explicit undo is the thing that makes everything else
    // safe.
    static constexpr std::string_view rollback_prefixes[] = {
        "rollback", "roll_back", "abort", "discard"};
    for (auto p : rollback_prefixes) {
        if (iprefix(callee, p)) return WorkKind::TxRollback;
    }
    static constexpr std::string_view compensate_prefixes[] = {
        "undo", "revert", "compensate", "refund", "reverse", "restore",
        "cancel", "unwind", "rescind"};
    for (auto p : compensate_prefixes) {
        if (iprefix(callee, p)) return WorkKind::Compensate;
    }

    // Transaction boundaries. "begintx"/"begintransaction" also match the
    // bare "begin" prefix, which is intended.
    static constexpr std::string_view begin_prefixes[] = {
        "begintransaction", "begintx", "begin", "starttransaction",
        "start_transaction", "transaction"};
    for (auto p : begin_prefixes) {
        if (iprefix(callee, p)) return WorkKind::TxBegin;
    }
    static constexpr std::string_view commit_prefixes[] = {
        "commit", "savechanges", "save_changes", "flush"};
    for (auto p : commit_prefixes) {
        if (iprefix(callee, p)) return WorkKind::TxCommit;
    }

    // Irreversible: it has left the process. No compensating write exists,
    // only a second message saying sorry.
    static constexpr std::string_view irreversible_prefixes[] = {
        "charge", "capturepayment", "capturecharge", "capturefunds",
        "settle", "payout", "transfer", "withdraw",
        "publish", "broadcast", "emit", "notify", "sendmail", "sendemail",
        "send_mail", "send_email", "email", "sms", "upload", "dispatch"};
    for (auto p : irreversible_prefixes) {
        if (iprefix(callee, p)) return WorkKind::Irreversible;
    }

    // Durable or shared state changes. Undoing one means writing another.
    static constexpr std::string_view mutation_prefixes[] = {
        "save", "insert", "update", "delete", "remove", "destroy", "create",
        "put", "post", "persist", "store", "writefile", "write_file",
        "enqueue", "increment", "decrement", "add_", "register", "provision",
        "allocate", "reserve", "grant", "revoke_"};
    for (auto p : mutation_prefixes) {
        if (iprefix(callee, p)) return WorkKind::Mutation;
    }
    return WorkKind::None;
}

// -- Undo cost ----------------------------------------------------------------
//
// The question these rules ask is not "was the error reported" but "what did
// it cost". Three shapes, cheapest first to detect:
//
//   1. A transaction opened and committed with no rollback anywhere: an error
//      in between leaves the unit half-applied and nothing undoes it.
//   2. Several state changes with a fallible point among them and no
//      compensation: the torn write, and the one that happens in code with no
//      transaction at all.
//   3. Irreversible work before fallible work: the ordering that CREATES the
//      undo problem. Advice, so it scores low.
//
// Precision-first, matching classify_resource_pairing: a guarded rollback (in
// defer/finally/except) or any compensating call clears the function, because
// without dataflow we cannot prove the guard does not cover the ops.
void classify_work_pairing(const std::vector<WorkOp>& ops,
                           const std::vector<int>& throw_lines,
                           bool has_catch, uint32_t effects,
                           std::vector<EhFinding>& out) {
    if (ops.empty()) return;

    // Work only needs undoing if it OUTLIVES the call, and a verb match alone
    // does not establish that. The discriminator is whether the verb stands
    // ALONE: `values.update(...)`, `history.insert(0, r)`, `rows.pop()` are
    // collection operations on locals, while `updateInventory(...)` and
    // `insertChild(...)` name something that lives somewhere. Both match the
    // same prefix; only the compound form can be torn.
    //
    // A first cut let a bare verb count when the function showed any durable
    // effect elsewhere. That was too loose in exactly the way this analysis
    // cannot afford: requests' Session.send does real network I/O AND calls
    // history.insert/history.pop on a local list, and flask's routes_command
    // prints with click.echo AND calls rows.insert — both were reported as
    // torn writes. A verb's own shape is the only local evidence, so a bare
    // one never counts.
    //
    // Known false negative, accepted deliberately: `db.save(obj)` and
    // `db.insert(row)` spell a genuine durable write with a bare verb and are
    // missed. Precision wins here, matching classify_resource_pairing's bound
    // — a wrong torn-write claim sends someone hunting for data loss that
    // never happened.
    (void)effects;
    auto is_durable_work = [](const WorkOp& op) {
        if (op.kind != WorkKind::Mutation &&
            op.kind != WorkKind::Irreversible) {
            return false;
        }
        if (is_bare_ambiguous_verb(op.callee)) return false;
        // `createLazyLoader(...)`, `crypto.createHash(...)`,
        // `app.add_option(...)`: create/add_/register are factories and
        // builders unless the receiver is a store (trpc's router builders,
        // lci's own CLI11 main() with 45 add_option calls, an npm
        // postinstall's createHash all read as torn writes).
        if (verb_needs_a_store(op.callee) &&
            !looks_like_persistence_receiver(op.qualifier)) {
            return false;
        }
        return true;
    };

    bool has_rollback = false;
    bool has_compensate = false;
    bool has_guarded_undo = false;
    int begin_line = 0;
    int commit_line = 0;
    for (const auto& op : ops) {
        switch (op.kind) {
            case WorkKind::TxRollback:
                has_rollback = true;
                if (op.guarded) has_guarded_undo = true;
                break;
            case WorkKind::Compensate:
                has_compensate = true;
                if (op.guarded) has_guarded_undo = true;
                break;
            case WorkKind::TxBegin:
                // `line.begin()` is an iterator, but the finding needs a
                // commit as well, and `commit` is unambiguous: the pair is
                // what disambiguates begin, so begin itself is not gated.
                if (begin_line == 0) begin_line = op.line;
                break;
            case WorkKind::TxCommit:
                // `std::cout.flush()` drains a stream; `session.flush()`
                // commits. Bare flush needs a store; commit never does.
                if (iprefix(op.callee, "flush") && op.callee.size() == 5 &&
                    !looks_like_persistence_receiver(op.qualifier)) {
                    break;
                }
                commit_line = op.line;
                break;
            default:
                break;
        }
    }
    const bool compensated = has_rollback || has_compensate;

    // (1) Transaction with no undo path.
    if (begin_line != 0 && commit_line > begin_line && !compensated) {
        EhFinding f;
        f.signal = EhSignal::UncompensatedTransaction;
        f.severity = FindingSeverity::High;
        f.confidence = 0.8;
        f.line = begin_line;
        f.detail = "commit at line " + std::to_string(commit_line) +
                   ", no rollback on any path";
        out.push_back(std::move(f));
    }

    // (2) Torn write. Needs a fallible point BETWEEN state changes: work
    // before it landed, work after it never ran. A throw site is the explicit
    // form; a catch in the function means the fallible point is the try body
    // itself.
    if (!compensated) {
        // Group by alternative arm before counting. Ops in sibling switch
        // cases or else-arms never both run, so they are not a sequence and
        // cannot leave each other half-applied — gin's SetMode stores four
        // modes in four cases around a panic, which read as a torn write
        // until the arms were separated. Conservative by design: a change at
        // statement level plus one inside an arm is not paired, matching the
        // analyzer's precision-first bound.
        absl::flat_hash_map<uint32_t, std::vector<const WorkOp*>> by_arm;
        for (const auto& op : ops) {
            if (is_durable_work(op)) by_arm[op.branch_id].push_back(&op);
        }
        // Deterministic: report the earliest-starting arm (karpathy #4).
        const std::vector<const WorkOp*>* worst = nullptr;
        for (const auto& [arm, changes] : by_arm) {
            if (changes.size() < 2) continue;
            bool fallible_between = has_catch;
            for (int t : throw_lines) {
                if (t > changes.front()->line && t < changes.back()->line) {
                    fallible_between = true;
                }
            }
            if (!fallible_between) continue;
            if (worst == nullptr ||
                changes.front()->line < (*worst).front()->line) {
                worst = &changes;
            }
        }
        if (worst != nullptr) {
            EhFinding f;
            f.signal = EhSignal::PartialWriteRisk;
            f.severity = FindingSeverity::High;
            f.confidence = 0.6;
            f.line = worst->front()->line;
            f.detail = std::to_string(worst->size()) +
                       " state changes through line " +
                       std::to_string(worst->back()->line) +
                       ", no compensation";
            out.push_back(std::move(f));
        }
    }

    // (3) Irreversible before fallible. A guarded undo does not excuse this —
    // there is no undo for a sent email — but an explicit compensating call
    // means the author already thought about it.
    if (!has_compensate) {
        for (const auto& op : ops) {
            if (op.kind != WorkKind::Irreversible || op.guarded) continue;
            if (!is_durable_work(op)) continue;
            int fallible_after = 0;
            for (int t : throw_lines) {
                if (t > op.line && (fallible_after == 0 || t < fallible_after)) {
                    fallible_after = t;
                }
            }
            if (fallible_after == 0) continue;
            EhFinding f;
            f.signal = EhSignal::IrreversibleBeforeFallible;
            f.severity = FindingSeverity::Low;
            f.confidence = 0.5;
            f.line = op.line;
            f.detail = op.callee + " cannot be recalled; line " +
                       std::to_string(fallible_after) + " can still fail";
            out.push_back(std::move(f));
            break;  // one per function: the ordering is the finding
        }
    }
}

// Functions whose NAME promises a sentinel on failure. `isValidIPv6` returning
// false is not a swallow — false is the answer, and the exception was the
// mechanism for computing it. `tryStat` returning undefined is the documented
// contract of every try-prefixed API since TryParse.
//
// Verified against the corpus: express's tryStat (catch -> undefined around
// fs.statSync) and four zod validators (isValidIPv6, isValidBase64,
// isValidJWT — catch -> false around a URL/JSON parse) were the entire
// real-world yield of the sentinel rule, and every one of them was correct
// code doing exactly what its name says.
bool name_promises_a_sentinel(std::string_view fn) {
    if (fn.empty()) return false;
    for (std::string_view p :
         {"is", "has", "can", "should", "was", "were", "are", "try",
          "maybe", "opt", "check", "test", "supports", "contains",
          "matches", "looks", "ignore"}) {
        if (!iprefix(fn, p)) continue;
        // Require a capital or underscore after the prefix so `issue()` and
        // `canvas()` are not mistaken for predicates.
        if (fn.size() == p.size()) return true;
        char next = fn[p.size()];
        if (next == '_' || (next >= 'A' && next <= 'Z')) return true;
    }
    // Snake case: is_valid, has_key, try_parse.
    for (std::string_view p :
         {"is_", "has_", "can_", "should_", "try_", "check_", "test_"}) {
        if (iprefix(fn, p)) return true;
    }
    // Suffixes that promise a sentinel just as loudly as a prefix does:
    // axios's `stringifySafely` returns '' on failure, and Kotlin/Rust-style
    // `getOrNull` / `parseOrDefault` say it outright.
    for (std::string_view suf :
         {"safe", "safely", "quietly", "silently", "ornull", "ornil",
          "ornone", "ordefault", "orelse", "orempty", "orzero", "orfalse",
          "_safe", "noexcept", "ifsupported", "ifavailable", "ifpossible"}) {
        if (fn.size() >= suf.size() &&
            iprefix(fn.substr(fn.size() - suf.size()), suf)) {
            return true;
        }
    }
    return fn.find("valid") != std::string_view::npos ||
           fn.find("Valid") != std::string_view::npos;
}

// Teardown may not throw: .NET's Dispose guideline, PHP's __destruct, Java's
// close(), a finalizer. A throw there masks the original failure or kills
// the finalizer thread, so catching and continuing is the documented
// behavior of the method. serilog's sinks catch-log-continue in Dispose on
// purpose; guzzle's CurlMultiHandler::__destruct swallows Throwable.
bool is_cleanup_method(std::string_view fn) {
    // Strip a C++ qualifier: `IndexServer::shutdown_locked`.
    if (auto q = fn.rfind("::"); q != std::string_view::npos) fn = fn.substr(q + 2);
    for (std::string_view n :
         {"dispose", "__destruct", "close", "finalize", "shutdown",
          "teardown", "cleanup", "destroy", "release"}) {
        // As a whole word in snake or camel case: shutdown_locked,
        // handle_shutdown, onClose, closeQuietly — not "closest".
        for (size_t at = 0; (at = fn.find(n, at)) != std::string_view::npos; ++at) {
            // Case-insensitive find is not available; check both spellings.
            bool start_ok = at == 0 || fn[at - 1] == '_' ||
                            !std::isalpha(static_cast<unsigned char>(fn[at - 1])) ||
                            std::islower(static_cast<unsigned char>(fn[at - 1]));
            size_t end = at + n.size();
            bool end_ok = end == fn.size() || fn[end] == '_' ||
                          std::isupper(static_cast<unsigned char>(fn[end])) ||
                          !std::isalpha(static_cast<unsigned char>(fn[end]));
            if (start_ok && end_ok) return true;
        }
        // Capitalized spelling (Dispose, Close, onClose).
        std::string cap(n);
        cap[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(cap[0])));
        for (size_t at = 0; (at = fn.find(cap, at)) != std::string_view::npos; ++at) {
            size_t end = at + n.size();
            bool end_ok = end == fn.size() || fn[end] == '_' ||
                          std::isupper(static_cast<unsigned char>(fn[end])) ||
                          !std::isalpha(static_cast<unsigned char>(fn[end]));
            if (end_ok) return true;
        }
    }
    return false;
}

void classify_catch_site(const CatchSiteInfo& site, std::string_view fn_name,
                         std::vector<EhFinding>& out) {
    // The site was judged before the body was read: the developer marked
    // the discard (`catch (E ignored)`), or the function's name promises
    // absorption (closeQuietly, ignoreIoExceptions, toHttpUrlOrNull). The
    // sentinel rule has read those names since express/zod; okhttp showed
    // the same names wrapping empty and continue-shaped catches.
    if (site.explicit_discard || name_promises_a_sentinel(fn_name)) return;

    const bool cleanup = is_cleanup_method(fn_name);
    auto add = [&](EhSignal sig, FindingSeverity sev, double conf) {
        if (cleanup && sev != FindingSeverity::Low) sev = FindingSeverity::Low;
        EhFinding f;
        f.signal = sig;
        f.severity = sev;
        f.confidence = conf;
        f.line = site.line;
        if (!site.caught_type.empty()) f.detail = "caught=" + site.caught_type;
        out.push_back(std::move(f));
    };

    // Two questions, scored separately: did the error LEAVE this block, and
    // how much of it survived the trip. A handler that forwards `e.message`
    // has answered the first and failed the second — the failure is reported,
    // and nobody can act on the report.
    auto note_fidelity = [&](CauseFidelity f) {
        if (f == CauseFidelity::Lossy && !out.empty()) {
            auto& last = out.back();
            last.detail = last.detail.empty()
                              ? "message only"
                              : last.detail + ", message only";
        }
    };

    // A catch for the normal end of a protocol — TimeoutError driving a
    // keepalive, EndOfStream, WebSocketDisconnect — handles a condition, not
    // a failure. Nothing below applies; fastapi's SSE keepalive loop and
    // websocket tutorial were two high findings each on this shape alone.
    if (site.normal_condition) return;

    const bool kept_cause_on_rethrow = site.has_rethrow && site.rethrow_uses_cause;

    if (site.body_empty) {
        // `catch (const json::exception&) { /* litter */ }` names the
        // failure it ignores; `catch (...) {}` and `except: pass` name
        // nothing. Same split as typed recovery.
        if (site.specific_type) {
            add(EhSignal::EmptyCatch, FindingSeverity::Med, 0.7);
            out.back().detail += ", typed";
        } else {
            add(EhSignal::EmptyCatch, FindingSeverity::High, 0.9);
        }
    } else if (site.has_rethrow) {
        if (!site.rethrow_uses_cause)
            add(EhSignal::RethrowNoCause, FindingSeverity::Low, 0.4);
    } else if (site.propagates_cause) {
        // The caught error is handed to a call, a constructor, a store, or
        // a yield — a callback, a promise rejection, a wrapper assigned out,
        // an error collected for the caller. It left the block; the route
        // just was not `throw`. Calling that a swallow made express's only
        // finding, four of axios's five, and flask's request-dispatch pair
        // false positives. Checked BEFORE the sentinel return: trpc's ws
        // adapter wraps the cause, responds with it, then `return []` — the
        // sentinel is the value of a handled failure, not a renamed one.
        if (site.propagated_fidelity == CauseFidelity::Lossy) {
            // PARTIAL credit: forwarding beats swallowing, so the deduction is
            // well under CatchAndContinue's, but the stack and the cause chain
            // (InnerException, getCause, __cause__) stopped here and the
            // receiver cannot reconstruct them.
            add(EhSignal::LossyPropagation, FindingSeverity::Med, 0.5);
            out.back().detail = "message only, stack and cause chain lost";
        }
    } else if (site.has_return && !site.returns_sentinel) {
        // Error may be surfaced through the return value — no swallow claim.
    } else if (site.returns_sentinel && !name_promises_a_sentinel(fn_name)) {
        // `catch (e) { return null; }`. The failure did not propagate; it was
        // renamed "no result", and the caller cannot tell an empty answer
        // from a broken one. Med rather than High: returning a sentinel is a
        // deliberate contract in lookup-style APIs, and the caller at least
        // sees SOMETHING is absent — unlike a bare continue.
        // A sentinel for a NAMED failure (`catch (NumberFormatException e)
        // { return -1; }`) is the anticipated-failure shape: same confidence
        // cut as typed recovery.
        add(EhSignal::ErrorToSentinel, FindingSeverity::Med,
            site.specific_type ? 0.5 : 0.7);
        if (site.specific_type) {
            out.back().detail = out.back().detail.empty()
                                    ? "typed"
                                    : out.back().detail + ", typed";
        }
    } else if (site.returns_sentinel) {
        // The name promised the sentinel (tryX, isX, getOrNull): the contract.
    } else if (site.has_log_call) {
        // Logging the bare error prints a stack; logging `e.message` prints a
        // sentence. Same swallow, materially different diagnosability, so the
        // confidence (and thus the deduction) splits. Recovery work beside
        // the log does not make it a blind catch-and-continue: the error
        // was reported, then handled.
        bool lossy = site.logged_fidelity == CauseFidelity::Lossy;
        add(EhSignal::LogAndSwallow, FindingSeverity::Med, lossy ? 0.75 : 0.5);
        note_fidelity(site.logged_fidelity);
    } else if (site.specific_type) {
        // `except NameError: signature = fallback(call)`. The author named
        // the failure they expected and wrote the recovery for it; the
        // cause is still gone, but this is the anticipated-failure shape,
        // not a blind catch. Med, and under log-and-swallow's confidence.
        add(EhSignal::CatchAndContinue, FindingSeverity::Med, 0.5);
        out.back().detail = out.back().detail.empty()
                                ? "typed recovery"
                                : out.back().detail + ", typed recovery";
    } else {
        add(EhSignal::CatchAndContinue, FindingSeverity::High, 0.7);
    }

    // Breadth is only a defect when something is lost by it. `except
    // Exception as e: raise HTTPException(...) from e` translates anything
    // into the API's error and keeps the chain — the breadth is the point.
    // fastapi charged six of these, every one a chained re-raise. Handing
    // the error to a call does NOT clear it: `except Exception as e:
    // recover(e)` still catches SystemExit and KeyboardInterrupt on the way.
    if (site.broad_type && !kept_cause_on_rethrow) {
        add(EhSignal::BroadCatch, FindingSeverity::Med, 0.6);
    }
}

void classify_resource_pairing(const std::vector<ResourceOp>& acquires,
                               const std::vector<ResourceOp>& releases,
                               const std::vector<int>& throw_lines,
                               bool returns_value,
                               std::vector<EhFinding>& out) {
    if (acquires.empty()) return;

    if (releases.empty()) {
        // A value-returning function is (syntactically) a factory: the
        // acquired resource may escape via the return. No dataflow layer
        // (design bound), so precision wins — suppress rather than guess.
        if (returns_value) return;
        for (const auto& a : acquires) {
            if (a.guarded) continue;  // e.g. Python `with` scope frees it
            EhFinding f;
            f.signal = EhSignal::LeakNoRelease;
            f.severity = FindingSeverity::Med;
            f.confidence = 0.6;
            f.line = a.line;
            f.detail = "acquire=" + a.callee;
            out.push_back(std::move(f));
        }
        std::sort(out.begin(), out.end(),
                  [](const EhFinding& x, const EhFinding& y) {
                      return x.line < y.line;
                  });
        return;
    }

    bool all_guarded = true;
    for (const auto& r : releases) {
        if (!r.guarded) all_guarded = false;
    }
    if (all_guarded) return;

    // Bare (unguarded) releases in a function that can also bail between
    // acquire and release: line-order heuristic, no CFG (design doc bound).
    int first_acquire = acquires.front().line;
    for (const auto& a : acquires) {
        first_acquire = std::min(first_acquire, a.line);
    }
    for (const auto& r : releases) {
        if (r.guarded) continue;
        bool throw_in_window = false;
        int throw_at = 0;
        for (int tl : throw_lines) {
            if (tl > first_acquire && tl < r.line) {
                throw_in_window = true;
                throw_at = tl;
                break;
            }
        }
        EhFinding f;
        f.line = r.line;
        if (throw_in_window) {
            f.signal = EhSignal::LeakOnErrorPath;
            f.severity = FindingSeverity::Med;
            f.confidence = 0.5;
            f.detail = "release at :" + std::to_string(r.line) +
                       " not guarded, throw at :" + std::to_string(throw_at);
        } else if (!throw_lines.empty()) {
            f.signal = EhSignal::UnguardedRelease;
            f.severity = FindingSeverity::Low;
            f.confidence = 0.4;
            f.detail = r.callee + " outside finally/defer";
        } else {
            continue;  // straight-line function, bare release is fine
        }
        out.push_back(std::move(f));
    }
    std::sort(out.begin(), out.end(),
              [](const EhFinding& x, const EhFinding& y) {
                  return x.line < y.line;
              });
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
    info.error_handling.catch_count = static_cast<int>(ctx.catch_sites.size());
    info.error_handling.error_return_lines = ctx.error_return_lines;
    info.has_error_handling = true;

    // Swallow findings: dropped errors recorded inline + catch-site
    // classification, sorted by line (karpathy #4: deterministic).
    info.error_findings = ctx.error_findings;
    for (const auto& site : ctx.catch_sites) {
        classify_catch_site(site, info.function_name, info.error_findings);
    }
    // NOTE: the sort lives after classify_work_pairing below — undo-cost
    // findings land in this same vector and must share the ordering
    // (karpathy #4: sort before emit).

    // Resource pairing findings.
    info.resource_acquires = ctx.resource_acquires;
    info.resource_releases = ctx.resource_releases;
    std::vector<int> throw_lines;
    throw_lines.reserve(ctx.throw_sites.size());
    for (const auto& ts : ctx.throw_sites) throw_lines.push_back(ts.line);
    classify_resource_pairing(ctx.resource_acquires, ctx.resource_releases,
                              throw_lines, ctx.returns_value,
                              info.resource_findings);

    // Undo cost: what an error here would leave half-done. Emitted into
    // error_findings (they are error-handling defects, not leaks) and sorted
    // with the rest below.
    info.work_ops = ctx.work_ops;
    classify_work_pairing(ctx.work_ops, throw_lines, !ctx.catch_sites.empty(),
                          ctx.side_effects, info.error_findings);

    // Developer overrides last: a directive names the rule and the line the
    // report printed, so it is matched against the finished findings.
    if (!suppressions_.empty()) {
        auto drop = [&](std::vector<EhFinding>& v) {
            std::erase_if(v, [&](const EhFinding& f) {
                return suppressions_.suppresses(f.line, to_string(f.signal));
            });
        };
        drop(info.error_findings);
        drop(info.resource_findings);
    }

    // Every error-handling finding is in now; one total order over the lot.
    std::sort(info.error_findings.begin(), info.error_findings.end(),
              [](const EhFinding& a, const EhFinding& b) {
                  if (a.line != b.line) return a.line < b.line;
                  return a.signal < b.signal;
              });

    // Extract parameter writes. A base identifier that is not in
    // ctx.parameters has no known position -- that happens when the write
    // reaches the parameter through an alias or a construct the extractor
    // did not tie back to the signature. It used to fall through as index 0,
    // which reads as a confident claim that the FIRST parameter was mutated.
    // kUnknownParameterIndex says "unknown" instead, and such a write is
    // deliberately kept out of mutated_parameters below (the mutation is
    // real, its position is not known).
    absl::flat_hash_map<int, bool> param_index_set;
    for (const auto& access : ctx.accesses) {
        if (access.type == AccessType::Write &&
            access.target_type == AccessTarget::Parameter) {
            int idx = kUnknownParameterIndex;
            auto it = ctx.parameters.find(access.base_identifier);
            if (it != ctx.parameters.end()) idx = it->second;

            info.parameter_writes.push_back(ParameterWriteInfo{
                access.base_identifier, idx, access.line, access.column,
                access.field_path, false});
            if (idx != kUnknownParameterIndex) param_index_set[idx] = true;
        }
    }

    // Extract global writes. Mirrors the closure handling in
    // populate_purity_classification: nothing else fills this vector, so
    // purity_classification.mutated_globals was always empty however many
    // globals a function wrote. is_package stays false -- the extractor does
    // not distinguish package-level from file-level globals.
    for (const auto& access : ctx.accesses) {
        if (access.type == AccessType::Write &&
            access.target_type == AccessTarget::Global) {
            info.global_writes.push_back(GlobalWriteInfo{
                access.base_identifier, access.line, access.column,
                access.field_path, false});
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

void SideEffectAnalyzer::record_error_return(int line) {
    if (!current_func_) return;
    current_func_->returns_error = true;
    current_func_->error_return_lines.push_back(line);
}

void SideEffectAnalyzer::record_return_value() {
    if (current_func_) current_func_->returns_value = true;
}

void SideEffectAnalyzer::record_catch(const CatchSiteInfo& site) {
    if (current_func_) current_func_->catch_sites.push_back(site);
}

void SideEffectAnalyzer::record_dropped_error(int line,
                                              std::string_view detail) {
    if (!current_func_) return;
    EhFinding f;
    f.signal = EhSignal::DroppedError;
    // `_ = f()` is the developer writing "I know". errcheck, the Go
    // community's own tool, excludes the explicit blank by default (-blank
    // turns it on), and pocketbase's 29 were all cleanup or best-effort
    // calls. Low: it is worth a glance, not a high-severity defect. The
    // unchecked form errcheck does report — a bare `f()` statement — needs
    // return types this analysis does not have, and is a known gap.
    f.severity = FindingSeverity::Low;
    f.confidence = 0.5;
    f.line = line;
    f.detail = std::string(detail);
    current_func_->error_findings.push_back(std::move(f));
}

void SideEffectAnalyzer::record_call_site_resources(std::string_view callee,
                                                    int line, bool guarded,
                                                    uint32_t branch_id,
                                                    std::string_view qualifier) {
    if (!current_func_) return;
    // Undo-cost model: what state this call changed, in source order. Kept
    // beside the resource pairing because both answer "what is outstanding
    // when an error fires", one for handles and one for state.
    if (WorkKind wk = classify_work_callee(callee); wk != WorkKind::None) {
        current_func_->work_ops.push_back(
            WorkOp{std::string(callee), line, wk, guarded, branch_id,
                   std::string(qualifier)});
    }
    switch (classify_resource_callee(callee)) {
        case ResourceOpKind::Acquire:
            current_func_->resource_acquires.push_back(
                ResourceOp{std::string(callee), line, guarded});
            break;
        case ResourceOpKind::Release:
            current_func_->resource_releases.push_back(
                ResourceOp{std::string(callee), line, guarded});
            break;
        case ResourceOpKind::None:
            break;
    }
}

void SideEffectAnalyzer::record_finally_hijack(int line,
                                               std::string_view verb) {
    if (!current_func_) return;
    EhFinding f;
    f.signal = EhSignal::FinallyHijacksControlFlow;
    // High and near-certain: unlike most signals here this is not a judgment
    // about intent. A return in a finally DOES discard the exception, in
    // every language that has the construct, and it is invisible at the call
    // site — there is no catch for a reader to find.
    f.severity = FindingSeverity::High;
    f.confidence = 0.85;
    f.line = line;
    f.detail = std::string(verb) + " in finally discards any in-flight error";
    current_func_->error_findings.push_back(std::move(f));
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
    auto rt_snap = ref.pin();
    for (FileID fid : indexer.get_all_file_ids()) {
        std::string file_path = indexer.get_file_path(fid);
        for (const auto& es : rt_snap->get_file_enhanced_symbols(fid)) {
            if (!es) continue;
            bool is_callable = es->symbol.type == SymbolType::Function ||
                               es->symbol.type == SymbolType::Method ||
                               es->symbol.type == SymbolType::Constructor;
            if (!is_callable) continue;

            uint32_t cats = side_effect::kNone;
            for (const auto& callee : ref.get_callee_names(es->id)) {
                cats |= classify_callee_category(callee);
            }

            std::string key = file_path + ":" +
                              std::to_string(es->symbol.line) + ":0";

            // If the AST pass already recorded precise local effects for this
            // function (param / receiver / global writes, throws, channel ops),
            // keep them and merely OR in the callee-name heuristic categories
            // (IO / network / database / throw the AST can't see from a bare
            // call node). Never discard AST precision by overwriting.
            if (auto it = results_.find(key); it != results_.end()) {
                it->second.categories |= cats;
                uint32_t combined =
                    it->second.categories | it->second.transitive_categories;
                it->second.is_pure = (combined == side_effect::kNone);
                continue;
            }

            SideEffectInfo info;
            info.function_name = std::string(es->symbol.name);
            info.file_path = file_path;
            info.start_line = es->symbol.line;
            info.end_line = es->symbol.end_line;

            info.categories = cats;
            info.is_pure = (cats == side_effect::kNone);
            info.purity_level = info.is_pure ? PurityLevel::Pure
                                              : PurityLevel::ExternalDependency;
            info.confidence = PurityConfidence::Medium;
            info.purity_score = info.is_pure ? 1.0 : 0.0;
            info.purity_confidence_score = 0.7;

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
    auto rt_snap = ref.pin();
    for (FileID fid : indexer.get_all_file_ids()) {
        std::string file_path = indexer.get_file_path(fid);
        for (const auto& es : rt_snap->get_file_enhanced_symbols(fid)) {
            if (!es) continue;
            std::string key =
                file_path + ":" + std::to_string(es->symbol.line) + ":0";
            auto it = results_.find(key);
            if (it != results_.end()) by_symbol[es->id] = &it->second;
        }
    }

    // Confidence decay along the call graph (Go SideEffectPropagator parity):
    // a function that directly holds a propagatable effect is a source at full
    // confidence; each hop outward toward callers multiplies confidence by
    // kConfidenceDecay, floored at kMinConfidence. Names/values mirror Go's
    // ConfidenceDecay / MinConfidence exactly for auditability.
    constexpr double kInitialConfidence = 1.0;
    constexpr double kConfidenceDecay = 0.95;
    constexpr double kMinConfidence = 0.3;

    // Seed each direct source with full confidence before the fixpoint.
    for (auto& [sid, info] : by_symbol) {
        (void)sid;
        if (categories_to_propagate(info->categories) != 0)
            info->transitive_confidence = kInitialConfidence;
    }

    // Fixpoint: push each symbol's effects upstream to its callers' transitive
    // set until nothing changes. Confidence at a caller is the best (highest,
    // i.e. shortest-hop) decayed value across incoming impure paths. Bounded
    // iterations guard against cycles.
    constexpr int kMaxIterations = 100;
    bool changed = true;
    for (int iter = 0; changed && iter < kMaxIterations; ++iter) {
        changed = false;
        for (auto& [sid, info] : by_symbol) {
            uint32_t combined = info->categories | info->transitive_categories;
            uint32_t to_propagate = categories_to_propagate(combined);
            if (to_propagate == 0) continue;
            double caller_confidence =
                std::max(kMinConfidence,
                         info->transitive_confidence * kConfidenceDecay);
            for (SymbolID caller_id : ref.get_caller_symbols(sid)) {
                auto cit = by_symbol.find(caller_id);
                if (cit == by_symbol.end()) continue;
                SideEffectInfo* caller = cit->second;
                uint32_t old = caller->transitive_categories;
                caller->transitive_categories |= to_propagate;
                if (caller->transitive_categories != old) changed = true;
                if (caller_confidence > caller->transitive_confidence) {
                    caller->transitive_confidence = caller_confidence;
                    changed = true;
                }
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

    // Deterministic violation order (Karpathy rule 4): by_target is an absl
    // hash map, so the emitted violations came out in per-process hash order.
    std::vector<std::string> targets;
    targets.reserve(by_target.size());
    for (const auto& [target, _] : by_target) targets.push_back(target);
    std::sort(targets.begin(), targets.end());

    for (const auto& target : targets) {
        auto& target_accesses = by_target[target];
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
