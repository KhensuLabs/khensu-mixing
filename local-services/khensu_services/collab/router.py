"""HTTP surface for collaboration and trust (CE-COL-1..4, CE-TRU-1).

Mounted in server.py behind the bearer token and the local-path guard. The
LAN listening room's own server is *not* this router: it lives in
``khensu_services/listening_room.py`` on a separate port.
"""
from __future__ import annotations

import os
from typing import Any

from fastapi import APIRouter, HTTPException

from khensu_services.common.api_models import RequestModel, ResponseModel
from khensu_services.common.errors import (
    conflict,
    internal_error,
    invalid_input,
    not_found,
    online_disabled,
    require_decode_budget,
    require_file,
    too_large,
)
from khensu_services.common.executors import run_blocking, run_io_blocking, run_long_blocking
from khensu_services.common.output_paths import require_output_extension, validate_output_path

router = APIRouter(tags=["collab"])


# ===========================================================================
# CE-COL-1: timestamped session comments
# ===========================================================================

class SessionComment(ResponseModel):
    id: str
    session_id: str
    author: str
    start_s: float
    end_s: float | None = None
    text: str
    resolved: bool
    created_at: str
    updated_at: str


class SessionCommentList(ResponseModel):
    comments: list[SessionComment]


class CommentCreateRequest(RequestModel):
    session_id: str
    text: str
    start_s: float
    end_s: float | None = None
    author: str = ""


class CommentUpdateRequest(RequestModel):
    text: str | None = None
    resolved: bool | None = None
    start_s: float | None = None
    end_s: float | None = None
    clear_end: bool = False


class CollabDeleteResponse(ResponseModel):
    deleted: bool


@router.get("/comments", response_model=SessionCommentList, response_model_exclude_unset=True)
async def list_session_comments(session_id: str, include_resolved: bool = True) -> dict:
    from khensu_services.collab import comments
    return {"comments": await run_io_blocking(comments.list_comments, session_id, include_resolved)}


@router.post("/comments", response_model=SessionComment, response_model_exclude_unset=True)
async def create_session_comment(req: CommentCreateRequest) -> dict:
    from khensu_services.collab import comments
    try:
        return await run_io_blocking(comments.add_comment, req.session_id, req.text, req.start_s,
                                     req.end_s, req.author)
    except comments.CommentError as exc:
        invalid_input(str(exc))


@router.patch("/comments/{comment_id}", response_model=SessionComment, response_model_exclude_unset=True)
async def update_session_comment(comment_id: str, req: CommentUpdateRequest) -> dict:
    from khensu_services.collab import comments
    try:
        got = await run_io_blocking(lambda: comments.update_comment(
            comment_id, text=req.text, resolved=req.resolved, start_s=req.start_s,
            end_s=req.end_s, clear_end=req.clear_end))
    except comments.CommentError as exc:
        invalid_input(str(exc))
    if got is None:
        not_found(f"Comment not found: {comment_id}")
    return got


@router.delete("/comments/{comment_id}", response_model=CollabDeleteResponse,
               response_model_exclude_unset=True)
async def delete_session_comment(comment_id: str) -> dict:
    from khensu_services.collab import comments
    if not await run_io_blocking(comments.delete_comment, comment_id):
        not_found(f"Comment not found: {comment_id}")
    return {"deleted": True}


# ===========================================================================
# CE-COL-2: render versions, labels, tags, branches
# ===========================================================================

class RenderVersion(ResponseModel):
    id: str
    session_id: str
    path: str
    parent_id: str | None = None
    label: str
    tags: list[str]
    branch: str
    created_at: str
    updated_at: str


class RenderBranch(ResponseModel):
    name: str
    base_version_id: str | None = None
    created_at: str


class RenderVersionList(ResponseModel):
    versions: list[RenderVersion]
    branches: list[RenderBranch]


class RenderNodeIn(RequestModel):
    path: str
    parent_path: str = ""


class VersionSyncRequest(RequestModel):
    session_id: str
    nodes: list[RenderNodeIn]
    branch: str = ""        # the active branch: new versions are recorded on it


class VersionUpdateRequest(RequestModel):
    label: str | None = None
    tags: list[str] | None = None


class BranchCreateRequest(RequestModel):
    session_id: str
    name: str
    base_version_id: str = ""


def _version_list(session_id: str) -> dict:
    from khensu_services.collab import versions
    return {"versions": versions.list_versions(session_id), "branches": versions.list_branches(session_id)}


@router.get("/versions", response_model=RenderVersionList, response_model_exclude_unset=True)
async def list_render_versions(session_id: str) -> dict:
    return await run_io_blocking(_version_list, session_id)


@router.post("/versions/sync", response_model=RenderVersionList, response_model_exclude_unset=True)
async def sync_render_versions(req: VersionSyncRequest) -> dict:
    """Record the desktop render tree. Idempotent: known paths keep their id,
    label, tags and branch; new ones land on `branch`."""
    from khensu_services.collab import versions
    if len(req.nodes) > versions.MAX_VERSIONS_PER_SESSION:
        too_large("Too many render nodes in one sync.")

    def run() -> dict:
        for node in req.nodes:   # parents precede children in the desktop tree
            versions.upsert_version(req.session_id, node.path, parent_path=node.parent_path,
                                    branch=req.branch)
        return _version_list(req.session_id)
    try:
        return await run_io_blocking(run)
    except versions.VersionError as exc:
        invalid_input(str(exc))


@router.patch("/versions/{version_id}", response_model=RenderVersion, response_model_exclude_unset=True)
async def update_render_version(version_id: str, req: VersionUpdateRequest) -> dict:
    from khensu_services.collab import versions
    try:
        got = await run_io_blocking(versions.update_version, version_id, req.label, req.tags)
    except versions.VersionError as exc:
        invalid_input(str(exc))
    if got is None:
        not_found(f"Version not found: {version_id}")
    return got


@router.delete("/versions/{version_id}", response_model=CollabDeleteResponse,
               response_model_exclude_unset=True)
async def delete_render_version(version_id: str) -> dict:
    from khensu_services.collab import versions
    if not await run_io_blocking(versions.delete_version, version_id):
        not_found(f"Version not found: {version_id}")
    return {"deleted": True}


@router.post("/versions/branches", response_model=RenderBranch, response_model_exclude_unset=True)
async def create_render_branch(req: BranchCreateRequest) -> dict:
    from khensu_services.collab import versions
    try:
        return await run_io_blocking(versions.create_branch, req.session_id, req.name, req.base_version_id)
    except versions.VersionError as exc:
        if "already exists" in str(exc):
            conflict(str(exc))
        invalid_input(str(exc))


# ===========================================================================
# CE-COL-1 / CE-COL-2: .khensu bundles
# ===========================================================================

class BundleExportRequest(RequestModel):
    session_id: str
    output_path: str
    session_name: str = ""
    # The desktop's SessionManager::exportSessionJson document (session +
    # analysis results). Absolute paths are reduced to file names on export.
    session_export: dict[str, Any] = {}
    source_paths: list[str] = []
    include_audio: bool = False
    # None = every version; "" = the main line; a name = that branch's lineage.
    branch: str | None = None


class BundleExportResponse(ResponseModel):
    output_path: str
    bytes: int
    comments: int
    versions: int
    audio_files: int
    branch: str | None = None


class BundleInspectRequest(RequestModel):
    bundle_path: str


class BundleAudioEntry(ResponseModel):
    name: str
    role: str
    bytes: int
    original_name: str


class BundleInspectResponse(ResponseModel):
    format_version: int
    created_at: str
    session_name: str
    branch: str | None = None
    comments: int
    versions: int
    branches: list[str]
    audio: list[BundleAudioEntry]


class BundleImportRequest(RequestModel):
    bundle_path: str
    dest_dir: str = ""       # extract the bundle's audio under this folder
    session_id: str = ""     # attach comments / versions / branches to this session


class BundleExtractedFile(ResponseModel):
    name: str
    role: str
    path: str
    version_id: str | None = None


class BundleImportResponse(ResponseModel):
    format_version: int
    session_name: str
    analysis: dict[str, Any]
    extracted: list[BundleExtractedFile]
    comments_imported: int
    versions_imported: int
    versions_skipped: int
    comments_in_bundle: int
    versions_in_bundle: int


def _bundle_error(exc: Exception) -> None:
    from khensu_services.collab.bundle import BundleTooLarge
    if isinstance(exc, BundleTooLarge):
        too_large(str(exc))
    invalid_input(str(exc))


@router.post("/bundle/export", response_model=BundleExportResponse, response_model_exclude_unset=True)
async def export_session_bundle(req: BundleExportRequest) -> dict:
    from khensu_services.collab import bundle, versions
    from khensu_services.version import SERVICE_VERSION
    require_output_extension(req.output_path, (bundle.BUNDLE_EXT,))
    validate_output_path(req.output_path)
    for p in req.source_paths:
        require_file(p, "Source audio")
    try:
        return await run_long_blocking(
            bundle.export_bundle, req.session_id, req.output_path, req.session_name,
            req.session_export, req.source_paths, req.include_audio, req.branch, SERVICE_VERSION)
    except (bundle.BundleError, versions.VersionError) as exc:
        _bundle_error(exc)
    except OSError as exc:
        invalid_input(f"Bundle export failed: {exc.strerror or exc.__class__.__name__}")


@router.post("/bundle/inspect", response_model=BundleInspectResponse, response_model_exclude_unset=True)
async def inspect_session_bundle(req: BundleInspectRequest) -> dict:
    from khensu_services.collab import bundle
    require_file(req.bundle_path, "Bundle")
    try:
        return await run_io_blocking(bundle.inspect_bundle, req.bundle_path)
    except bundle.BundleError as exc:
        _bundle_error(exc)


@router.post("/bundle/import", response_model=BundleImportResponse, response_model_exclude_unset=True)
async def import_session_bundle(req: BundleImportRequest) -> dict:
    from khensu_services.collab import bundle
    require_file(req.bundle_path, "Bundle")
    if req.dest_dir:
        validate_output_path(os.path.join(req.dest_dir, "x.wav"))
    try:
        return await run_long_blocking(bundle.import_bundle, req.bundle_path, req.dest_dir, req.session_id)
    except bundle.BundleError as exc:
        _bundle_error(exc)
    except OSError as exc:
        invalid_input(f"Bundle import failed: {exc.strerror or exc.__class__.__name__}")


# ===========================================================================
# CE-COL-1 / FEAT-9: revision report
# ===========================================================================

class RevisionFinding(RequestModel):
    code: str
    severity: float          # 0..1, or the desktop's 0..100
    title: str = ""


class RevisionReportRequest(RequestModel):
    a_path: str = ""
    b_path: str = ""
    a_result_id: str = ""
    b_result_id: str = ""
    a_metrics: dict[str, float] = {}
    b_metrics: dict[str, float] = {}
    a_findings: list[RevisionFinding] | None = None
    b_findings: list[RevisionFinding] | None = None
    a_label: str = ""
    b_label: str = ""


class RevisionMetricChange(ResponseModel):
    metric: str
    label: str
    unit: str
    before: float
    after: float
    delta: float
    text: str


class RevisionFindingChange(ResponseModel):
    code: str
    title: str
    before: float | None = None
    after: float | None = None
    text: str


class RevisionReportResponse(ResponseModel):
    a_label: str
    b_label: str
    summary: str
    changed: list[RevisionMetricChange]
    fixed: list[RevisionFindingChange]
    improved: list[RevisionFindingChange]
    got_worse: list[RevisionFindingChange]
    unchanged: list[RevisionFindingChange]
    a_metrics: dict[str, float]
    b_metrics: dict[str, float]


async def _revision_side(path: str, result_id: str, metrics: dict[str, float],
                         label: str) -> tuple[dict[str, float], list[dict[str, Any]], str]:
    from khensu_services.collab import revision
    if path:
        require_file(path, "Audio")
        await require_decode_budget(path)
        m, f = await run_blocking(revision.measure_file, path)
        return m, f, label or revision.default_label(path)
    if result_id:
        from khensu_services.collab.db import _db_path
        row = await run_io_blocking(revision.load_stored_result, result_id, _db_path())
        if row is None:
            not_found(f"Stored analysis result not found: {result_id}")
        m = revision.normalize_metrics(row["metrics"])
        return m, revision.measurement_findings(m), label or f"analysis {row['computed_at']}"
    if metrics:
        m = revision.normalize_metrics(metrics)
        return m, revision.measurement_findings(m), label
    invalid_input("Each side needs a file path, a stored result id or metrics.")


@router.post("/revision/report", response_model=RevisionReportResponse, response_model_exclude_unset=True)
async def revision_report(req: RevisionReportRequest) -> dict:
    """What changed between A (before) and B (after): metric deltas, findings
    fixed, improved, unchanged, and new or worse."""
    from khensu_services.collab import revision
    a_m, a_f, a_label = await _revision_side(req.a_path, req.a_result_id, req.a_metrics, req.a_label)
    b_m, b_f, b_label = await _revision_side(req.b_path, req.b_result_id, req.b_metrics, req.b_label)
    a_label, b_label = a_label or "A", b_label or "B"
    if req.a_findings is not None:
        a_f = revision.normalize_findings([f.model_dump() for f in req.a_findings])
    if req.b_findings is not None:
        b_f = revision.normalize_findings([f.model_dump() for f in req.b_findings])
    return revision.build_report(a_m, b_m, a_f, b_f, a_label, b_label)


# ===========================================================================
# CE-COL-3: LAN listening room (the room itself is listening_room.py)
# ===========================================================================

class ListeningVersionIn(RequestModel):
    label: str
    path: str


class ListeningStartRequest(RequestModel):
    session_id: str = ""
    versions: list[ListeningVersionIn]
    port: int = 0


class ListeningVersionOut(ResponseModel):
    label: str
    gain_db: float
    lufs: float | None = None


class ListeningStatusResponse(ResponseModel):
    running: bool
    url: str | None = None
    session_id: str | None = None
    host: str | None = None
    port: int | None = None
    started_at: float | None = None
    versions: list[ListeningVersionOut] | None = None
    listeners: int | None = None
    rejected_requests: int | None = None


class ListeningTally(ResponseModel):
    label: str
    votes: int
    share: float
    gain_db: float
    lufs: float | None = None


class ListeningVote(ResponseModel):
    version: str
    presented_as: str
    voted_at: float


class ListeningResultsResponse(ResponseModel):
    running: bool
    listeners: int
    total_votes: int
    tallies: list[ListeningTally]
    votes: list[ListeningVote]
    leader: str | None = None
    session_id: str | None = None
    started_at: float | None = None
    stopped_at: float | None = None


class ListeningStopResponse(ResponseModel):
    running: bool
    results: ListeningResultsResponse | None = None


@router.post("/listening/start", response_model=ListeningStatusResponse, response_model_exclude_unset=True)
async def start_listening_room(req: ListeningStartRequest) -> dict:
    """Start the opt-in LAN listening room. 403 unless the master online
    switch and the `lan_listening` toggle are both on."""
    from khensu_services import listening_room
    from khensu_services.common.online import online_allowed
    if not online_allowed("lan_listening"):
        online_disabled("The LAN listening room is off. Turn on the master online switch and "
                        "'LAN listening room' in Settings to share a room on your network.")
    for v in req.versions:
        require_file(v.path, f"Version '{v.label}'")
    await require_decode_budget(*[v.path for v in req.versions])
    try:
        return await run_long_blocking(listening_room.start_room, req.session_id,
                                       [v.model_dump() for v in req.versions], "", req.port)
    except listening_room.RoomError as exc:
        if "already running" in str(exc) or "No LAN" in str(exc):
            conflict(str(exc))
        invalid_input(str(exc))
    except OSError as exc:
        conflict(f"Could not open the listening-room port: {exc.strerror or exc}")


@router.post("/listening/stop", response_model=ListeningStopResponse, response_model_exclude_unset=True)
async def stop_listening_room() -> dict:
    from khensu_services import listening_room
    out = await run_io_blocking(listening_room.stop_room)
    if "results" in out:
        out["results"] = {"running": False, **out["results"]}
    return out


@router.get("/listening/status", response_model=ListeningStatusResponse, response_model_exclude_unset=True)
async def listening_room_status() -> dict:
    from khensu_services import listening_room
    return listening_room.status()


@router.get("/listening/results", response_model=ListeningResultsResponse,
            response_model_exclude_unset=True)
async def listening_room_results() -> dict:
    from khensu_services import listening_room
    return listening_room.results()


# ===========================================================================
# CE-COL-4: shared reference profiles
# ===========================================================================

class ReferenceProfileMetrics(ResponseModel):
    lufs_integrated: float
    lra: float
    spectral_tilt_db_per_octave: float
    crest_factor_db: float


class UserReferenceProfile(ResponseModel):
    id: str
    name: str
    genre: str
    metrics: ReferenceProfileMetrics
    profile_sha256: str
    source: str
    added_at: str
    analysed_at: str | None = None
    notes: str | None = None


class ReferenceProfileList(ResponseModel):
    profiles: list[UserReferenceProfile]


class ReferenceProfileAnalyzeRequest(RequestModel):
    file_path: str
    name: str = ""
    genre: str = ""


class ReferenceProfileExportRequest(RequestModel):
    output_path: str
    profile_ids: list[str] = []


class ReferenceProfileExportResponse(ResponseModel):
    output_path: str
    count: int
    sha256: str


class ReferenceProfileImportRequest(RequestModel):
    path: str


class ReferenceProfileImportResponse(ResponseModel):
    imported: int
    skipped_duplicates: int
    profiles: list[UserReferenceProfile]


class ReferenceIndexBrowseRequest(RequestModel):
    index_url: str


class ReferenceIndexEntry(ResponseModel):
    name: str
    url: str
    sha256: str
    genre: str | None = None
    count: int | None = None


class ReferenceIndexBrowseResponse(ResponseModel):
    entries: list[ReferenceIndexEntry]


class ReferenceIndexFetchRequest(RequestModel):
    url: str
    sha256: str


def _profile_error(exc: Exception) -> None:
    invalid_input(str(exc))


@router.get("/references/profiles", response_model=ReferenceProfileList, response_model_exclude_unset=True)
async def list_reference_profiles(genre: str | None = None) -> dict:
    from khensu_services.collab import reference_profiles as rp
    return {"profiles": await run_io_blocking(rp.list_profiles, genre)}


@router.post("/references/profiles/analyze", response_model=UserReferenceProfile,
             response_model_exclude_unset=True)
async def analyze_reference_profile(req: ReferenceProfileAnalyzeRequest) -> dict:
    """Measure a reference track and keep its numbers (never its audio)."""
    from khensu_services.collab import reference_profiles as rp
    require_file(req.file_path, "Reference audio")
    await require_decode_budget(req.file_path)
    try:
        return await run_blocking(rp.analyse_profile, req.file_path, req.name, req.genre)
    except rp.ProfileError as exc:
        _profile_error(exc)


@router.post("/references/profiles/export", response_model=ReferenceProfileExportResponse,
             response_model_exclude_unset=True)
async def export_reference_profiles(req: ReferenceProfileExportRequest) -> dict:
    from khensu_services.collab import reference_profiles as rp
    require_output_extension(req.output_path, (".json",))
    validate_output_path(req.output_path)
    try:
        return await run_io_blocking(rp.export_profiles, req.output_path, req.profile_ids or None)
    except rp.ProfileError as exc:
        _profile_error(exc)


@router.post("/references/profiles/import", response_model=ReferenceProfileImportResponse,
             response_model_exclude_unset=True)
async def import_reference_profiles(req: ReferenceProfileImportRequest) -> dict:
    from khensu_services.collab import reference_profiles as rp
    require_file(req.path, "Profile file")
    try:
        return await run_io_blocking(rp.import_profiles, req.path)
    except rp.ProfileError as exc:
        _profile_error(exc)


@router.delete("/references/profiles/{profile_id}", response_model=CollabDeleteResponse,
               response_model_exclude_unset=True)
async def delete_reference_profile(profile_id: str) -> dict:
    from khensu_services.collab import reference_profiles as rp
    if not await run_io_blocking(rp.delete_profile, profile_id):
        not_found(f"Reference profile not found: {profile_id}")
    return {"deleted": True}


def _require_index_allowed() -> None:
    from khensu_services.common.online import online_allowed
    if not online_allowed("reference_index"):
        from khensu_services.common.security_log import KIND_ONLINE_GATE_BLOCKED, record_event
        record_event(KIND_ONLINE_GATE_BLOCKED, "reference-profile index fetch refused (online off)")
        online_disabled("Browsing a shared reference index is off. Turn on the master online switch "
                        "and 'Shared reference index' in Settings.")


@router.post("/references/index/browse", response_model=ReferenceIndexBrowseResponse,
             response_model_exclude_unset=True)
async def browse_reference_index(req: ReferenceIndexBrowseRequest) -> dict:
    from khensu_services.collab import reference_profiles as rp
    _require_index_allowed()
    try:
        return {"entries": await run_io_blocking(rp.browse_index, req.index_url)}
    except rp.ProfileError as exc:
        _profile_error(exc)
    except HTTPException:
        raise
    except Exception as exc:  # noqa: BLE001 - network / TLS / missing httpx
        invalid_input(f"The index could not be read: {exc.__class__.__name__}")


@router.post("/references/index/fetch", response_model=ReferenceProfileImportResponse,
             response_model_exclude_unset=True)
async def fetch_reference_index_entry(req: ReferenceIndexFetchRequest) -> dict:
    from khensu_services.collab import reference_profiles as rp
    _require_index_allowed()
    try:
        return await run_io_blocking(rp.fetch_from_index, req.url, req.sha256)
    except rp.ProfileError as exc:
        _profile_error(exc)
    except HTTPException:
        raise
    except Exception as exc:  # noqa: BLE001
        invalid_input(f"The profile could not be fetched: {exc.__class__.__name__}")


# ===========================================================================
# CE-TRU-1: calibration table summary
# ===========================================================================

class CalibrationCodeSummary(ResponseModel):
    n: int
    precision: float
    recall: float


class CalibrationTableResponse(ResponseModel):
    available: bool
    codes: dict[str, CalibrationCodeSummary]
    built_at: str | None = None
    corpus_sha256: str | None = None


@router.get("/calibration/findings", response_model=CalibrationTableResponse,
            response_model_exclude_unset=True)
async def calibration_findings() -> dict:
    """Which finding codes are calibrated (n, precision, recall). Empty and
    `available: false` until a table is built from a real labelled corpus."""
    from khensu_services.common.calibration import table_summary
    try:
        return await run_io_blocking(table_summary)
    except OSError:
        internal_error()
