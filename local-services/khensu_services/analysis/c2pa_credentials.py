"""C2PA content credentials for deliverables (CE-DEL-2).

Two outputs, one set of assertions:

* **A signed C2PA manifest** embedded in a copy of the file (``c2pa-python``,
  the optional ``[c2pa]`` extra), when the user has configured a signing
  certificate chain and private key in Settings (``settings.json`` ->
  ``provenance.cert_path`` / ``provenance.key_path``). The source file is
  never modified: the signed copy goes to ``output_path`` (default
  ``<name>_c2pa<ext>`` beside it).
* **An unsigned sidecar** ``<file>.provenance.json``, ALWAYS written: the same
  actions, the file's SHA-256 and the signing outcome. When the file already
  has a sidecar, the earlier records are kept in ``history``.

The manifest records the ``c2pa.created`` action (a new master) or, for an
edit, ``c2pa.opened`` on the source as the parent ingredient followed by
``c2pa.edited``; the software agent is ``Khensu Mixing <version>``. When the
user flags AI use, the IPTC ``digitalSourceType`` says so
(``trainedAlgorithmicMedia`` for AI-generated audio,
``compositeWithTrainedAlgorithmicMedia`` for a mix that contains AI-generated
parts). C2PA 2.x requires a digitalSourceType on ``c2pa.created``, so an
unflagged new master is ``digitalCreation`` (made in software).

Never removes provenance (the Clean Metadata guarantee, CUT-5): an existing
manifest store in the source rides along as the parent ingredient, the other
RIFF / ID3 chunks of the file are left as they are (the SDK inserts its own
C2PA chunk), and an existing sidecar's records are kept.

No network: the reader and builder run with remote-manifest and OCSP fetching
off. A time-stamp authority (``provenance.tsa_url``) is contacted only when
one is configured AND the online master switch allows it.
"""
from __future__ import annotations

import datetime as _dt
import hashlib
import json
import os
from typing import Any, Optional

IPTC = "http://cv.iptc.org/newscodes/digitalsourcetype/"
AI_USE_SOURCE_TYPES: dict[str, Optional[str]] = {
    "none": None,
    "ai_assisted": IPTC + "compositeWithTrainedAlgorithmicMedia",
    "ai_generated": IPTC + "trainedAlgorithmicMedia",
}
NON_AI_CREATED_SOURCE_TYPE = IPTC + "digitalCreation"
ACTIONS = ("created", "edited")
SIDECAR_SUFFIX = ".provenance.json"
_SIDECAR_SCHEMA = "khensu.provenance.v1"
_MIME = {".wav": "audio/wav", ".wave": "audio/wav", ".mp3": "audio/mpeg", ".flac": "audio/flac",
         ".m4a": "audio/mp4", ".mp4": "audio/mp4", ".aif": "audio/aiff", ".aiff": "audio/aiff"}


class C2paUnavailable(RuntimeError):
    """The [c2pa] extra is not installed."""


class SigningNotConfigured(RuntimeError):
    """No usable certificate / key in Settings."""


def software_agent() -> dict[str, str]:
    from khensu_services import __version__
    return {"name": f"Khensu Mixing {__version__}", "version": str(__version__)}


def c2pa_status() -> tuple[bool, str]:
    """``(available, reason)`` for the optional c2pa-python SDK."""
    try:
        import c2pa  # noqa: F401
    except ImportError:
        return False, "The c2pa extra is not installed (pip install -e .[c2pa])."
    except Exception as exc:          # a broken native library must not 500 the service
        return False, f"The c2pa library failed to load: {exc}"
    return True, ""


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def mime_for(path: str) -> Optional[str]:
    return _MIME.get(os.path.splitext(path)[1].lower())


def sidecar_path(path: str) -> str:
    return path + SIDECAR_SUFFIX


# --- settings --------------------------------------------------------------------

def signing_config(settings: Optional[dict[str, Any]] = None) -> dict[str, Any]:
    """The provenance section of settings.json, checked. ``configured`` is
    True only when both the certificate chain and the key file exist."""
    if settings is None:
        from khensu_services.common.online import load_settings
        settings = load_settings()
    prov = settings.get("provenance") if isinstance(settings, dict) else None
    prov = prov if isinstance(prov, dict) else {}
    cert = str(prov.get("cert_path", "") or "").strip()
    key = str(prov.get("key_path", "") or "").strip()
    tsa = str(prov.get("tsa_url", "") or "").strip()
    reason = ""
    if not cert or not key:
        reason = "No signing certificate and key are set in Settings > Content credentials."
    elif not os.path.isfile(cert):
        reason = f"The signing certificate file does not exist: {cert}"
    elif not os.path.isfile(key):
        reason = f"The signing key file does not exist: {key}"
    return {"configured": not reason, "cert_path": cert, "key_path": key, "tsa_url": tsa, "reason": reason}


def _signing_alg(key_pem: bytes) -> str:
    """c2pa alg name for the private key (es256/es384/es512/ed25519/ps256)."""
    try:
        from cryptography.hazmat.primitives.asymmetric import ec, ed25519, rsa
        from cryptography.hazmat.primitives.serialization import load_pem_private_key
    except ImportError:
        return "es256"
    key = load_pem_private_key(key_pem, password=None)
    if isinstance(key, ec.EllipticCurvePrivateKey):
        return {256: "es256", 384: "es384", 521: "es512"}.get(key.curve.key_size, "es256")
    if isinstance(key, ed25519.Ed25519PrivateKey):
        return "ed25519"
    if isinstance(key, rsa.RSAPrivateKey):
        return "ps256"
    raise ValueError("Unsupported signing key type (use EC P-256/P-384, Ed25519 or RSA)")


def _context():
    """A c2pa Context with every network fetch off, or None on SDKs that
    reject the settings (the defaults there already skip OCSP)."""
    import c2pa
    try:
        settings = c2pa.Settings.from_dict({"verify": {"remote_manifest_fetch": False,
                                                       "ocsp_fetch": False}})
        return c2pa.Context(settings)
    except Exception:
        return None


# --- assertions ----------------------------------------------------------------

def build_actions(action: str, ai_use: str) -> list[dict[str, Any]]:
    """The c2pa.actions list for ``action`` ('created' | 'edited')."""
    if action not in ACTIONS:
        raise ValueError(f"action must be one of {ACTIONS}")
    if ai_use not in AI_USE_SOURCE_TYPES:
        raise ValueError(f"ai_use must be one of {tuple(AI_USE_SOURCE_TYPES)}")
    agent = {"name": software_agent()["name"]}
    dst = AI_USE_SOURCE_TYPES[ai_use]
    when = _dt.datetime.now(_dt.timezone.utc).replace(microsecond=0).isoformat()
    if action == "created":
        return [{"action": "c2pa.created", "softwareAgent": agent, "when": when,
                 "digitalSourceType": dst or NON_AI_CREATED_SOURCE_TYPE}]
    edited: dict[str, Any] = {"action": "c2pa.edited", "softwareAgent": agent, "when": when}
    if dst:
        edited["digitalSourceType"] = dst
    return [{"action": "c2pa.opened", "softwareAgent": agent, "when": when,
             "parameters": {"ingredientIds": ["parent"]}}, edited]


def _manifest(title: str, actions: list[dict[str, Any]]) -> dict[str, Any]:
    agent = software_agent()
    return {
        "claim_generator_info": [{"name": "Khensu Mixing", "version": agent["version"]}],
        "title": title,
        "assertions": [{"label": "c2pa.actions", "data": {"actions": actions}}],
    }


# --- sidecar -------------------------------------------------------------------

def write_sidecar(file_path: str, action: str, ai_use: str, actions: list[dict[str, Any]],
                  signing: dict[str, Any], source_path: Optional[str] = None) -> dict[str, Any]:
    """Write ``<file>.provenance.json`` for ``file_path``. An existing sidecar
    is never discarded: its current record moves into ``history``."""
    path = sidecar_path(file_path)
    record: dict[str, Any] = {
        "schema": _SIDECAR_SCHEMA,
        "file": os.path.basename(file_path),
        "sha256": sha256_file(file_path),
        "bytes": os.path.getsize(file_path),
        "created_at": _dt.datetime.now(_dt.timezone.utc).replace(microsecond=0).isoformat(),
        "software_agent": software_agent()["name"],
        "action": action,
        "ai_use": ai_use,
        "digital_source_type": AI_USE_SOURCE_TYPES.get(ai_use),
        "assertions": [{"label": "c2pa.actions", "data": {"actions": actions}}],
        "signed": bool(signing.get("signed")),
        "signing_note": signing.get("reason") or "",
    }
    if source_path and os.path.abspath(source_path) != os.path.abspath(file_path):
        record["source"] = {"file": os.path.basename(source_path), "sha256": sha256_file(source_path)}
    history: list[Any] = []
    if os.path.isfile(path):
        try:
            with open(path, encoding="utf-8") as f:
                old = json.load(f)
            if isinstance(old, dict):
                history = list(old.get("history") or [])
                old.pop("history", None)
                history.append(old)
        except (OSError, ValueError):
            # Unreadable old sidecar: keep its bytes rather than overwrite them.
            os.replace(path, path + ".bak")
    record["history"] = history
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        json.dump(record, f, indent=2)
    os.replace(tmp, path)
    return {"path": path, "record": record}


# --- sign / read ----------------------------------------------------------------

def _has_manifest(path: str) -> bool:
    import c2pa
    try:
        reader = c2pa.Reader.try_create(path, context=_context())
    except Exception:
        return False
    if reader is None:
        return False
    try:
        return bool(json.loads(reader.json()).get("active_manifest"))
    finally:
        reader.close()


def sign_file(src: str, dst: str, action: str, ai_use: str, title: str = "",
              config: Optional[dict[str, Any]] = None) -> dict[str, Any]:
    """Embed a signed manifest in a copy of ``src`` at ``dst``. Raises
    ``C2paUnavailable`` / ``SigningNotConfigured``; ``ValueError`` for an
    unsupported format or key."""
    ok, why = c2pa_status()
    if not ok:
        raise C2paUnavailable(why)
    cfg = config or signing_config()
    if not cfg["configured"]:
        raise SigningNotConfigured(cfg["reason"])
    mime = mime_for(src)
    if mime is None:
        raise ValueError(f"C2PA signing does not support {os.path.splitext(src)[1] or 'this file type'}")
    import c2pa
    from khensu_services.common.online import online_allowed

    with open(cfg["cert_path"], "rb") as f:
        certs = f.read()
    with open(cfg["key_path"], "rb") as f:
        key = f.read()
    alg = _signing_alg(key)
    tsa = cfg.get("tsa_url") or None
    tsa_used = bool(tsa) and online_allowed("c2pa_timestamp")
    info = c2pa.C2paSignerInfo(alg.encode("ascii"), certs, key, (tsa.encode("utf-8") if tsa_used else None))
    signer = c2pa.Signer.from_info(info)
    actions = build_actions(action, ai_use)
    prior = _has_manifest(src)
    ctx = _context()
    builder = c2pa.Builder(_manifest(title or os.path.basename(dst), actions), context=ctx) \
        if ctx is not None else c2pa.Builder(_manifest(title or os.path.basename(dst), actions))
    try:
        if action == "edited":
            # The source is the parent ingredient; its own manifest store
            # (if any) is carried inside the ingredient, never dropped.
            with open(src, "rb") as f:
                builder.add_ingredient({"title": os.path.basename(src), "relationship": "parentOf",
                                        "label": "parent"}, mime, f)
        tmp = dst + ".c2pa.tmp" + os.path.splitext(dst)[1]
        try:
            builder.sign_file(src, tmp, signer)
            os.replace(tmp, dst)
        finally:
            if os.path.exists(tmp):
                os.remove(tmp)
    finally:
        builder.close()
        signer.close()
    summary = read_embedded(dst)
    return {"signed": True, "output_path": dst, "alg": alg, "timestamped": tsa_used,
            "source_had_manifest": prior, "actions": actions,
            "manifest_label": (summary or {}).get("label"),
            "validation_state": (summary or {}).get("validation_state")}


def read_embedded(path: str) -> Optional[dict[str, Any]]:
    """Summary of the embedded manifest store, or None when there is none.
    Raises ``C2paUnavailable`` without the extra."""
    ok, why = c2pa_status()
    if not ok:
        raise C2paUnavailable(why)
    import c2pa
    reader = c2pa.Reader.try_create(path, context=_context())
    if reader is None:
        return None
    try:
        store = json.loads(reader.json())
        state = reader.get_validation_state()
    finally:
        reader.close()
    label = store.get("active_manifest")
    manifests = store.get("manifests") or {}
    active = manifests.get(label) or {}
    actions: list[dict[str, Any]] = []
    for a in active.get("assertions") or []:
        if str(a.get("label", "")).startswith("c2pa.actions"):
            for act in (a.get("data") or {}).get("actions") or []:
                agent = act.get("softwareAgent")
                actions.append({
                    "action": str(act.get("action", "")),
                    "software_agent": (agent.get("name") if isinstance(agent, dict) else str(agent or "")) or "",
                    "digital_source_type": act.get("digitalSourceType"),
                    "when": act.get("when"),
                })
    sig = active.get("signature_info") or {}
    codes = [str(v.get("code", "")) for v in store.get("validation_status") or []]
    return {
        "label": label,
        "title": active.get("title"),
        "claim_generator": ", ".join(str(g.get("name", "")) + " " + str(g.get("version", ""))
                                     for g in active.get("claim_generator_info") or []).strip() or None,
        "actions": actions,
        "ingredient_count": len(active.get("ingredients") or []),
        "manifest_count": len(manifests),
        "signer": sig.get("common_name") or sig.get("issuer"),
        "issuer": sig.get("issuer"),
        "signed_at": sig.get("time"),
        "validation_state": state,
        "validation_codes": codes,
        # A self-signed / private chain is reported as untrusted by every
        # validator; that is a trust-list question, not tampering.
        "trusted": "signingCredential.untrusted" not in codes,
    }


def read_credentials(path: str) -> dict[str, Any]:
    """Blocking. The embedded manifest (when the extra is installed) and the
    sidecar, with whether the sidecar's hash still matches the file."""
    digest = sha256_file(path)
    out: dict[str, Any] = {"file_path": path, "sha256": digest}
    ok, why = c2pa_status()
    out["c2pa_available"] = ok
    if ok:
        try:
            emb = read_embedded(path)
        except Exception as exc:       # malformed store: report, don't 500
            emb = None
            out["embedded_error"] = str(exc)[:300]
        out["embedded"] = emb is not None
        if emb is not None:
            out["manifest"] = emb
    else:
        out["embedded"] = False
        out["reason"] = why
    sc = sidecar_path(path)
    side: dict[str, Any] = {"path": sc, "present": os.path.isfile(sc)}
    if side["present"]:
        try:
            with open(sc, encoding="utf-8") as f:
                rec = json.load(f)
            side["sha256_matches"] = rec.get("sha256") == digest
            side["action"] = rec.get("action")
            side["ai_use"] = rec.get("ai_use")
            side["signed"] = bool(rec.get("signed"))
            side["created_at"] = rec.get("created_at")
            side["history_count"] = len(rec.get("history") or [])
        except (OSError, ValueError) as exc:
            side["error"] = f"Unreadable sidecar: {exc}"[:300]
    out["sidecar"] = side
    return out


def sign_and_record(src: str, dst: Optional[str], action: str, ai_use: str, title: str = "",
                    require_signature: bool = False,
                    config: Optional[dict[str, Any]] = None) -> dict[str, Any]:
    """Blocking. Sign when possible, and ALWAYS write the sidecar - for the
    signed copy when there is one, else for ``src`` itself."""
    actions = build_actions(action, ai_use)       # validates action / ai_use
    cfg = config or signing_config()
    ok, why = c2pa_status()
    signing: dict[str, Any] = {"signed": False}
    if not ok:
        signing["reason"] = why
    elif not cfg["configured"]:
        signing["reason"] = cfg["reason"]
    elif mime_for(src) is None:
        signing["reason"] = f"C2PA signing does not support {os.path.splitext(src)[1] or 'this file type'}."
    if signing.get("reason"):
        if require_signature:
            if not ok:
                raise C2paUnavailable(signing["reason"])
            raise SigningNotConfigured(signing["reason"])
    else:
        signing = sign_file(src, dst or default_signed_path(src), action, ai_use, title, cfg)
        actions = signing["actions"]
    target = signing["output_path"] if signing.get("signed") else src
    side = write_sidecar(target, action, ai_use, actions, signing, source_path=src)
    out: dict[str, Any] = {
        "file_path": src,
        "signed": bool(signing.get("signed")),
        "c2pa_available": ok,
        "credentials_configured": bool(cfg["configured"]),
        "sidecar_path": side["path"],
        "sha256": side["record"]["sha256"],
        "action": action,
        "ai_use": ai_use,
        "digital_source_type": AI_USE_SOURCE_TYPES[ai_use],
        "software_agent": software_agent()["name"],
    }
    if signing.get("reason"):
        out["reason"] = signing["reason"]
    for k in ("output_path", "alg", "timestamped", "source_had_manifest", "manifest_label",
              "validation_state"):
        if k in signing and signing[k] is not None:
            out[k] = signing[k]
    return out


def default_signed_path(src: str) -> str:
    base, ext = os.path.splitext(src)
    return f"{base}_c2pa{ext}"
