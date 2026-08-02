"""One convention for latest-wins, attempt history, and arm pairing.

Three consumers previously implemented this independently with divergent
semantics (scorer, claim_validation, the claim scoring CLI). The single
convention, shared by all of them:

  * Latest-wins per run_key with a completion guard: once a key holds a
    terminal COMPLETED record (answered / tool_violation / malformed_output),
    a later retryable retry (timeout / provider_error / config_error) must
    not erase it; among completed retries the latest wins.
  * A task/seed cell pairs when BOTH arms' surviving records are terminal
    completions. A completed terminal failure is a real (failed) completion
    of its arm -- it participates in paired rates and drags them down --
    while a retryable failure leaves the whole cell unpaired so the run can
    still be retried.
"""

from runner.record import COMPLETED_STATUSES


class IncompatibleRuns(Exception):
    """Records that cannot be aggregated together were folded into one
    aggregate without the caller explicitly opting in -- fail loud."""


def _completed(entry):
    return entry.get("status") in COMPLETED_STATUSES


def collect_latest(entries, attempt_view=None):
    """Return ``(latest, attempts)``: the surviving record per run_key plus
    the full per-key attempt history.

    ``attempt_view(entry) -> list`` customises the recorded attempt rows;
    when omitted, no attempt history is collected.
    """
    latest = {}
    attempts = {}
    for entry in entries:
        key = entry.get("run_key")
        if key is None:
            raise IncompatibleRuns("score record is missing required run_key")
        if attempt_view is not None:
            attempts.setdefault(key, []).extend(attempt_view(entry))
        previous = latest.get(key)
        if previous is None or _completed(entry) or not _completed(previous):
            latest[key] = entry
    return list(latest.values()), attempts


def pair_cells(latest, completed_statuses=COMPLETED_STATUSES):
    """Pair task/seed cells across arms; return ``(paired, unpaired)``.

    ``completed_statuses`` names the terminal completions that make a cell
    pairable; ``None`` pairs on presence alone (the edit scorer's own status
    vocabulary is judged by its gates before scores reach pairing)."""

    def _cell_completed(entry):
        return (completed_statuses is None
                or entry.get("status") in completed_statuses)
    cells = {}
    for entry in latest:
        cell = (entry.get("task_id"), entry.get("seed"))
        arms = cells.setdefault(cell, {})
        arm = entry.get("arm")
        if arm in arms:
            raise IncompatibleRuns(
                "multiple run keys claim the same task/seed/arm cell: "
                f"{arms[arm].get('run_key')!r} and {entry.get('run_key')!r}"
            )
        arms[arm] = entry
    paired = []
    unpaired = []
    for cell, arms in sorted(
        cells.items(), key=lambda item: (str(item[0][0]), str(item[0][1]))
    ):
        treatment = arms.get("treatment")
        baseline = arms.get("baseline")
        if (treatment is not None and baseline is not None
                and _cell_completed(treatment) and _cell_completed(baseline)):
            paired.extend((treatment, baseline))
            continue
        for arm in sorted(arms, key=str):
            unpaired.append({
                "task_id": cell[0],
                "seed": cell[1],
                "arm": arm,
                "run_key": arms[arm].get("run_key"),
                "status": arms[arm].get("status"),
            })
    return paired, unpaired
