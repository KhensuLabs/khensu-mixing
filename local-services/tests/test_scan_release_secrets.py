"""scripts/scan_release_secrets.py: the v1.0.0 release gate against shipping
keys, tokens or the builder's personal data."""
import importlib.util
from pathlib import Path

import pytest

_SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "scan_release_secrets.py"
_spec = importlib.util.spec_from_file_location("scan_release_secrets", _SCRIPT)
assert _spec is not None and _spec.loader is not None
scanner = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(scanner)


@pytest.fixture(autouse=True)
def builder(monkeypatch):
    """A made-up builder identity, so the test never depends on (or prints)
    the real one."""
    monkeypatch.setenv("USERNAME", "probe_user")
    monkeypatch.setenv("USERPROFILE", r"C:\Users\probe_user")
    monkeypatch.setattr(scanner, "_git_email", lambda: "probe.builder@example.org")


def test_clean_tree_passes(tmp_path):
    (tmp_path / "a.py").write_text("print('hello')\n", encoding="utf-8")
    (tmp_path / "data.bin").write_bytes(b"\x00\x01binary\xff")
    assert scanner.scan([tmp_path]) == []


@pytest.mark.parametrize(
    ("name", "body", "label"),
    [
        (".env", b"X=1", "file name is never shipped"),
        ("token.txt", b"abc", "file name is never shipped"),
        ("settings.json", b"{}", "file name is never shipped"),
        ("khensu.db", b"", "file name is never shipped"),
        ("cert.pfx", b"", "file name is never shipped"),
        ("a.txt", b"-----BEGIN PRIVATE KEY-----\nMII", "PEM private key"),
        ("b.txt", b"key: sk-ant-" + b"a" * 30, "Anthropic API key"),
        ("c.txt", b"AIza" + b"B" * 35, "Google API key"),
        ("d.txt", b"hf_" + b"c" * 34, "Hugging Face token"),
        ("e.txt", b"ghp_" + b"d" * 36, "GitHub token"),
        ("f.exe", b"\x00by probe.builder@example.org\x00", "builder's git email"),
        ("g.json", b'{"path": "C:\\\\Users\\\\probe_user\\\\Music\\\\x.wav"}', "builder's user-profile path"),
        ("h.txt", b"D:/stuff C:/Users/probe_user/AppData/x", "builder's user-profile path"),
    ],
)
def test_each_kind_of_secret_is_caught(tmp_path, name, body, label):
    (tmp_path / "sub").mkdir()
    (tmp_path / "sub" / name).write_bytes(body)
    hits = scanner.scan([tmp_path])
    assert any(label in h for h in hits), hits


def test_public_ca_bundle_is_not_flagged(tmp_path):
    bundle = tmp_path / "Lib" / "site-packages" / "certifi"
    bundle.mkdir(parents=True)
    (bundle / "cacert.pem").write_text("-----BEGIN CERTIFICATE-----\n", encoding="utf-8")
    assert scanner.scan([tmp_path]) == []


def test_cli_exit_codes(tmp_path, capsys):
    assert scanner.main([str(tmp_path)]) == 0
    (tmp_path / ".env").write_text("SECRET=1", encoding="utf-8")
    assert scanner.main([str(tmp_path)]) == 1
    assert ".env" in capsys.readouterr().out
