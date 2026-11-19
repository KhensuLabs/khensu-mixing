"""Pydantic models for the /report endpoint."""
from __future__ import annotations
from typing import Annotated, Any

from pydantic import Field

from khensu_services.common.api_models import RequestModel, ResponseModel


ColourToken = Annotated[str, Field(pattern=r"^#[0-9a-fA-F]{6}([0-9a-fA-F]{2})?$")]


class ReportTheme(RequestModel):
    """A serializable snapshot of the desktop KhensuTheme palette.

    Reports are rendered by the local Python service, so they cannot read the
    desktop's runtime theme directly. Keeping the complete palette in the
    request makes HTML and PDF exports match the theme selected at export time.
    """

    bg_void: ColourToken = "#050608"
    bg_base: ColourToken = "#080b0d"
    bg_surface: ColourToken = "#0e1518"
    bg_raised: ColourToken = "#141d21"
    bg_overlay: ColourToken = "#1a2429"
    border_subtle: ColourToken = "#ffffff12"
    border_active: ColourToken = "#ffffff22"
    accent_teal: ColourToken = "#22d3c5"
    accent_teal_dim: ColourToken = "#22d3c533"
    accent_amber: ColourToken = "#f5a623"
    accent_amber_dim: ColourToken = "#f5a62333"
    accent_red: ColourToken = "#ff4d6a"
    accent_red_dim: ColourToken = "#ff4d6a33"
    accent_purple: ColourToken = "#a78bfa"
    accent_purple_dim: ColourToken = "#a78bfa33"
    accent_magenta: ColourToken = "#f0308f"
    accent_magenta_dim: ColourToken = "#f0308f33"
    accent_blue: ColourToken = "#4da3ff"
    accent_blue_dim: ColourToken = "#4da3ff33"
    accent_green: ColourToken = "#34d399"
    accent_green_dim: ColourToken = "#34d39933"
    scrim: ColourToken = "#000000b3"
    shadow_colour: ColourToken = "#00000080"
    highlight: ColourToken = "#ffffff0d"
    focus_ring: ColourToken = "#22d3c5"
    text_primary: ColourToken = "#eaf0f4"
    text_secondary: ColourToken = "#8a94a0"
    text_tertiary: ColourToken = "#71808a"
    text_accent: ColourToken = "#22d3c5"


class RecommendationSummary(RequestModel):
    type: str
    what: str
    severity: float
    suggested_action: str
    # Optional: lets the report add myth context and fill cause / trade-off /
    # verification from recommendations/narrative.py when the caller omits them.
    issue_code: str = ""
    most_likely_cause: str = ""
    trade_off: str = ""
    how_to_verify: str = ""


class ResonanceHotspotSummary(RequestModel):
    hz: float
    db: float
    prominence_db: float


class ReportRequest(RequestModel):
    session_name: str
    file_name: str
    session_id: str = ""
    report_mode: str = "client"
    computed_at: str
    lufs_integrated: float
    lra: float
    true_peak_dbtp: float
    crest_factor_db: float
    dynamic_range_db: float
    stereo_width: float
    stereo_correlation: float = 0.0
    bpm: float = 0.0
    pitch_key: str = ""
    # Estimate confidences (0-1) for bpm / pitch_key, and whether the pitched
    # material behind them came from the monophonic pyin fallback; None /
    # False = not sent. The reports print them so an estimate isn't read as
    # a measurement (AUDIT-052).
    bpm_confidence: float | None = None
    key_confidence: float | None = None
    low_quality_extraction: bool = False
    genre_detected: str = ""
    hook_strength: float = 0.0
    hook_count: int = 0
    memorability_score: float = 0.0
    heaviness_distortion: float = 0.0
    # Spectral balance
    spectral_balance_low: float  = 0.0
    spectral_balance_mid: float  = 0.0
    spectral_balance_high: float = 0.0
    spectral_centroid_hz: float  = 0.0
    # Translation risk per environment (0–1, higher = riskier)
    translation_risk_studio:  float = 0.0
    translation_risk_earbuds: float = 0.0
    translation_risk_phone:   float = 0.0
    translation_risk_car:     float = 0.0
    translation_risk_club:    float = 0.0
    # Harshness
    harshness_score: float = 0.0
    harshness_peak_hz: float | None = None
    # Resonance hotspots
    resonance_hotspots: list[ResonanceHotspotSummary] = []
    recommendations: list[RecommendationSummary] = []
    output_path: str

    # Quick stats (from /analyze/quick_stats) — optional
    quick_stats_lufs_integrated: float | None = None
    quick_stats_true_peak: float | None = None
    quick_stats_lra: float | None = None
    quick_stats_crest: float | None = None
    quick_stats_clipping_frames: int | None = None
    quick_stats_dc_offset: float | None = None
    theme: ReportTheme = Field(default_factory=ReportTheme)
    #: WF-14: the session's follow-up scans (mono compatibility, source
    #: forensics, streaming normalisation...), each a compacted result keyed
    #: by scan name - the report lists their headline numbers.
    follow_up_scans: dict[str, Any] = {}


class ReportResponse(ResponseModel):
    saved_path: str
