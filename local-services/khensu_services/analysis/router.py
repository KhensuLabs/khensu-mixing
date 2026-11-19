from __future__ import annotations
from fastapi import APIRouter

# Dynamic wrappers for unit test mock patch compatibility
def analyze_quick(*args, **kwargs):
    import khensu_services.analysis.quick_analyzer as qa
    return qa.analyze_quick(*args, **kwargs)

def analyze_advanced(*args, **kwargs):
    import khensu_services.analysis.advanced_analyzer as aa
    return aa.analyze_advanced(*args, **kwargs)

def analyze_sections(*args, **kwargs):
    import khensu_services.analysis.section_analyzer as sa
    return sa.analyze_sections(*args, **kwargs)

def separate_stems(*args, **kwargs):
    import khensu_services.analysis.separator as sep
    return sep.separate_stems(*args, **kwargs)

def separate_stems_demucs(*args, **kwargs):
    import khensu_services.analysis.separator as sep
    return sep.separate_stems_demucs(*args, **kwargs)


from khensu_services.analysis.router_audio import router as audio_router
from khensu_services.analysis.router_separate import router as separate_router
from khensu_services.analysis.router_transport import router as transport_router
from khensu_services.analysis.router_masking import router as masking_router
from khensu_services.analysis.router_benchmark import router as benchmark_router
from khensu_services.analysis.router_delivery_compliance import router as delivery_compliance_router
from khensu_services.analysis.router_metric_timeline import router as metric_timeline_router
from khensu_services.analysis.router_reference_retrieval import router as reference_retrieval_router
from khensu_services.analysis.router_evidence_graph import router as evidence_graph_router
from khensu_services.analysis.router_gain_rider import router as gain_rider_router
from khensu_services.analysis.router_album import router as album_router
from khensu_services.analysis.router_comping import router as comping_router
from khensu_services.analysis.router_pitch_accuracy import router as pitch_accuracy_router
from khensu_services.analysis.router_stem_taxonomy import router as stem_taxonomy_router
from khensu_services.analysis.router_artifacts_scan import router as artifacts_scan_router
from khensu_services.analysis.router_full_intake import router as full_intake_router
from khensu_services.analysis.router_sound_recreation import router as sound_recreation_router
from khensu_services.analysis.router_fingerprint import router as fingerprint_router
from khensu_services.analysis.router_watermark_models import router as watermark_models_router
from khensu_services.analysis.router_model_manifest import router as model_manifest_router
from khensu_services.analysis.router_codec_artifacts import router as codec_artifacts_router
from khensu_services.analysis.router_dither import router as dither_router
from khensu_services.analysis.router_performance import router as performance_router
from khensu_services.analysis.router_pitch_editor import router as pitch_editor_router
from khensu_services.analysis.router_take_alignment import router as take_alignment_router
from khensu_services.analysis.router_word_clip_gain import router as word_clip_gain_router
from khensu_services.analysis.router_multitrack import router as multitrack_router
# ENG-5: the plain file analyses are one table.
from khensu_services.analysis.router_file_analyses import router as file_analyses_router

from fastapi import Depends  # noqa: E402
from khensu_services.common.audio_limits import audio_decode_budget_guard  # noqa: E402

# Every analysis route that decodes a caller-supplied audio path is held to the
# decode budget (413 over-limit / 422 unreadable audio) by one shared guard,
# instead of each router re-implementing (or forgetting) the check.
router = APIRouter(dependencies=[Depends(audio_decode_budget_guard)])

router.include_router(file_analyses_router)
router.include_router(audio_router)
router.include_router(separate_router)
router.include_router(transport_router)
router.include_router(masking_router)
router.include_router(benchmark_router)
router.include_router(delivery_compliance_router)
router.include_router(metric_timeline_router)
router.include_router(reference_retrieval_router)
router.include_router(evidence_graph_router)
router.include_router(gain_rider_router)
router.include_router(album_router)
router.include_router(comping_router)
router.include_router(pitch_accuracy_router)
router.include_router(stem_taxonomy_router)
router.include_router(artifacts_scan_router)
router.include_router(full_intake_router)
router.include_router(sound_recreation_router)
router.include_router(fingerprint_router)
router.include_router(watermark_models_router)
router.include_router(model_manifest_router)
router.include_router(codec_artifacts_router)
router.include_router(dither_router)
router.include_router(performance_router)
router.include_router(pitch_editor_router)
router.include_router(take_alignment_router)
router.include_router(word_clip_gain_router)
router.include_router(multitrack_router)

# CE-AI-1 / CE-AI-5 / CE-AI-6 / CE-TRU-2: embeddings + reference index,
# partial loudness, "explain this moment", counterfactual fixes.
from khensu_services.analysis.router_embeddings import router as embeddings_router  # noqa: E402
from khensu_services.analysis.router_perceptual_loudness import router as perceptual_loudness_router  # noqa: E402
from khensu_services.analysis.router_explain_moment import router as explain_moment_router  # noqa: E402
from khensu_services.analysis.router_counterfactual import router as counterfactual_router  # noqa: E402
router.include_router(embeddings_router)
router.include_router(perceptual_loudness_router)
router.include_router(explain_moment_router)
router.include_router(counterfactual_router)

# CE-PLT-2: persistent analysis cache stats/clear + changed-region detection.
from khensu_services.analysis.router_analysis_cache import router as analysis_cache_router  # noqa: E402
router.include_router(analysis_cache_router)

# GEN-1e: the user's confirmed genres, blended into the genre probe.
from khensu_services.analysis.router_genre_examples import router as genre_examples_router  # noqa: E402
router.include_router(genre_examples_router)

# CE-DAW-4: fixes written back as DAW automation (.mid CC lanes + REAPER envelopes).
from khensu_services.copilot.router_automation_export import router as automation_export_router  # noqa: E402
router.include_router(automation_export_router)

# CE-DEL-2..4: C2PA content credentials, streaming playback preview, one-click
# delivery package. (CE-DEL-1 /delivery/immersive_check is mounted in server.py,
# outside this router's 8-channel decode-budget guard.)
from khensu_services.analysis.router_c2pa import router as c2pa_router  # noqa: E402
from khensu_services.analysis.router_stream_preview import router as stream_preview_router  # noqa: E402
from khensu_services.analysis.router_delivery_package import router as delivery_package_router  # noqa: E402
router.include_router(c2pa_router)
router.include_router(stream_preview_router)
router.include_router(delivery_package_router)
