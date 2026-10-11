"""Key ids, the weak-key rule and encrypted key backups (GLKB v1, docs/key-management.md) in
tools/gatelink_client/keybackup.py, against the firmware's vectors and a backup the web console's code wrote
(fixtures/key-backup-v1.glkey, by web/js/keys.js under Node); and `gatelink.py key`. The web side's unit tests
(tests/web/unit/keys.test.js) read fixtures/key-backup-v1-python.glkey, written by this module."""
import base64
import json
import os
import re
from pathlib import Path

import pytest
from gatelink_client import keybackup as kb

FIXTURES = Path(__file__).parent / "fixtures"
REPO_ROOT = Path(__file__).resolve().parents[2]
KEY = bytes.fromhex("8a3f1c6e9b2d4f70a1c3e5f7092b4d6f")
PASS = "correct horse battery staple"


def test_key_id_label_is_the_firmwares():
    src = (REPO_ROOT / "firmware" / "GateLink" / "config.cpp").read_text(encoding="utf-8")
    assert re.search(r'KEY_ID_LABEL\[\] = "([^"]+)"', src).group(1).encode() == kb.KEY_ID_LABEL


def test_key_ids_match_the_shared_vectors():
    fx = json.loads((REPO_ROOT / "tests" / "web" / "fixtures" / "firmware.json").read_text(encoding="utf-8"))
    assert fx["key"]["vectors"]
    for key, kid in fx["key"]["vectors"]:
        assert kb.key_id(bytes.fromhex(key)) == kid


@pytest.mark.parametrize("hexkey, reason", [
    ("00000000000000000000000000000000", "same"),
    ("000102030405060708090a0b0c0d0e0f", "up by one"),
    ("f8f9fafbfcfdfeff0001020304050607", "up by one"),
    ("0f0e0d0c0b0a09080706050403020100", "down by one"),
    ("01020304050607080102030405060708", "only 8"),
    ("deadbeefdeadbeefdeadbeefdeadbeef", "only 4"),
    ("02030405060708090102030405060708", None),
    ("00112233445566778899aabbccddeeff", None),
    ("000102030405060708090a0b0c0d0e10", None),
    ("8a3f1c6e9b2d4f70a1c3e5f7092b4d6f", None),
])
def test_weak_keys(hexkey, reason):
    got = kb.weak_key_reason(bytes.fromhex(hexkey))
    assert (got is None) if reason is None else (reason in got)


def test_generated_keys_are_16_strong_bytes():
    keys = {kb.generate_key() for _ in range(20)}
    assert len(keys) == 20
    assert all(len(k) == 16 and kb.weak_key_reason(k) is None for k in keys)


def test_parse_key_hex_takes_pasted_separators():
    assert kb.parse_key_hex("8A:3F:1C:6E 9B-2D-4F-70 a1c3e5f7092b4d6f") == KEY
    assert kb.parse_key_hex("8a3f") is None


def test_round_trip_and_the_files_fields():
    b = kb.encrypt_backup(KEY, PASS, note="bench pair")
    assert list(b) == ["format", "version", "key_id", "created", "note", "kdf", "cipher", "ciphertext"]
    assert (b["format"], b["version"], b["key_id"]) == ("gatelink-key-backup", 1, "e03fddf7")
    assert re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ", b["created"])
    assert {k: v for k, v in b["kdf"].items() if k != "salt"} == {"name": "PBKDF2", "hash": "SHA-256",
                                                                  "iterations": 600000}
    assert len(base64.b64decode(b["kdf"]["salt"])) == 16
    assert b["cipher"]["name"] == "AES-GCM" and len(base64.b64decode(b["cipher"]["iv"])) == 12
    assert len(base64.b64decode(b["ciphertext"])) == 32
    text = kb.dumps(b)
    assert KEY.hex() not in text
    assert kb.backup_file_name(b) == "gatelink-key-e03fddf7.glkey"
    assert kb.decrypt_backup(text, PASS) == KEY


def test_short_passphrase_refused():
    with pytest.raises(kb.BackupError, match="at least 12"):
        kb.encrypt_backup(KEY, "short pass")


def test_wrong_passphrase_refused():
    b = kb.encrypt_backup(KEY, PASS)
    with pytest.raises(kb.BackupError, match="wrong passphrase"):
        kb.decrypt_backup(b, PASS + "!")


def test_changed_file_refused():
    b = kb.encrypt_backup(KEY, PASS)
    ct = bytearray(base64.b64decode(b["ciphertext"]))
    ct[3] ^= 1
    with pytest.raises(kb.BackupError, match="has been changed"):
        kb.decrypt_backup({**b, "ciphertext": base64.b64encode(bytes(ct)).decode()}, PASS)
    # The key id is authenticated: relabelled to another key's id, the file doesn't open.
    with pytest.raises(kb.BackupError, match="has been changed"):
        kb.decrypt_backup({**b, "key_id": "fa60d1a7"}, PASS)
    with pytest.raises(kb.BackupError, match="iterations"):
        kb.parse_backup({**b, "kdf": {**b["kdf"], "iterations": 1000}})
    with pytest.raises(kb.BackupError, match="version 2"):
        kb.parse_backup({**b, "version": 2})
    with pytest.raises(kb.BackupError, match="iv must be 12 bytes"):
        kb.parse_backup({**b, "cipher": {**b["cipher"], "iv": base64.b64encode(b"short").decode()}})
    with pytest.raises(kb.BackupError, match="isn't JSON"):
        kb.parse_backup("{not json")
    with pytest.raises(kb.BackupError, match="unsupported kdf"):
        kb.parse_backup({**b, "kdf": "PBKDF2"})
    with pytest.raises(kb.BackupError, match="unsupported cipher"):
        kb.parse_backup({**b, "cipher": ["AES-GCM"]})
    with pytest.raises(kb.BackupError, match="version True"):
        kb.parse_backup({**b, "version": True})


def test_key_id_mismatch_refused():
    """A file sealed consistently for another key id (as a buggy or hostile writer could) is still refused."""
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM

    salt, iv = os.urandom(16), os.urandom(12)
    ct = AESGCM(kb._aes_key(PASS, salt, kb.KDF_ITERATIONS)).encrypt(iv, KEY, kb.backup_aad("fa60d1a7"))
    b64 = lambda x: base64.b64encode(x).decode()  # noqa: E731
    forged = {"format": "gatelink-key-backup", "version": 1, "key_id": "fa60d1a7", "created": "2026-10-10T00:00:00Z",
              "kdf": {"name": "PBKDF2", "hash": "SHA-256", "iterations": kb.KDF_ITERATIONS, "salt": b64(salt)},
              "cipher": {"name": "AES-GCM", "iv": b64(iv)}, "ciphertext": b64(ct)}
    with pytest.raises(kb.BackupError, match="has id e03fddf7, not the fa60d1a7"):
        kb.decrypt_backup(forged, PASS)


def test_reads_the_web_consoles_backup():
    """fixtures/key-backup-v1.glkey: written by web/js/keys.js (WebCrypto, under Node)."""
    text = (FIXTURES / "key-backup-v1.glkey").read_text(encoding="utf-8")
    assert "web/js/keys.js" in json.loads(text)["note"]
    assert kb.decrypt_backup(text, PASS) == KEY


def test_python_fixture_opens_with_either_unicode_form():
    """fixtures/key-backup-v1-python.glkey (this module's, read by the web unit tests): passphrases are NFC, so the
    decomposed form (u + combining diaeresis) opens it too."""
    text = (FIXTURES / "key-backup-v1-python.glkey").read_text(encoding="utf-8")
    want = bytes.fromhex("00112233445566778899aabbccddeeff")
    assert kb.decrypt_backup(text, "Gr\u00fc\u00dfe vom Tor, 2026") == want
    assert kb.decrypt_backup(text, "Gru\u0308\u00dfe vom Tor, 2026") == want


# gatelink.py key ...

@pytest.fixture
def cli(monkeypatch):
    """Runs `gatelink.py key ...` with the passphrases given, in order, to getpass. Needs pyserial (imported by
    gatelink.py), which the tools' requirements include."""
    gatelink = pytest.importorskip("gatelink")

    def run(*argv, passphrases=()):
        answers = iter(passphrases)
        monkeypatch.setattr(gatelink.getpass, "getpass", lambda prompt="": next(answers))
        return gatelink.main(list(argv))

    return run


def test_cli_gen_backup_restore_id(cli, tmp_path, capsys):
    key_file = tmp_path / "gatelink_key"
    assert cli("key", "gen", "--backup", str(tmp_path), "--out", str(key_file), "--note", "test",
               passphrases=[PASS, PASS]) == 0
    out = capsys.readouterr().out
    key = re.search(r"key +([0-9a-f]{32})", out).group(1)
    kid = re.search(r"key id ([0-9a-f]{8})", out).group(1)
    assert key_file.read_text().strip() == key
    backup = tmp_path / f"gatelink-key-{kid}.glkey"
    assert json.loads(backup.read_text())["note"] == "test"
    assert key not in backup.read_text()

    assert cli("key", "id", "--key-file", str(key_file)) == 0
    assert capsys.readouterr().out.strip() == kid

    restored = tmp_path / "restored"
    assert cli("key", "restore", str(backup), "--out", str(restored), passphrases=["wrong passphrase!", PASS]) == 0
    assert restored.read_text().strip() == key
    assert key not in capsys.readouterr().out  # written, not printed
    assert cli("key", "restore", str(backup), passphrases=[PASS]) == 0
    assert key in capsys.readouterr().out


def test_cli_backup_of_the_key_file(cli, tmp_path, capsys):
    key_file = tmp_path / "gatelink_key"
    key_file.write_text(KEY.hex() + "\n")
    target = tmp_path / "pair.glkey"
    assert cli("key", "backup", str(target), "--key-file", str(key_file), passphrases=[PASS, PASS]) == 0
    assert kb.decrypt_backup(target.read_text(), PASS) == KEY
    # Never overwrites a backup, and a short or mismatched passphrase is asked again.
    with pytest.raises(SystemExit, match="already exists"):
        cli("key", "backup", str(target), "--key-file", str(key_file))
    target.unlink()
    assert cli("key", "backup", str(target), "--key-file", str(key_file),
               passphrases=["short", PASS, PASS + "x", PASS, PASS]) == 0
    assert kb.decrypt_backup(target.read_text(), PASS) == KEY


def test_cli_gen_cancelled_at_the_passphrase_shows_and_writes_nothing(tmp_path, capsys, monkeypatch):
    gatelink = pytest.importorskip("gatelink")

    def interrupted(prompt=""):
        raise KeyboardInterrupt

    monkeypatch.setattr(gatelink.getpass, "getpass", interrupted)
    with pytest.raises(SystemExit, match="cancelled"):
        gatelink.main(["key", "gen", "--backup", str(tmp_path), "--out", str(tmp_path / "gatelink_key")])
    assert not re.search(r"[0-9a-f]{32}", capsys.readouterr().out)
    assert list(tmp_path.iterdir()) == []


def test_cli_restore_out_wont_replace_another_key(cli, tmp_path):
    other = tmp_path / "gatelink_key"
    other.write_text("00112233445566778899aabbccddeeff\n")
    with pytest.raises(SystemExit, match="already holds key fa60d1a7"):
        cli("key", "restore", str(FIXTURES / "key-backup-v1.glkey"), "--out", str(other), passphrases=[PASS])
    assert cli("key", "restore", str(FIXTURES / "key-backup-v1.glkey"), "--out", str(other), "--force",
               passphrases=[PASS]) == 0
    assert other.read_text().strip() == KEY.hex()


def test_ports_shows_the_key_id():
    gatelink = pytest.importorskip("gatelink")
    assert gatelink.board_key({"key_set": True, "key_id": "e03fddf7"}) == "e03fddf7"
    assert gatelink.board_key({"key_set": True}) == "set"  # firmware before 0.13.8
    assert gatelink.board_key({"key_set": False, "key_id": None}) == "NOT SET"


class FakeBoard:
    """Stands in for a board on its console: key.set (refusing weak keys, as firmware 0.13.8+ does) and info."""

    def __init__(self, reports_ids=True):
        self.key = None
        self.reports_ids = reports_ids
        self.closed = False

    def request(self, cmd, check=True, **args):
        assert cmd == "key.set"
        key = bytes.fromhex(args["key"])
        if kb.weak_key_reason(key):
            return {"ok": False, "error": "weak key"}
        self.key = key
        return {"ok": True}

    def info(self):
        return {"key_set": self.key is not None, "key_id": kb.key_id(self.key) if self.reports_ids else None}

    def close(self):
        self.closed = True


@pytest.mark.parametrize("reports_ids", [True, False])
def test_cli_key_set_writes_the_key_file_to_the_board(cli, tmp_path, capsys, monkeypatch, reports_ids):
    gatelink = pytest.importorskip("gatelink")
    board = FakeBoard(reports_ids)
    monkeypatch.setattr(gatelink, "open_target", lambda target: board)
    key_file = tmp_path / "gatelink_key"
    key_file.write_text(KEY.hex() + "\n")
    assert cli("key", "set", "gate", "--key-file", str(key_file)) == 0
    assert board.key == KEY and board.closed
    out = capsys.readouterr().out
    assert f"gate: key {kb.key_id(KEY)} set" in out and KEY.hex() not in out


def test_cli_key_set_reports_a_refusal(cli, tmp_path, capsys, monkeypatch):
    gatelink = pytest.importorskip("gatelink")
    board = FakeBoard()
    monkeypatch.setattr(gatelink, "open_target", lambda target: board)
    key_file = tmp_path / "gatelink_key"
    key_file.write_text("00" * 16 + "\n")
    assert cli("key", "set", "house", "--key-file", str(key_file)) == 1
    assert "refused: weak key" in capsys.readouterr().err and board.key is None
