"""CE-COL-2: render versions, labels, tags, branches, branch lineage."""
from __future__ import annotations

import pytest

from khensu_services.collab import db as collab_db
from khensu_services.collab import versions


@pytest.fixture(autouse=True)
def collab_db_path(tmp_path, monkeypatch):
    monkeypatch.setenv("KHENSU_COLLAB_DB", str(tmp_path / "khensu.db"))
    yield
    collab_db.close()


def test_sync_is_idempotent_and_keeps_labels(client):
    nodes = [{"path": "C:/r/a.wav"}, {"path": "C:/r/b.wav", "parent_path": "C:/r/a.wav"}]
    r = client.post("/versions/sync", json={"session_id": "s", "nodes": nodes})
    assert r.status_code == 200, r.text
    vs = r.json()["versions"]
    assert len(vs) == 2 and vs[1]["parent_id"] == vs[0]["id"] and vs[0]["branch"] == ""

    lab = client.patch(f"/versions/{vs[0]['id']}", json={"label": "Mix v1", "tags": ["client", "client", "radio"]})
    assert lab.json()["label"] == "Mix v1" and lab.json()["tags"] == ["client", "radio"]

    again = client.post("/versions/sync", json={"session_id": "s", "nodes": nodes, "branch": "loud"}).json()
    assert [v["id"] for v in again["versions"]] == [v["id"] for v in vs]
    assert again["versions"][0]["label"] == "Mix v1"
    assert again["versions"][0]["branch"] == ""          # existing versions keep their branch


def test_branch_from_a_node_and_lineage_export(client):
    base = client.post("/versions/sync", json={"session_id": "s", "nodes": [
        {"path": "/r/main1.wav"}, {"path": "/r/main2.wav", "parent_path": "/r/main1.wav"}]}).json()
    main1 = base["versions"][0]["id"]
    br = client.post("/versions/branches", json={"session_id": "s", "name": "darker", "base_version_id": main1})
    assert br.status_code == 200 and br.json()["base_version_id"] == main1
    assert client.post("/versions/branches", json={"session_id": "s", "name": "darker"}).status_code == 409
    assert client.post("/versions/branches", json={"session_id": "s", "name": "../x"}).status_code == 422
    assert client.post("/versions/branches", json={"session_id": "s", "name": "y",
                                                   "base_version_id": "nope"}).status_code == 422

    client.post("/versions/sync", json={"session_id": "s", "branch": "darker",
                                        "nodes": [{"path": "/r/dark1.wav", "parent_path": "/r/main1.wav"}]})
    lineage = versions.branch_lineage("s", "darker")
    assert [v["path"] for v in lineage] == ["/r/main1.wav", "/r/dark1.wav"]
    main_line = versions.branch_lineage("s", "")
    assert {v["path"] for v in main_line} == {"/r/main1.wav", "/r/main2.wav"}
    with pytest.raises(versions.VersionError):
        versions.branch_lineage("s", "missing")
    listed = client.get("/versions", params={"session_id": "s"}).json()
    assert [b["name"] for b in listed["branches"]] == ["darker"]


def test_delete_reparents_children(client):
    vs = client.post("/versions/sync", json={"session_id": "s", "nodes": [
        {"path": "/a.wav"}, {"path": "/b.wav", "parent_path": "/a.wav"},
        {"path": "/c.wav", "parent_path": "/b.wav"}]}).json()["versions"]
    assert client.delete(f"/versions/{vs[1]['id']}").json() == {"deleted": True}
    left = client.get("/versions", params={"session_id": "s"}).json()["versions"]
    assert left[1]["path"] == "/c.wav" and left[1]["parent_id"] == vs[0]["id"]
    assert client.delete(f"/versions/{vs[1]['id']}").status_code == 404
    assert client.patch("/versions/nope", json={"label": "x"}).status_code == 404
