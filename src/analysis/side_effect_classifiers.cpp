#include <lci/analysis/side_effect_analyzer.h>

#include <lci/core/reference_tracker.h>
#include <lci/indexing/master_index.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace lci {

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

// A receiver that names a helper bag rather than a place state lives:
// `app.utils.deleteByPath(...)` mutates an in-memory object. The inverse of
// looks_like_persistence_receiver, and it outranks the verb entirely.
bool looks_like_utility_receiver(std::string_view q) {
    if (q.empty()) return false;
    std::string low(q);
    for (auto& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (std::string_view n :
         {"utils", "util", "helpers", "helper", "lodash", "_", "commonhelper",
          "object", "array", "string", "strings", "path", "url", "math",
          "json", "reflect", "assert"}) {
        if (low == n) return true;
        if (low.size() > n.size() &&
            low.compare(low.size() - n.size(), n.size(), n) == 0) {
            return true;
        }
    }
    return false;
}

// Verbs that are durable only on a persistence receiver. Elsewhere they are
// iterators, streams, factories and builders.
bool verb_needs_a_store(std::string_view callee) {
    // "post" is here because English "post-" (after: post_process,
    // post_init, rack's frame.post_context) is a false friend of the
    // publish verb; only `client.post(...)`-shaped calls keep it.
    for (std::string_view v : {"flush", "create", "add_", "register", "post"}) {
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
        // A receiver-gated verb lives or dies on its receiver alone:
        // `client.post(...)` is the HTTP verb even though bare "post" would
        // otherwise be ambiguous; `post_process(x)` has no store and drops.
        if (verb_needs_a_store(op.callee)) {
            return looks_like_persistence_receiver(op.qualifier) &&
                   !looks_like_utility_receiver(op.qualifier);
        }
        if (is_bare_ambiguous_verb(op.callee)) return false;
        // `createLazyLoader(...)`, `crypto.createHash(...)`,
        // `app.add_option(...)`: create/add_/register are factories and
        // builders unless the receiver is a store (trpc's router builders,
        // lci's own CLI11 main() with 45 add_option calls, an npm
        // postinstall's createHash all read as torn writes).
        if (looks_like_utility_receiver(op.qualifier)) return false;
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
         {"dispose", "__destruct", "__del__", "close", "finalize", "shutdown",
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
        // The level rides along: prod log configs routinely drop debug and
        // info, so `level=debug` means the report vanishes exactly when the
        // incident happens. Annotation only — severity is unchanged until a
        // calibration round says otherwise.
        if (!site.log_level.empty()) {
            out.back().detail = out.back().detail.empty()
                                    ? "level=" + site.log_level
                                    : out.back().detail + ", level=" +
                                          site.log_level;
        }
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

}  // namespace lci
