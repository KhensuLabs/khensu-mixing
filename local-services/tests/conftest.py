"""Pytest fixtures for the khensu-services test suite.

Auth bypass: use FastAPI's dependency_overrides so verify_token is replaced
at the framework level — no runtime environment checks baked into production code.
"""
import sys
from pathlib import Path

# AQ-09: make `launch_service` (local-services/launch_service.py) importable no
# matter which directory pytest was invoked from. `python -m pytest` only puts
# the current working directory on sys.path, so `from launch_service import ...`
# used to work from local-services/ but fail on a repository-root collection.
# This resolves the module by the test tree's own fixed location instead.
_SERVICE_ROOT = Path(__file__).resolve().parent.parent
if str(_SERVICE_ROOT) not in sys.path:
    sys.path.insert(0, str(_SERVICE_ROOT))

import os

# The service's TrustedHostMiddleware admits only loopback names (AUDIT-073);
# Starlette's TestClient addresses the app as "testserver". Set before the
# server module is imported, which is when the allowlist is read.
os.environ.setdefault("KHENSU_TRUSTED_HOSTS", "testserver")
# NEW-9: the CLAP genre probe runs only where CLAP and its checkpoint are
# installed. Off for the suite so genre results don't depend on the machine;
# tests/test_genre_probe.py turns it back on where it needs it.
os.environ.setdefault("KHENSU_DISABLE_GENRE_PROBE", "1")

import pytest  # noqa: E402
from fastapi.testclient import TestClient  # noqa: E402
from khensu_services.server import app  # noqa: E402
from khensu_services.auth import verify_token
from khensu_services.common import api_models as _api_models

# Response models reject keys they do not declare while tests run, so a model
# can't fall behind the dict its endpoint returns (production keeps them).
_api_models.STRICT_EXTRA = True

# This is the intentionally small, hermetic edit/test shard. The complete
# suite remains the default pre-PR gate; tests outside this list are classified
# as `full` so the quick command cannot silently grow into the whole suite.
_QUICK_TEST_MODULES = frozenset(
    {
        "test_analysis_request_limits.py",
        "test_analyze_models.py",
        "test_channel_layout.py",
        "test_credential_protector.py",
        "test_db_unify.py",
        "test_evidence_graph.py",
        "test_genre_schema.py",
        "test_integrity_checker.py",
        "test_key.py",
        "test_model_manifest.py",
        "test_online_gate.py",
        "test_quick_stats.py",
        "test_render_finite_guards.py",
        "test_render_output_safety.py",
        "test_render_validator.py",
        "test_server_health.py",
        "test_surround_validation.py",
        "test_training_provenance.py",
    }
)


def pytest_collection_modifyitems(items):
    """Give every test an explicit quick/full classification.

    The full-module list is deliberately small and named: adding a new
    heavyweight integration test requires an intentional classification rather
    than silently making the quick command slower.
    """
    for item in items:
        if item.fspath.basename in _QUICK_TEST_MODULES:
            item.add_marker("quick")
        else:
            item.add_marker("full")


@pytest.fixture(autouse=True)
def bypass_auth():
    """Replace verify_token with a no-op for every test in this suite."""
    app.dependency_overrides[verify_token] = lambda: None
    yield
    app.dependency_overrides.clear()


@pytest.fixture(autouse=True)
def isolated_analysis_cache(tmp_path_factory, monkeypatch):
    """CE-PLT-2: every test gets its own (lazily created) persistent analysis
    cache directory and empty in-memory analyze caches, so a result one test
    produced - possibly from a mocked analyzer - is never served to another.
    Tests of the cache itself use the same directory through the env var."""
    import uuid

    root = tmp_path_factory.getbasetemp() / "analysis_cache" / uuid.uuid4().hex
    monkeypatch.setenv("KHENSU_ANALYSIS_CACHE_DIR", str(root))
    try:
        from khensu_services.analysis import router_audio as _ra
        _ra._analysis_cache.clear()
        _ra._extended_cache.clear()
    except Exception:
        pass
    yield root


@pytest.fixture(autouse=True)
def watermark_gate_closed(request, monkeypatch):
    """Keep the AudioSeal/WavMark tiers off for every test so the suite is
    hermetic and fast — otherwise, on a machine that has pre-cached the weights,
    `gate_open()` auto-opens and every artifacts scan loads + runs both models
    (a ~15x slowdown). Tests that exercise the tiers monkeypatch `_model`/
    `_detector` directly; a test needing the real gate marks itself
    `@pytest.mark.watermark_gate_real`."""
    if request.node.get_closest_marker("watermark_gate_real"):
        return
    try:
        from khensu_services.analysis.artifacts import watermark_precache as wp
        monkeypatch.setattr(wp, "gate_open", lambda model_key: False)
    except Exception:
        pass


@pytest.fixture
def tmp_output_root(tmp_path, monkeypatch):
    """Add this test's tmp_path to the allowed output roots.

    The process cwd is not an allowed root (common/output_paths.py), and
    pytest's ``--basetemp`` lives under local-services/ — outside home/temp
    on a machine whose checkout isn't under the user profile. Endpoints that
    write with no source file (``/report``, ``/report/pdf``, storyboard) use
    this instead of silently relying on cwd."""
    from khensu_services.common import output_paths
    original = output_paths.allowed_output_roots
    root = tmp_path.resolve()
    monkeypatch.setattr(output_paths, "allowed_output_roots",
                        lambda source_path=None: [*original(source_path), root])
    return root


@pytest.fixture
def client():
    """A TestClient with auth already bypassed via the autouse fixture above."""
    return TestClient(app)
