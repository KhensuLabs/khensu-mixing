# local-services/khensu_services/copilot/batch_smart.py
"""Per-file smart batch (ENH-4): analyse, fix and master each file on its own terms.

The plain batch (``router_render_batch``) applies ONE chain to every file. This
one treats every file separately, reusing the existing building blocks:

1. **Measure** the file (``agent_loop.measure``) and derive its findings
   (``agent_loop.derive_findings``) - the measured findings whose fix the agent
   loop can render and verify. Advice-only findings (no renderable parameters)
   and denoise are not part of that set, so they are never acted on here.
2. **Fix** (optional): the verified agent loop (``agent_loop.run_agent``) -
   one step per finding, each rendered through the shared safety pipeline
   (validator, quarantine, post-render guardrail) and kept only when it moved
   its metric without making another worse.
3. **Master** (optional): the self-correcting mastering loop
   (``mastering_agent.run_mastering_agent``) toward the genre's targets in the
   chosen style; its best safe candidate is re-checked against the ORIGINAL
   file and dropped when the guardrail fails.
4. **Write** the result as ``<name><suffix>.wav`` in the output folder and
   measure it again: loudness and true peak before / after, findings before,
   after and cleared.

Blocking; the router runs it on the long-job pool. Cancellation is checked
between steps and inside the renders.
"""
from __future__ import annotations

from khensu_services.common.job_status import JobStatus
import logging
import math
import os
import shutil
from typing import Any, Callable

from khensu_services.copilot import agent_loop

_log = logging.getLogger(__name__)

AUDIO_EXTS = {".wav", ".flac", ".aif", ".aiff", ".ogg", ".mp3"}


class SmartBatchCancelled(Exception):
    pass


def _finite(value: Any) -> float | None:
    try:
        v = float(value)
    except (TypeError, ValueError):
        return None
    return v if math.isfinite(v) else None


def finding_specs(targets: dict[str, list[float]] | None, target_lufs: float) -> dict:
    """The metric ranges the agent loop judges against: ``targets`` over the
    defaults, loudness as ``target_lufs`` +- 1 LU unless a target names it
    (exactly as ``agent_loop.run_agent`` resolves them)."""
    specs = agent_loop.apply_targets(targets)
    if "lufs" not in (targets or {}):
        s = specs["lufs"]
        specs["lufs"] = agent_loop.MetricSpec(s.key, s.label, s.unit, target_lufs - 1.0,
                                              target_lufs + 1.0, s.tolerance, s.min_gain, s.matched)
    return specs


def fixable_findings(metrics: dict, specs: dict, target_lufs: float,
                     include_loudness: bool) -> list[dict]:
    """Findings with a renderable fix (advice-only ones are left out)."""
    return [f for f in agent_loop.derive_findings(metrics, specs, target_lufs, include_loudness)
            if agent_loop.chain_from_suggested_parameters(f.get("suggested_parameters") or {})]


def unique_output_name(stem: str, suffix: str, used: set[str]) -> str:
    """``<stem><suffix>.wav``, numbered when two sources share a stem."""
    name = f"{stem}{suffix}.wav"
    n = 2
    while name.lower() in used:
        name = f"{stem}{suffix}_{n}.wav"
        n += 1
    used.add(name.lower())
    return name


def _require_finite_samples(src: str) -> None:
    """Refuse a file whose samples are not all finite (NaN / infinity): no
    measurement or render of it means anything. The decode is the shared,
    cached one the measurement reads next."""
    import numpy as np

    from khensu_services.analysis.context import load_audio

    y, _sr = load_audio(src, sr=None, mono=False)
    if not np.all(np.isfinite(y)):
        raise ValueError("The file has non-finite samples (NaN or infinity); it was not processed.")


def process_file(src: str, out_path: str, work_dir: str, *,
                 fix: bool, master: bool, genre: dict, style: str, strength: float,
                 target_lufs: float, include_loudness: bool, targets: dict | None,
                 max_steps: int, max_iterations: int,
                 render_step: Callable[[str, str, list[dict]], dict],
                 should_cancel: Callable[[], bool],
                 on_stage: Callable[[str], None] | None = None) -> dict:
    """Analyse, fix and master one file. Returns its result entry."""
    from khensu_services.common.output_paths import reject_output_alias, validate_output_path

    def stage(text: str) -> None:
        if on_stage:
            on_stage(text)

    def check_cancel() -> None:
        if should_cancel():
            raise SmartBatchCancelled()

    entry: dict[str, Any] = {"file_path": src, "output_path": "", "status": JobStatus.FAILED.value,
                             "kept_steps": 0, "mastered": False, "cleared": [], "remaining": []}
    validate_output_path(out_path, src)
    reject_output_alias(out_path, src)
    os.makedirs(work_dir, exist_ok=True)

    stage("analysing")
    _require_finite_samples(src)
    specs = finding_specs(targets, target_lufs)
    before = agent_loop.measure(src)
    findings_before = fixable_findings(before, specs, target_lufs, include_loudness)
    entry.update(before_lufs=_finite(before.get("lufs")),
                 before_true_peak_dbtp=_finite(before.get("true_peak_dbtp")),
                 findings_before=len(findings_before))

    current = src
    if fix and findings_before:
        check_cancel()
        stage("fixing")
        try:
            result = agent_loop.run_agent(
                src, work_dir, job_tag="batch", targets=targets, target_lufs=target_lufs,
                # Mastering sets the loudness: the fix pass leaves it alone then.
                include_loudness=include_loudness and not master, max_steps=max_steps,
                render_step=render_step, should_cancel=should_cancel,
                on_progress=lambda st: stage("fixing: " + str(st.get("stage", ""))))
        except agent_loop.AgentCancelled as exc:
            raise SmartBatchCancelled() from exc
        entry["kept_steps"] = int(result.get("kept_count", 0))
        entry["fix_summary"] = str(result.get("summary", ""))
        if result.get("changed"):
            current = str(result["final_path"])
    elif fix:
        entry["fix_summary"] = "Nothing to fix: every tracked metric is in range."

    if master:
        check_cancel()
        stage("mastering")
        from khensu_services.copilot.mastering_agent import _guardrail_failed, run_mastering_agent
        from khensu_services.copilot.render_guardrails import post_render_guardrail
        mastered = run_mastering_agent(current, os.path.join(work_dir, "master"), genre,
                                       max_iterations=max_iterations, should_cancel=should_cancel,
                                       style=style, strength=strength)
        if mastered.get("stop_reason") == "cancelled":
            raise SmartBatchCancelled()
        best = mastered.get("best_safe_output_path") or ""
        if best and best != current and os.path.isfile(best):
            try:
                guard = post_render_guardrail(src, best)
            except Exception as exc:   # fail closed: an unchecked render is not safe
                guard = _guardrail_failed(exc)
            if guard.get("ok", False):
                current = best
                entry["mastered"] = True
                entry["master_summary"] = (f"{mastered.get('style_summary', '')} "
                                           f"Best safe candidate: iteration "
                                           f"{mastered.get('best_safe_iteration', 0)}.").strip()
            else:
                blocks = "; ".join(guard.get("blocks") or []) or "guardrail failed"
                entry["master_summary"] = f"Master not used: {blocks}."
        else:
            entry["master_summary"] = ("No mastering candidate beat the source safely ("
                                       + str(mastered.get("stop_reason", "")) + ").")

    check_cancel()
    if current == src:
        entry.update(status="unchanged", after_lufs=entry["before_lufs"],
                     after_true_peak_dbtp=entry["before_true_peak_dbtp"],
                     findings_after=len(findings_before), findings_cleared=0,
                     remaining=[f["title"] for f in findings_before])
        return entry

    stage("writing")
    shutil.copyfile(current, out_path)
    after = agent_loop.measure(out_path)
    findings_after = fixable_findings(after, specs, target_lufs, include_loudness)
    after_metrics = {f["metric"] for f in findings_after}
    cleared = [f["title"] for f in findings_before if f["metric"] not in after_metrics]
    entry.update(status=JobStatus.COMPLETED.value, output_path=out_path,
                 after_lufs=_finite(after.get("lufs")),
                 after_true_peak_dbtp=_finite(after.get("true_peak_dbtp")),
                 findings_after=len(findings_after), findings_cleared=len(cleared),
                 cleared=cleared, remaining=[f["title"] for f in findings_after])
    return entry


def remove_tree_quiet(path: str) -> None:
    try:
        shutil.rmtree(path, ignore_errors=True)
    except OSError:
        pass
