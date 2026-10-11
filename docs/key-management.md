# Link key management

Both boards share one 128-bit link key. Every LoRa frame carries an HMAC-SHA256 tag made with it (truncated to
8 bytes; [protocol.md](protocol.md)), so a board acts only on frames from a board holding the same key. Without a
key the link is off, so a fresh or reset board can never be commanded.

The key is write-only: `key.set` stores it in the board's SPI flash, and no console command reads it back. This
page covers how to create, keep, check and replace it. Commands are in [console.md](console.md); the tools are
`tools/gatelink.py key …` and the web console's **Security** tab.

## Where the key lives

| Place | Form | Notes |
|---|---|---|
| Each board's SPI flash | Plaintext | Survives firmware uploads. Anyone holding the board and a flash reader or debugger can read it, so a board that leaves your hands takes the key with it (see Rotating) |
| Encrypted backup (`.glkey`) | AES-GCM under a passphrase | Safe to store anywhere on its own; useless without the passphrase |
| Password manager | The backup's passphrase (or the key itself) | The one thing that must not be lost |
| `~/.gatelink_key` (bench only) | Plaintext | What `gatelink.py restore` and the e2e suite (`GATELINK_KEY`) use. Give the bench boards their own key, never the install's |

## Key id

Each board reports a **key id** (`info`, `config.get`; firmware 0.13.8 or later): the first 4 bytes of
HMAC-SHA256(key, `"GateLink key id v1"`), as 8 hex digits, e.g. `e03fddf7`. The web console shows it on the Security
tab and in the board picker, `gatelink.py ports` lists it for each board, `gatelink.py key id` gives it for the key
in `~/.gatelink_key`, and every backup file carries it in its name and contents.

What it is:

- **A name for a key.** Two boards with the same key id hold the same key; different ids mean different keys. A
  backup's id says which key is inside without opening it.
- **A check after writing.** The web console and `gatelink.py restore` compare the id the board reports with the id
  of the key they wrote.

What it isn't:

- **Not secret, and not the key.** 4 bytes of an HMAC reveal nothing usable about the key, and the id can't
  authenticate a frame (its message starts with `G`; frames start with the protocol version, 1).
- **Not a guess-proof shield.** Someone who sees an id can test candidate keys against it offline. For a random
  128-bit key that is hopeless, but not for a guessable one, which is why `key.set` refuses weak keys (below) and the
  key must come from a random generator, never be typed up by hand.
- **Not proof of a working link.** Matching ids with the link down point at the radio settings (`net_id`, frequency,
  spreading factor…), range or power, not the key.
- **Not unique across all keys.** 32 bits: two different keys share an id about once in 4 billion pairs. For telling
  your own few keys apart that never matters.

**Weak keys.** From 0.13.8 `key.set` refuses a key whose 16 bytes are all equal, count up or down by one (mod 256),
or take 8 or fewer distinct values (`weak key: …`). The web console flags such a key before writing it (for older
firmware too), and the tools never generate one. A random key is weak about once in 10^10, and is then redrawn.

## Lifecycle

1. **Generate**, from a source you trust, on a machine you trust:
   - `python tools/gatelink.py key gen --backup DIR` prints a new key once and writes its encrypted backup into
     `DIR`. Nothing else is written unless `--out PATH` is given (e.g. `--out ~/.gatelink_key` on the bench).
   - Or the web console's **Generate**, from a local copy of the page (`python -m http.server 8000 -d web` in a
     checkout you have looked at). The hosted page works too, but a key generated there is only as trustworthy as
     the page that was served to you that day.

   Both use the operating system's random generator (`secrets`, `crypto.getRandomValues`). The tools ask for
   passphrases on the console, never on the command line: run them in a real terminal (Windows Terminal,
   PowerShell, cmd, or Git Bash through `winpty`), where a prompt without a console would otherwise wait forever.
2. **Back it up, encrypted.** `gatelink.py key gen --backup …` does it as it generates; `gatelink.py key backup FILE`
   backs up `~/.gatelink_key` (or `--key-file`); in the web console, **Save encrypted backup…** saves the key in the
   field. The passphrase needs at least 12 characters; a long random one from the password manager is best. The
   file is named `gatelink-key-<key id>.glkey`.
3. **Keep the passphrase in a password manager**, and the backup file somewhere else (another disk, a USB stick,
   cloud storage). Either alone is useless; lose the passphrase and the backup is gone for good, as nothing can
   recover it. (Keeping the key itself in the password manager instead is fine too; then the backup is a spare.)
   Delete plaintext copies you don't need.
4. **Set both boards.** Web console: connect a board, put the key in the field (**Restore from backup…** or paste
   it), **Write to board**; then the other board. Tools: `gatelink.py key restore FILE --out ~/.gatelink_key`, then
   `gatelink.py restore` (after a `snapshot`) or `gatelink.py key set house` and `gatelink.py key set gate`, which
   read the key file (never put the key itself on a command line: it lands in shell history and process lists). The link is down from the first write until the second, so the house's contact sensor reads open.
5. **Verify.** Both boards report the same key id (Security tab, the board picker, `gatelink.py ports`), the same as
   the backup's name, and the link comes up (*Peer verified* yes).
6. **Rotate** (a new key, steps 1–5, then destroy the old backups) when:
   - a board is replaced, lost, stolen, sent away (repair, a loan) or decommissioned: it holds the key in flash.
     Wipe it first if you still can (step 7), but rotate anyway if it was out of your hands;
   - the backup file and its passphrase may both have been exposed, or the passphrase alone;
   - the plaintext key may have leaked: a lost laptop with `~/.gatelink_key`, a key pasted into a chat or a ticket,
     a screenshot of the Security tab;
   - a board's key was set from a weak or hand-made value.
7. **Wipe** a board before it leaves your hands, or for good: **Factory reset** (Config tab) or `config.reset`
   erases the saved config and the key from both flash records, and the link stops at once. Check afterwards that
   the board reports no key (`key_set` false). A board on the program-flash fallback (`cfg_store` `internal`) is
   reset to defaults there.

## Backup file format (GLKB v1)

One JSON object, written by `web/js/keys.js` (WebCrypto) and `tools/gatelink_client/keybackup.py` (`cryptography`),
which read each other's files (`tests/web/unit/keys.test.js`, `tests/tools/test_keys.py` and the fixtures in
`tests/tools/fixtures/`):

```json
{
  "format": "gatelink-key-backup",
  "version": 1,
  "key_id": "e03fddf7",
  "created": "2026-10-10T07:31:07Z",
  "note": "optional, free text",
  "kdf": { "name": "PBKDF2", "hash": "SHA-256", "iterations": 600000, "salt": "<base64, 16 bytes>" },
  "cipher": { "name": "AES-GCM", "iv": "<base64, 12 bytes>" },
  "ciphertext": "<base64, 32 bytes>"
}
```

- **Key derivation:** PBKDF2-HMAC-SHA256 over the passphrase, NFC-normalized and UTF-8 encoded (so the same
  passphrase typed on another system gives the same bytes), with the random 16-byte `salt`; 32 bytes out, an
  AES-256 key. Writers use 600,000 iterations; readers accept 600,000 to 10,000,000 and refuse anything else.
- **Encryption:** AES-256-GCM with the random 12-byte `iv`. The plaintext is the 16 raw key bytes; `ciphertext` is
  the encrypted key followed by the 16-byte GCM tag. The additional authenticated data is the UTF-8 of
  `gatelink-key-backup v1 <key_id>`, so a file relabelled with another id doesn't open.
- **Reading** checks the shape first (format, version, field sizes, iterations), then decrypts: a wrong passphrase
  and a changed file both fail the GCM tag and can't be told apart. Finally the key id of the decrypted key must
  equal `key_id`.
- `created` (UTC) and `note` are informational and not authenticated: don't put secrets in the note.
- File name: `gatelink-key-<key_id>.glkey`.
