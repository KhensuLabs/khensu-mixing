"""CE-DEL-2: C2PA content credentials + the always-written sidecar."""
from __future__ import annotations

import datetime
import hashlib
import json
import os
import struct

import numpy as np
import pytest
import soundfile as sf

from khensu_services.analysis import c2pa_credentials as cc


def _wav(path, seconds=1.0, info_text: bytes | None = None) -> str:
    sf.write(path, (0.1 * np.sin(np.arange(int(48000 * seconds)) / 10)).astype(np.float32), 48000,
             subtype="PCM_24")
    if info_text is not None:
        data = open(path, "rb").read()
        body = b"INFO" + b"ICMT" + struct.pack("<I", len(info_text) + 1) + info_text + b"\x00"
        if len(body) % 2:
            body += b"\x00"
        data += b"LIST" + struct.pack("<I", len(body)) + body
        data = data[:4] + struct.pack("<I", len(data) - 8) + data[8:]
        open(path, "wb").write(data)
    return str(path)


def _sha(path) -> str:
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def make_test_chain(dir_path) -> dict:
    """A throwaway ES256 root + signer chain (cryptography). Tests only."""
    pytest.importorskip("cryptography")
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

    now = datetime.datetime.now(datetime.timezone.utc)
    ca_key = ec.generate_private_key(ec.SECP256R1())
    ca_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "Khensu Test Root"),
                         x509.NameAttribute(NameOID.ORGANIZATION_NAME, "Khensu Test")])
    ca = (x509.CertificateBuilder().subject_name(ca_name).issuer_name(ca_name)
          .public_key(ca_key.public_key()).serial_number(x509.random_serial_number())
          .not_valid_before(now - datetime.timedelta(days=1))
          .not_valid_after(now + datetime.timedelta(days=30))
          .add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
          .add_extension(x509.KeyUsage(False, False, False, False, False, True, True, False, False),
                         critical=True)
          .add_extension(x509.SubjectKeyIdentifier.from_public_key(ca_key.public_key()), critical=False)
          .sign(ca_key, hashes.SHA256()))
    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "Khensu Test Signer"),
                      x509.NameAttribute(NameOID.ORGANIZATION_NAME, "Khensu Test")])
    leaf = (x509.CertificateBuilder().subject_name(name).issuer_name(ca_name)
            .public_key(key.public_key()).serial_number(x509.random_serial_number())
            .not_valid_before(now - datetime.timedelta(days=1))
            .not_valid_after(now + datetime.timedelta(days=30))
            .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
            .add_extension(x509.KeyUsage(True, False, False, False, False, False, False, False, False),
                           critical=True)
            .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.EMAIL_PROTECTION]), critical=False)
            .add_extension(x509.SubjectKeyIdentifier.from_public_key(key.public_key()), critical=False)
            .add_extension(x509.AuthorityKeyIdentifier.from_issuer_public_key(ca_key.public_key()),
                           critical=False)
            .sign(ca_key, hashes.SHA256()))
    cert_path = os.path.join(dir_path, "chain.pem")
    key_path = os.path.join(dir_path, "signer.key")
    with open(cert_path, "wb") as f:
        f.write(leaf.public_bytes(serialization.Encoding.PEM) + ca.public_bytes(serialization.Encoding.PEM))
    with open(key_path, "wb") as f:
        f.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                  serialization.NoEncryption()))
    return {"configured": True, "cert_path": cert_path, "key_path": key_path, "tsa_url": "", "reason": ""}


@pytest.fixture
def no_credentials(monkeypatch):
    monkeypatch.setattr(cc, "c2pa_status", lambda: (True, ""))   # the SDK is present; only the keys are missing
    monkeypatch.setattr(cc, "signing_config", lambda settings=None: {
        "configured": False, "cert_path": "", "key_path": "", "tsa_url": "",
        "reason": "No signing certificate and key are set in Settings > Content credentials."})


@pytest.fixture
def credentials(tmp_path, monkeypatch):
    pytest.importorskip("c2pa")
    cfg = make_test_chain(str(tmp_path))
    monkeypatch.setattr(cc, "signing_config", lambda settings=None: cfg)
    return cfg


def test_actions_and_digital_source_types():
    created = cc.build_actions("created", "none")
    assert created[0]["action"] == "c2pa.created"
    assert created[0]["digitalSourceType"].endswith("/digitalCreation")
    assert created[0]["softwareAgent"]["name"].startswith("Khensu Mixing ")
    gen = cc.build_actions("created", "ai_generated")
    assert gen[0]["digitalSourceType"].endswith("/trainedAlgorithmicMedia")
    edited = cc.build_actions("edited", "ai_assisted")
    assert [a["action"] for a in edited] == ["c2pa.opened", "c2pa.edited"]
    assert edited[1]["digitalSourceType"].endswith("/compositeWithTrainedAlgorithmicMedia")
    assert "digitalSourceType" not in cc.build_actions("edited", "none")[1]
    with pytest.raises(ValueError):
        cc.build_actions("deleted", "none")


def test_signing_config_reads_settings(tmp_path):
    cert, key = tmp_path / "c.pem", tmp_path / "k.pem"
    cert.write_text("x")
    assert not cc.signing_config({})["configured"]
    cfg = cc.signing_config({"provenance": {"cert_path": str(cert), "key_path": str(key)}})
    assert not cfg["configured"] and "key file" in cfg["reason"]
    key.write_text("y")
    assert cc.signing_config({"provenance": {"cert_path": str(cert), "key_path": str(key)}})["configured"]


def test_unsigned_sidecar_always_written_and_history_kept(client, tmp_path, no_credentials):
    src = _wav(tmp_path / "m.wav")
    before = _sha(src)
    r = client.post("/provenance/sign", json={"file_path": src, "ai_use": "ai_assisted"})
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["signed"] is False and "Settings" in body["reason"]
    assert body["sidecar_path"] == src + ".provenance.json" and body["sha256"] == before
    rec = json.load(open(body["sidecar_path"], encoding="utf-8"))
    assert rec["sha256"] == before and rec["ai_use"] == "ai_assisted"
    assert rec["assertions"][0]["data"]["actions"][1]["digitalSourceType"].endswith(
        "compositeWithTrainedAlgorithmicMedia")
    r2 = client.post("/provenance/sign", json={"file_path": src, "action": "created"})
    rec2 = json.load(open(r2.json()["sidecar_path"], encoding="utf-8"))
    assert rec2["action"] == "created" and len(rec2["history"]) == 1
    assert rec2["history"][0]["ai_use"] == "ai_assisted"          # never discarded
    assert _sha(src) == before                                     # source untouched
    read = client.post("/provenance/read", json={"file_path": src}).json()
    assert read["sidecar"]["present"] and read["sidecar"]["sha256_matches"]
    assert read["sidecar"]["history_count"] == 1


def test_require_signature_errors(client, tmp_path, no_credentials, monkeypatch):
    src = _wav(tmp_path / "m.wav")
    assert client.post("/provenance/sign", json={"file_path": src, "require_signature": True}).status_code == 409
    monkeypatch.setattr(cc, "c2pa_status", lambda: (False, "The c2pa extra is not installed."))
    r = client.post("/provenance/sign", json={"file_path": src})
    assert r.status_code == 200 and r.json()["c2pa_available"] is False
    assert os.path.isfile(src + ".provenance.json")
    assert client.post("/provenance/sign", json={"file_path": src, "require_signature": True}).status_code == 503
    read = client.post("/provenance/read", json={"file_path": src}).json()
    assert read["c2pa_available"] is False and read["embedded"] is False


def test_bad_requests(client, tmp_path, no_credentials):
    src = _wav(tmp_path / "m.wav")
    assert client.post("/provenance/sign", json={"file_path": src, "ai_use": "maybe"}).status_code == 422
    assert client.post("/provenance/sign", json={"file_path": src, "action": "stolen"}).status_code == 422
    assert client.post("/provenance/sign", json={"file_path": src, "output_path": src}).status_code in (409, 422)
    assert client.post("/provenance/sign",
                       json={"file_path": src, "output_path": str(tmp_path / "x.mp3")}).status_code == 422


def test_sign_and_read_back_wav(client, tmp_path, credentials):
    src = _wav(tmp_path / "song.wav", info_text=b"made with suno ai")
    before = _sha(src)
    r = client.post("/provenance/sign", json={"file_path": src, "action": "edited",
                                              "ai_use": "ai_assisted", "require_signature": True})
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["signed"] and body["alg"] == "es256"
    out = body["output_path"]
    assert out.endswith("song_c2pa.wav") and os.path.isfile(out)
    assert _sha(src) == before                                   # source never modified
    assert b"made with suno ai" in open(out, "rb").read()        # other chunks kept
    assert body["sha256"] == _sha(out)
    read = client.post("/provenance/read", json={"file_path": out}).json()
    assert read["embedded"] and read["sidecar"]["sha256_matches"]
    m = read["manifest"]
    assert m["claim_generator"].startswith("Khensu Mixing")
    actions = {a["action"]: a for a in m["actions"]}
    assert "c2pa.opened" in actions and "c2pa.edited" in actions
    assert actions["c2pa.edited"]["digital_source_type"].endswith("compositeWithTrainedAlgorithmicMedia")
    assert actions["c2pa.edited"]["software_agent"].startswith("Khensu Mixing")
    assert m["trusted"] is False                                  # throwaway chain
    assert m["validation_state"] in ("Valid", "Trusted")
    assert sf.read(out)[0].shape == sf.read(src)[0].shape        # still plays

    # Re-signing a signed file keeps the earlier manifest as the parent.
    r2 = client.post("/provenance/sign", json={"file_path": out, "output_path": str(tmp_path / "song_v2.wav")})
    assert r2.status_code == 200 and r2.json()["signed"] and r2.json()["source_had_manifest"]
    m2 = client.post("/provenance/read", json={"file_path": str(tmp_path / "song_v2.wav")}).json()["manifest"]
    assert m2["manifest_count"] >= 2 and m2["ingredient_count"] == 1


def test_created_action_signs(tmp_path, credentials):
    src = _wav(tmp_path / "new.wav")
    out = cc.sign_and_record(src, str(tmp_path / "new_signed.wav"), "created", "ai_generated")
    assert out["signed"]
    m = cc.read_embedded(out["output_path"])
    assert m["actions"][0]["action"] == "c2pa.created"
    assert m["actions"][0]["digital_source_type"].endswith("/trainedAlgorithmicMedia")
