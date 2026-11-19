"""FastAPI router for HTML report generation at POST /report."""
import logging

from fastapi import APIRouter, Depends, HTTPException
from typing import Any

from khensu_services.common.api_models import RequestModel, ResponseModel

from .models import ReportRequest, ReportResponse
from .generator import generate_html_report
from ._report_common import ReportMarkupError
from khensu_services.common.audio_limits import audio_decode_budget_guard
from khensu_services.common.output_paths import require_output_extension, validate_output_path
from khensu_services.common.errors import internal_error, invalid_input, not_found
from khensu_services.common.executors import run_blocking

_log = logging.getLogger(__name__)
router = APIRouter(prefix="/report", tags=["report"])


def generate_pdf_report(req):
    """The PDF writer, imported on first use: ReportLab is only needed for a
    PDF and was ~125 ms of every service start-up (PERF-4)."""
    from .generator_pdf import generate_pdf_report as _write_pdf
    return _write_pdf(req)


async def _run_writer(label: str, fn, *args):
    """Run a report writer off the event loop with the shared status
    convention (SVC-09 / CPL-08): a write the OS refuses (permissions, a full
    disk, a locked file) is an input problem -> 422 naming only the OS reason;
    text ReportLab cannot lay out -> 422; anything else -> a generic 500 with
    the traceback in the service log, never the exception text."""
    try:
        return await run_blocking(fn, *args)
    except HTTPException:
        raise
    except ReportMarkupError as exc:
        invalid_input(f"{label} failed: {exc}")
    except OSError as exc:
        reason = exc.strerror or exc.__class__.__name__
        invalid_input(f"{label} failed: the output could not be written ({reason}).")
    except Exception as exc:  # noqa: BLE001 - mapped to the generic 500
        from khensu_services.common.errors import undecodable_audio_cause
        if undecodable_audio_cause(exc) is not None:
            raise
        _log.exception("%s failed", label)
        internal_error()

# Each writer produces exactly one format (AUDIT-023): an arbitrary extension
# let a token holder drop e.g. a `.pth` into site-packages.
_HTML_EXTS = (".html", ".htm")
_PDF_EXTS = (".pdf",)


@router.post("", response_model=ReportResponse, response_model_exclude_unset=True)
async def generate_report(req: ReportRequest) -> ReportResponse:
    """Generate a self-contained HTML analysis report and save it to disk."""
    require_output_extension(req.output_path, _HTML_EXTS)
    validate_output_path(req.output_path)
    # Offloaded: jinja layout + disk write would otherwise block the event
    # loop while every analysis endpoint correctly offloads.
    saved = await _run_writer("Report write", generate_html_report, req)
    return ReportResponse(saved_path=saved)


class StoryboardRequest(RequestModel):
    session_id: str
    output_path: str
    title: str = "Revision Storyboard"


class StoryboardResponse(ResponseModel):
    saved_path: str
    event_count: int


@router.post("/storyboard", response_model=StoryboardResponse, response_model_exclude_unset=True)
async def generate_storyboard_endpoint(req: StoryboardRequest) -> StoryboardResponse:
    """Client-facing Revision Storyboard: a standalone HTML chronology of the
    session's client-visible changes (applied processing, saved versions,
    exports), built from the provenance event store."""
    require_output_extension(req.output_path, _HTML_EXTS)
    validate_output_path(req.output_path)
    from .storyboard import generate_storyboard
    result = await _run_writer("Storyboard write", generate_storyboard, req.session_id,
                               req.output_path, req.title)
    return StoryboardResponse(saved_path=result["output_path"],
                              event_count=result["event_count"])


class ComplianceRequest(RequestModel):
    file_path: str
    pdf_path: str = ""   # when set, also render the one-page PDF and return its path


def _compliance_with_pdf(file_path: str, pdf_path: str) -> dict:
    from .compliance import build_compliance_sheet
    sheet = build_compliance_sheet(file_path)
    if pdf_path:
        import os as _os
        from .compliance_pdf import render_compliance_pdf
        _os.makedirs(_os.path.dirname(pdf_path) or ".", exist_ok=True)
        sheet["pdf_path"] = render_compliance_pdf(sheet, pdf_path)
    return sheet


class ComplianceCheck(ResponseModel):
    status: str          # "pass" | "warn" | "fail"
    value: Any = None    # number, count or tag dict, depending on the check
    note: str


class ComplianceFormat(ResponseModel):
    sample_rate: int
    bit_depth: int | None = None
    channels: int
    subtype: str | None = None


class CompliancePlatform(ResponseModel):
    platform_id: str
    name: str
    target_lufs: float
    ceiling_dbtp: float
    normalization_gain_db: float | None = None
    true_peak_ok: bool


class ComplianceSummary(ResponseModel):
    overall: str
    fail: int
    warn: int


class ComplianceSheetResponse(ResponseModel):
    file_path: str
    format: ComplianceFormat
    integrated_lufs: float | None = None
    # Check id -> verdict (true_peak_dbtp, dc_offset, clipping, mono_bass, ...).
    checks: dict[str, ComplianceCheck]
    platforms: list[CompliancePlatform]
    summary: ComplianceSummary
    markdown: str
    pdf_path: str | None = None


@router.post("/compliance", dependencies=[Depends(audio_decode_budget_guard)],
             response_model=ComplianceSheetResponse, response_model_exclude_unset=True)
async def compliance_sheet(req: ComplianceRequest) -> dict:
    """Delivery/compliance one-pager: loudness + range, true peak, mono compat,
    DC offset, clipping, bit depth, metadata hygiene, and per-platform
    normalisation — each with a pass/warn/fail verdict, plus a markdown report
    and (when pdf_path is set) a rendered one-page PDF."""
    import os
    if not os.path.isfile(req.file_path):
        not_found(f"File not found: {req.file_path}")
    if req.pdf_path:
        require_output_extension(req.pdf_path, _PDF_EXTS)
        validate_output_path(req.pdf_path, req.file_path)
    return await _run_writer("Compliance sheet", _compliance_with_pdf, req.file_path, req.pdf_path)


from .evidence import EvidenceReportRequest, EvidenceReportResponse  # noqa: E402  (CE-TRU-3)


@router.post("/evidence", response_model=EvidenceReportResponse, response_model_exclude_unset=True)
async def generate_evidence_report(req: EvidenceReportRequest) -> dict:
    """Evidence trail (CE-TRU-3): every recommendation with the measurements,
    thresholds and threshold sources behind it, its confidence basis, and the
    evidence-graph links - as a client-readable HTML page and optional JSON."""
    from .evidence import write_evidence_report
    require_output_extension(req.output_path, _HTML_EXTS)
    validate_output_path(req.output_path)
    if req.json_path:
        require_output_extension(req.json_path, (".json",))
        validate_output_path(req.json_path)
    return await _run_writer("Evidence report write", write_evidence_report, req)


@router.post("/pdf", response_model=ReportResponse, response_model_exclude_unset=True)
async def generate_pdf(req: ReportRequest) -> ReportResponse:
    """Generate a PDF analysis report and save it to disk."""
    require_output_extension(req.output_path, _PDF_EXTS)
    validate_output_path(req.output_path)
    saved = await _run_writer("PDF write", generate_pdf_report, req)
    return ReportResponse(saved_path=saved)
