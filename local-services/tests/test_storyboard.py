# local-services/tests/test_storyboard.py
"""Tests for the client-facing Revision Storyboard generator."""
import uuid

import pytest

from khensu_services.events.db import append_event
from khensu_services.report.storyboard import generate_storyboard

# Output lands in tmp_path with no source file: opt it into the allowed
# output roots (cwd is not one - see common/output_paths.py).
pytestmark = pytest.mark.usefixtures("tmp_output_root")


def test_storyboard_includes_client_visible_events(tmp_path):
    # Unique per run: the provenance store is append-only and persistent,
    # so a reused session id accumulates events across test runs.
    sid = f"storyboard-test-{uuid.uuid4()}"
    append_event(session_id=sid, event_type="chain_applied",
              summary="Warmed up the low mids and tamed 3 kHz harshness")
    append_event(session_id=sid, event_type="recommendation_shown",
              summary="INTERNAL: should not appear")
    append_event(session_id=sid, event_type="export",
              summary="Delivered v2 master, WAV 24/44.1")

    out = tmp_path / "storyboard.html"
    result = generate_storyboard(sid, str(out), title="Song X — Revision 2")
    assert result["event_count"] == 2

    html_text = out.read_text(encoding="utf-8")
    assert "Warmed up the low mids" in html_text
    assert "Delivered v2 master" in html_text
    assert "should not appear" not in html_text
    assert "Song X" in html_text


def test_storyboard_empty_session(tmp_path):
    out = tmp_path / "empty.html"
    result = generate_storyboard("no-such-session", str(out))
    assert result["event_count"] == 1   # the empty-state card
    assert "No client-visible" in out.read_text(encoding="utf-8")


def test_endpoint_happy_path(client, tmp_path):
    sid = f"storyboard-endpoint-{uuid.uuid4()}"
    append_event(session_id=sid, event_type="chain_applied", summary="Vocal de-essed")
    out = tmp_path / "sb.html"
    resp = client.post("/report/storyboard",
                       json={"session_id": sid, "output_path": str(out)})
    assert resp.status_code == 200
    assert out.exists()
