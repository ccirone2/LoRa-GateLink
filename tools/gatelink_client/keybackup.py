"""The link key's lifecycle off the board: key ids, the weak-key rule and encrypted backups ("GLKB v1").

The same format as the web console's (web/js/keys.js); docs/key-management.md describes it. Backups need the
`cryptography` package (tests/e2e/requirements.txt), imported only when one is made or opened, so everything else
here (and in tools/gatelink.py) works without it.
"""
import base64
import binascii
import datetime
import hashlib
import hmac
import json
import re
import secrets
import unicodedata

KEY_ID_LABEL = b"GateLink key id v1"  # firmware config.cpp
BACKUP_FORMAT = "gatelink-key-backup"
BACKUP_VERSION = 1
KDF_ITERATIONS = 600_000  # what a backup is written with
KDF_MAX_ITERATIONS = 10_000_000  # the most a backup may ask for
MIN_PASSPHRASE = 12  # characters
BACKUP_EXT = ".glkey"


class BackupError(Exception):
    """Not a backup, a wrong passphrase, or a changed file."""


def parse_key_hex(text):
    """Hex as typed or pasted (spaces, colons and dashes allowed) -> 16 key bytes, or None."""
    hexstr = re.sub(r"[\s:-]", "", str(text)).lower()
    return bytes.fromhex(hexstr) if re.fullmatch(r"[0-9a-f]{32}", hexstr) else None


def key_id(key):
    """The key's id: the first 4 bytes of HMAC-SHA256(key, "GateLink key id v1") as 8 lowercase hex digits, as the
    firmware reports it (info, config.get). It tells keys apart without revealing them."""
    return hmac.new(bytes(key), KEY_ID_LABEL, hashlib.sha256).hexdigest()[:8]


def weak_key_reason(key):
    """Why key.set would refuse this key (firmware 0.13.8), or None: all 16 bytes equal, bytes counting up or down by
    one (mod 256), or 8 or fewer distinct byte values."""
    key = bytes(key)
    distinct = len(set(key))
    if distinct == 1:
        return "all 16 bytes are the same"
    steps = {(b - a) % 256 for a, b in zip(key, key[1:], strict=False)}
    if steps in ({1}, {255}):
        return f"the bytes count {'up' if steps == {1} else 'down'} by one"
    if distinct <= 8:
        return f"only {distinct} different byte values (at least 9 needed)"
    return None


def generate_key():
    """A fresh random key (redrawn in the ~1 in 10^10 case that it is weak)."""
    while True:
        key = secrets.token_bytes(16)
        if not weak_key_reason(key):
            return key


def passphrase_bytes(passphrase):
    """NFC-normalized UTF-8, so a passphrase typed on another system (or in the web console) gives the same bytes."""
    return unicodedata.normalize("NFC", passphrase).encode("utf-8")


def passphrase_length(passphrase):
    return len(unicodedata.normalize("NFC", passphrase))


def backup_aad(kid):
    """Additional authenticated data: binds the ciphertext to the key id in the file."""
    return f"{BACKUP_FORMAT} v{BACKUP_VERSION} {kid}".encode()


def _crypto():
    try:
        from cryptography.hazmat.primitives import hashes
        from cryptography.hazmat.primitives.ciphers.aead import AESGCM
        from cryptography.hazmat.primitives.kdf.pbkdf2 import PBKDF2HMAC
    except ImportError:
        raise BackupError("key backups need the cryptography package: pip install -r tests/e2e/requirements.txt") \
            from None
    return hashes, AESGCM, PBKDF2HMAC


def _aes_key(passphrase, salt, iterations):
    hashes, _, PBKDF2HMAC = _crypto()
    return PBKDF2HMAC(algorithm=hashes.SHA256(), length=32, salt=salt, iterations=iterations).derive(
        passphrase_bytes(passphrase))


def _b64(data):
    return base64.b64encode(data).decode("ascii")


def _unb64(text, what, length):
    try:
        data = base64.b64decode(str(text), validate=True)
    except (binascii.Error, ValueError):
        raise BackupError(f"not a key backup: {what} isn't base64") from None
    if len(data) != length:
        raise BackupError(f"not a key backup: {what} must be {length} bytes")
    return data


def encrypt_backup(key, passphrase, note="", created=None):
    """The 16 key bytes encrypted under the passphrase: the backup as a dict (json.dumps it to save)."""
    key = bytes(key)
    if len(key) != 16:
        raise BackupError("the key must be 16 bytes")
    if passphrase_length(passphrase) < MIN_PASSPHRASE:
        raise BackupError(f"the passphrase needs at least {MIN_PASSPHRASE} characters")
    _, AESGCM, _ = _crypto()
    kid = key_id(key)
    salt, iv = secrets.token_bytes(16), secrets.token_bytes(12)
    ct = AESGCM(_aes_key(passphrase, salt, KDF_ITERATIONS)).encrypt(iv, key, backup_aad(kid))
    if created is None:
        created = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    backup = {"format": BACKUP_FORMAT, "version": BACKUP_VERSION, "key_id": kid, "created": created}
    if note:
        backup["note"] = str(note)
    backup.update({
        "kdf": {"name": "PBKDF2", "hash": "SHA-256", "iterations": KDF_ITERATIONS, "salt": _b64(salt)},
        "cipher": {"name": "AES-GCM", "iv": _b64(iv)},
        "ciphertext": _b64(ct),
    })
    return backup


def backup_file_name(backup):
    return f"gatelink-key-{backup['key_id']}{BACKUP_EXT}"


def dumps(backup):
    return json.dumps(backup, indent=2) + "\n"


def parse_backup(data):
    """A backup's text (or parsed dict) checked for shape, before asking for its passphrase."""
    b = data
    if isinstance(data, (str, bytes)):
        try:
            b = json.loads(data)
        except ValueError:
            raise BackupError("not a key backup: the file isn't JSON") from None
    if not isinstance(b, dict) or b.get("format") != BACKUP_FORMAT:
        raise BackupError("not a GateLink key backup")
    if isinstance(b.get("version"), bool) or b.get("version") != BACKUP_VERSION:  # true == 1 in Python, not in JS
        raise BackupError(f"key backup version {b.get('version')} isn't supported (this tool reads version "
                          f"{BACKUP_VERSION})")
    if not isinstance(b.get("key_id"), str) or not re.fullmatch(r"[0-9a-f]{8}", b["key_id"]):
        raise BackupError("not a key backup: bad key_id")
    kdf, cipher = b.get("kdf"), b.get("cipher")
    if not isinstance(kdf, dict) or kdf.get("name") != "PBKDF2" or kdf.get("hash") != "SHA-256":
        raise BackupError("not a key backup: unsupported kdf")
    it = kdf.get("iterations")
    if type(it) is not int or not KDF_ITERATIONS <= it <= KDF_MAX_ITERATIONS:
        raise BackupError(f"not a key backup: kdf iterations must be {KDF_ITERATIONS} to {KDF_MAX_ITERATIONS}")
    if not isinstance(cipher, dict) or cipher.get("name") != "AES-GCM":
        raise BackupError("not a key backup: unsupported cipher")
    _unb64(kdf.get("salt"), "the salt", 16)
    _unb64(cipher.get("iv"), "the iv", 12)
    _unb64(b.get("ciphertext"), "the ciphertext", 32)  # 16 key bytes + the 16-byte GCM tag
    return b


def decrypt_backup(data, passphrase):
    """The 16 key bytes in a backup. Raises BackupError for a wrong passphrase or a changed file (GCM can't tell them
    apart), and if the key inside doesn't have the file's key id."""
    b = parse_backup(data)
    _, AESGCM, _ = _crypto()
    from cryptography.exceptions import InvalidTag

    aes = _aes_key(passphrase, _unb64(b["kdf"]["salt"], "the salt", 16), b["kdf"]["iterations"])
    try:
        key = AESGCM(aes).decrypt(_unb64(b["cipher"]["iv"], "the iv", 12), _unb64(b["ciphertext"], "the ciphertext", 32),
                                  backup_aad(b["key_id"]))
    except InvalidTag:
        raise BackupError("wrong passphrase, or the backup file has been changed") from None
    kid = key_id(key)
    if kid != b["key_id"]:
        raise BackupError(f"the key in the backup has id {kid}, not the {b['key_id']} the file says")
    return key
