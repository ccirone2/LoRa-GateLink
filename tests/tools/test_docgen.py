"""tools/docgen.py: each checker against small synthetic sources and docs (the drift it must catch, and a clean
case), the generators' tables, --write in place, and --write then check on a copy of the real repo."""
import shutil
import textwrap
from pathlib import Path

import docgen
import pytest
from docgen import D, Fact, Q, Repo

REPO_ROOT = Path(__file__).resolve().parents[2]
FW = "firmware/GateLink"


def make(tmp_path, files, js=None):
    """A checkout holding `files` ({path: text}, dedented), with `js` standing in for the web console's tables."""
    for rel, text in files.items():
        p = tmp_path / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(textwrap.dedent(text).lstrip("\n"), encoding="utf-8")
    return Repo(tmp_path, js=js)


def has(problems, *parts):
    """True if one problem mentions every part."""
    return any(all(p in prob for p in parts) for prob in problems)


# The firmware pieces the status, command, event and pin checks read.
BASE = {
    f"{FW}/GateLink.ino": """
        static const char *resetCauseName(uint8_t rc) {
          if (rc & 0x20) return "watchdog";
          return "unknown";
        }
        const char *gateStateName(uint8_t s) {
          static const char *const n[] = { "unknown", "closed" };
          static_assert(sizeof(n) / sizeof(n[0]) == GS_COUNT, "a name per GateState");
          return s < GS_COUNT ? n[s] : "?";
        }
        const char *causeName(uint8_t c) {
          static const char *const n[] = { "none", "lora" };
          return c < 2 ? n[c] : "?";
        }
        const char *resultName(uint8_t r) {
          static const char *const n[] = { "none", "reached" };
          return r < 2 ? n[r] : "?";
        }
        void appFillStatus(JsonObject o) {
          o["fw"] = fwVersion();
          o["role"] = activeRole == ROLE_HOUSE ? "house" : activeRole == ROLE_GATE ? "gate" : "unset";
          o["reset_cause"] = resetCauseName(resetCause);
          o["cfg_store"] = configStoreName();
          JsonObject io = o["io"].to<JsonObject>();
          io["in1"] = in1.active();
          io["k1"] = k1.on();
          JsonObject l = o["link"].to<JsonObject>();
          l["rssi"] = st.lastRssi;
          if (activeRole == ROLE_HOUSE) houseStatus(o);
          else if (activeRole == ROLE_GATE) gateStatus(o);
        }
    """,
    f"{FW}/config.cpp": """
        const char *configStoreName() {
          return extFlashPresent() ? "spi" : "internal";
        }
    """,
    f"{FW}/role_house.cpp": """
        void houseStatus(JsonObject o) {
          o["gate"] = gateStateName(gateState);
          o["armed"] = armed;
          JsonObject g = o["remote"].to<JsonObject>();
          g["uptime_s"] = gateUptime;
        }
    """,
    f"{FW}/role_gate.cpp": """
        void gateStatus(JsonObject o) {
          o["gate"] = gateStateName(state);
          o["cause"] = causeName(cause);
          o["last_result"] = resultName(lastResult);
        }
    """,
    f"{FW}/console.cpp": """
        static void send(JsonDocument &doc, ConsolePort *to = nullptr) {
          serializeJson(doc, out);
        }
        static void fillParams(JsonDocument &res) {
          JsonObject params = res["params"].to<JsonObject>();
          JsonArray meta = res["meta"].to<JsonArray>();
          for (size_t i = 0; i < PARAM_COUNT; i++) {
            params[PARAMS[i].name] = 1;
            JsonObject m = meta.add<JsonObject>();
            m["name"] = PARAMS[i].name;
            m["id"] = PARAMS[i].id;
          }
        }
        static void handle(JsonDocument &req, ConsolePort &from) {
          JsonDocument res;
          res["id"] = req["id"];
          const char *cmd = req["cmd"] | "";
          if (!strcmp(cmd, "info")) {
            res["fw"] = fwVersion();
          } else if (!strcmp(cmd, "config.get")) {
            fillParams(res);
          } else if (!strcmp(cmd, "relay.test")) {
            int32_t k = req["k"] | 0;
            int32_t ms = req["ms"] | 500;
            if (k > 2) {
              res["ok"] = false;
              res["error"] = "busy";
            }
          } else if (!strcmp(cmd, "radio.ping")) {
            appPing();
          }
          send(res, &from);
        }
        void consoleEventPong(uint16_t id, uint32_t rttMs) {
          JsonDocument doc;
          doc["event"] = "pong";
          doc["ping_id"] = id;
          doc["rtt_ms"] = rttMs;
          send(doc);
        }
        void consoleEventDiag(const uint8_t *p, uint8_t len) {
          JsonDocument doc;
          doc["event"] = "remote_diag";
          static const char *const names[] = { "tx", "rx" };
          JsonObject c = doc["counters"].to<JsonObject>();
          for (int i = 0; i < 2; i++) c[names[i]] = getU16(p + 2 * i);
          send(doc);
        }
    """,
}

STATUS_DOC = """
    # Console

    ## Status

    Common fields: `fw`, `role`, `reset_cause`
    (`watchdog`, `unknown`), `cfg_store` (`spi` or `internal`, as in `info`), `io` (`in1`, `k1`) and
    `link` (`rssi`).

    - **Gate:** `gate` (`unknown`, `closed`), `cause` (`none`, `lora`), `last_result` (`none`, `reached`).
    - **House:** the gate's `gate`, plus `armed` and `remote` (`uptime_s`).

    ## Link history
"""


# ---------------------------------------------------------------------------------------------------------------
# Helpers


def test_flex_matches_across_a_line_break():
    assert docgen.re.search(docgen.flex(r"at most (\d+) s"), "at most\n10 s").group(1) == "10"
    assert docgen.flex(r"[ x] a") == r"[ x]\s+a"


def test_ticks_expands_ranges_and_split_groups_nests():
    assert docgen.ticks("`io` (`in1`–`in3`, `k1`)") == ["io", "in1", "in2", "in3", "k1"]
    top, groups = docgen.split_groups("`a` (`x` (`y`)), `b`\n(`z`)")
    assert docgen.ticks(top) == ["a", "b"]
    assert groups == {"a": ["`x` (`y`)"], "b": ["`z`"]}


def test_c_eval_and_enum_members():
    names = {"EXTFLASH_PAGE": 256, "REC_HDR": 31}
    assert docgen.c_eval("((EXTFLASH_PAGE - REC_HDR) / 5)", names) == 45
    assert docgen.c_eval("0x40000000UL") == 1 << 30
    src = """enum LogCode : uint8_t {
      EV_BOOT = 0,   // a = cause
      EV_RADIO_FAIL, // a = 0 init failed,
                     //   1 TX fault
      // a standalone note
      EV_COUNT
    };"""
    assert docgen.enum_members(src, "LogCode") == [("EV_BOOT", 0, "a = cause"),
                                                   ("EV_RADIO_FAIL", 1, "a = 0 init failed, 1 TX fault"),
                                                   ("EV_COUNT", 2, "")]


# ---------------------------------------------------------------------------------------------------------------
# Generated: settings reference

CONFIG_CPP = """
    const ParamDef PARAMS[] = {
      { 1, "role", &Config::role, 0, 2, P_REBOOT },
      { 3, "bw_hz", &Config::bw_hz, 125000, 500000, P_RADIO },
      { 9, "pulse_ms", &Config::pulse_ms, 100, 5000, P_REMOTE },
      { 12, "hidden_s", &Config::hidden_s, 1, 9, 0 },  // in no group
    };
    bool paramValid(const ParamDef *p, int32_t value) {
      if (!p || value < p->minV || value > p->maxV) return false;
      if (p->field == &Config::bw_hz && value != 125000 && value != 250000 && value != 500000) return false;
      return true;
    }
    void configDefaults(Config &c) {
      memset(&c, 0, sizeof(c));
      c.role = ROLE_UNSET;
      c.bw_hz = 500000;
      c.pulse_ms = 500;
      c.hidden_s = 3;
    }
"""
CONFIG_H = "enum Role : uint8_t { ROLE_UNSET = 0, ROLE_HOUSE = 1, ROLE_GATE = 2 };\n"
SETTINGS = {
    "GROUPS": [["General", ["role"]], ["Radio", ["bw_hz", "pulse_ms", "gone"]]],
    "HELP": {"role": "Which end.", "bw_hz": "Both boards must match | 500 kHz.", "pulse_ms": "How long; see pulse_ms."},
    "MUST_MATCH": ["bw_hz"],
    "SELECTS": {"role": [[0, "unset"], [1, "house"], [2, "gate"]], "bw_hz": [[125000, "125 kHz"], [500000, "500 kHz"]]},
}


def test_params_table_and_its_drift(tmp_path):
    repo = make(tmp_path, {f"{FW}/config.cpp": CONFIG_CPP, f"{FW}/config.h": CONFIG_H}, js=SETTINGS)
    text, probs = docgen.gen_params(repo)
    assert "| `role` | 1 | 0 (unset), 1 (house), 2 (gate) | 0 (unset) | after reboot | Which end. |" in text
    assert ("| `bw_hz` | 3 | 125000 (125 kHz), 250000, 500000 (500 kHz) | 500000 (500 kHz) | radio restart, "
            "both boards | Both boards must match \\| 500 kHz. |") in text
    assert "| `pulse_ms` | 9 | 100–5000 (0.1–5 s) | 500 (0.5 s) | at once, remote | How long; see `pulse_ms`. |" in text
    assert "## Not on the Config tab" in text and "| `hidden_s` | 12 | 1–9 | 3 | at once | — |" in text
    assert has(probs, "`hidden_s`", "isn't in GROUPS")
    assert has(probs, "`hidden_s` has no HELP")
    assert has(probs, "`gone`", "isn't a setting")
    assert has(probs, "SELECTS.bw_hz offers [125000, 500000]", "[125000, 250000, 500000]")


def test_params_clean(tmp_path):
    js = {"GROUPS": [["General", ["role", "pulse_ms"]]], "HELP": {"role": "r", "pulse_ms": "p"}, "MUST_MATCH": [],
          "SELECTS": {"role": [[0, "unset"], [1, "house"], [2, "gate"]]}}
    cpp = CONFIG_CPP.replace('{ 3, "bw_hz", &Config::bw_hz, 125000, 500000, P_RADIO },', "").replace(
        '{ 12, "hidden_s", &Config::hidden_s, 1, 9, 0 },  // in no group', "")
    repo = make(tmp_path, {f"{FW}/config.cpp": cpp, f"{FW}/config.h": CONFIG_H}, js=js)
    assert docgen.gen_params(repo)[1] == []


def test_write_fills_markers_and_keeps_crlf_then_check_is_clean(tmp_path):
    repo = make(tmp_path, {f"{FW}/config.cpp": CONFIG_CPP, f"{FW}/config.h": CONFIG_H}, js=SETTINGS)
    doc = tmp_path / "docs" / "config.md"
    doc.parent.mkdir()
    doc.write_bytes(b"# Settings\r\n\r\n<!-- docgen:params begin -->\r\nold\r\n<!-- docgen:params end -->\r\nafter\r\n")
    body = docgen.gen_params(repo)[0]
    text = docgen.block("params", body, "config.cpp")
    assert has(docgen.sync(repo, "docs/config.md", "params", text, write=False), "out of date", "--write", "'old'")
    assert docgen.sync(repo, "docs/config.md", "params", text, write=True) == []
    raw = doc.read_bytes()
    assert b"\r\n| `role` | 1 |" in raw and raw.endswith(b"<!-- docgen:params end -->\r\nafter\r\n")
    assert b"\n" not in raw.replace(b"\r\n", b"")
    assert docgen.sync(Repo(tmp_path, js=SETTINGS), "docs/config.md", "params", text, write=False) == []


def test_missing_markers_are_reported(tmp_path):
    repo = make(tmp_path, {"docs/config.md": "# Settings\n"})
    assert has(docgen.sync(repo, "docs/config.md", "params", "x", write=True), "no `<!-- docgen:params begin -->`")


# ---------------------------------------------------------------------------------------------------------------
# Generated: wire formats

ROLES_H = """
    enum GateState : uint8_t { GS_UNKNOWN = 0, GS_CLOSED, GS_COUNT };
    enum Cause : uint8_t { CAUSE_NONE = 0, CAUSE_LORA };
    enum TravelResult : uint8_t { TR_NONE = 0, TR_REACHED };
    #define ST_STATE 0
    #define ST_RSSI 1        // i16, RSSI at gate
    #define ST_LEN_V1 3      // up to 0.3.x; the fields below were added in 0.4.0
    #define ST_RETRIES 3     // u32, retries
    #define ST_LEN 5
    #define STI_IN1 0x01     // open limit
    #define STI_AC_LOST 0x40 // 0.7.0
    #define DIAG_FW 0        // u8 x3: major, minor, patch
    #define DIAG_UPTIME 3    // u32 seconds
    #define DIAG_COUNTERS 7  // u16 x DC_COUNT, saturating
    enum DiagCounter : uint8_t { DC_TX = 0, DC_RX, DC_COUNT };
    #define DIAG_HDR (DIAG_COUNTERS + 2 * DC_COUNT)
    static_assert(DIAG_HDR == 12, "DIAG header changed");
"""


def test_status_layout(tmp_path, monkeypatch):
    monkeypatch.setattr(docgen, "ST_MEANING", {})
    monkeypatch.setattr(docgen, "STI_MEANING", {"STI_AC_LOST": "no AC power"})
    repo = make(tmp_path, {f"{FW}/roles.h": ROLES_H})
    text, probs = docgen.gen_status(repo)
    assert "5 bytes, little-endian" in text
    assert "| 1 | 2 | i16 | `ST_RSSI` | RSSI at gate | — |" in text
    assert "| 3 | 2 | u32 | `ST_RETRIES` | retries | 0.4.0 |" in text
    assert "| 0 | 0x01 | `STI_IN1` | open limit |" in text
    assert "| 6 | 0x40 | `STI_AC_LOST` | no AC power (since 0.7.0) |" in text
    assert "- `GateState`: 0 `unknown`, 1 `closed`" in text
    assert has(probs, "ST_RETRIES at 3 is u32, but the next field starts 2 byte(s) later")
    assert has(probs, "ST_STATE has no description")


def test_status_meaning_for_a_removed_field_is_reported(tmp_path, monkeypatch):
    monkeypatch.setattr(docgen, "ST_MEANING", {"ST_STATE": "state", "ST_GONE": "x"})
    monkeypatch.setattr(docgen, "STI_MEANING", {})
    probs = docgen.gen_status(make(tmp_path, {f"{FW}/roles.h": ROLES_H}))[1]
    assert has(probs, "describes ST_GONE")


def test_diag_layout_and_counter_names(tmp_path):
    files = {f"{FW}/roles.h": ROLES_H, f"{FW}/config.cpp": CONFIG_CPP, f"{FW}/config.h": CONFIG_H,
             f"{FW}/console.cpp": BASE[f"{FW}/console.cpp"].replace('{ "tx", "rx" }', '{ "rx", "tx" }')}
    text, probs = docgen.gen_diag(make(tmp_path, files))
    assert "| 0 | 3 | u8 ×3 | `DIAG_FW` | firmware version: major, minor, patch |" in text
    assert "| 9 | 2 | u16 | `DC_RX` | counter `rx` (saturating) |" in text
    assert "| 11 | 5 each | u8 + i32 | — |" in text and "`pulse_ms` (9)" in text
    assert has(probs, "DIAG_HDR works out to 11 but its static_assert says 12")
    assert has(probs, "consoleEventDiag names the DIAG counters ['rx', 'tx']", "['tx', 'rx']")


def test_messages(tmp_path):
    link = """
        enum MsgType : uint8_t {
          MSG_HELLO = 1,  // challenge u32
          MSG_CMD = 4,    // cmd_id u16, action u8   (reliable, house->gate)
        };
        enum AckResult : uint8_t {
          RES_OK = 0,
          // 1 was retired
          RES_BAD = 2,    // malformed
          RES_ODD = 3,
        };
    """
    text, probs = docgen.gen_messages(make(tmp_path, {f"{FW}/link.h": link}))
    assert "| 4 | `MSG_CMD` | cmd_id u16, action u8 | house → gate | yes |" in text
    assert "| 1 | `MSG_HELLO` | challenge u32 | either | no |" in text
    assert "| 0 | `RES_OK` | done |" in text and "| 2 | `RES_BAD` | malformed |" in text
    assert has(probs, "RES_ODD has no comment")
    odd = link.replace("(reliable, house->gate)", "(reliable, house->gate, urgent)")
    probs = docgen.gen_messages(make(tmp_path / "b", {f"{FW}/link.h": odd}))[1]
    assert has(probs, "MSG_CMD's comment says (reliable, house->gate, urgent)")


def test_diag_counter_type_must_fit_the_header(tmp_path):
    roles = ROLES_H.replace("// u16 x DC_COUNT, saturating", "// u32 x DC_COUNT")
    files = {f"{FW}/roles.h": roles, f"{FW}/config.cpp": CONFIG_CPP, f"{FW}/config.h": CONFIG_H,
             f"{FW}/console.cpp": BASE[f"{FW}/console.cpp"]}
    text, probs = docgen.gen_diag(make(tmp_path, files))
    assert "| 11 | 4 | u32 | `DC_RX` | counter `rx` |" in text
    assert has(probs, "2 DIAG counters of u32", "don't end at DIAG_HDR (11)")


# ---------------------------------------------------------------------------------------------------------------
# Checked: status fields, events, commands, history


def test_status_fields_clean(tmp_path):
    repo = make(tmp_path, {**BASE, "docs/console.md": STATUS_DOC})
    assert docgen.check_status(repo) == []


def test_status_fields_drift(tmp_path):
    doc = (STATUS_DOC.replace("`watchdog`, `unknown`", "`watchdog`, `brownout`")
           .replace("(`in1`, `k1`)", "(`in1`–`in3`)")
           .replace("`cause` (`none`, `lora`)", "`cause` (`none`)")
           .replace("plus `armed` and", "plus `stale` and"))
    probs = docgen.check_status(make(tmp_path, {**BASE, "docs/console.md": doc}))
    assert has(probs, "Status (Common fields)", "`reset_cause` can be `unknown`")
    assert has(probs, "`reset_cause` lists `brownout`, which the firmware never reports")
    assert has(probs, "› `io`", "`k1` isn't documented", f"{FW}/GateLink.ino")
    assert has(probs, "› `io`", "`in2` isn't a field the firmware writes")
    assert has(probs, "› `io`", "`in3` isn't a field the firmware writes")
    assert has(probs, "Status (Gate)", "`cause` can be `lora`")
    assert has(probs, "Status (House)", "`armed` isn't documented", "role_house.cpp")
    assert has(probs, "Status (House)", "`stale` isn't a field")
    assert len(probs) == 8, probs


def test_status_fields_written_by_helpers_count(tmp_path):
    """A field a helper writes (handed the status object, or one nested in it) must be documented too."""
    files = dict(BASE)
    files[f"{FW}/GateLink.ino"] = files[f"{FW}/GateLink.ino"].replace(
        '  l["rssi"] = st.lastRssi;', '  l["rssi"] = st.lastRssi;\n          linkExtra(l);\n          supplyStatus(o);')
    files[f"{FW}/supply.cpp"] = """
        void supplyStatus(JsonObject s) {
          s["supply_mv"] = mv;
        }
        static void linkExtra(JsonObject l) {
          l["fei"] = fei;
        }
    """
    probs = docgen.check_status(make(tmp_path, {**files, "docs/console.md": STATUS_DOC}))
    assert has(probs, "Status (Common fields)", "`supply_mv` isn't documented", f"{FW}/GateLink.ino")
    assert has(probs, "› `link`", "`fei` isn't documented")
    assert len(probs) == 2, probs


def test_status_found_when_app_fill_status_moves_to_app_cpp(tmp_path):
    files = dict(BASE)
    files[f"{FW}/app.cpp"] = files.pop(f"{FW}/GateLink.ino")
    files[f"{FW}/GateLink.ino"] = "void setup() {}\nvoid loop() {}\n"
    doc = STATUS_DOC.replace("`fw`, ", "")
    probs = docgen.check_status(make(tmp_path, {**files, "docs/console.md": doc}))
    assert probs == [f"docs/console.md Status (Common fields): `fw` isn't documented: {FW}/app.cpp writes it; "
                     "describe it there"]


EVENTS_DOC = """
    | Event | Fields | When |
    |---|---|---|
    | `pong` | `ping_id`, `rtt_ms` (ms) | Answer to `radio.ping` |
    | `remote_diag` | `counters` (`tx`, `rx`) | Answer |
"""


def test_events(tmp_path):
    assert docgen.check_events(make(tmp_path, {**BASE, "docs/console.md": EVENTS_DOC})) == []
    doc = textwrap.dedent(EVENTS_DOC).replace("`rtt_ms` (ms)", "`old`").replace("(`tx`, `rx`)", "(`tx`)")
    doc += "| `ghost` | | |\n"
    probs = docgen.check_events(make(tmp_path / "b", {**BASE, "docs/console.md": doc}))
    assert has(probs, "event `pong`", "`rtt_ms` isn't documented")
    assert has(probs, "event `pong`", "`old` isn't a field")
    assert has(probs, "event `remote_diag` › `counters`", "`rx` isn't documented")
    assert has(probs, "lists `ghost`, which", "never sends")


COMMANDS_DOC = """
    | Command | Arguments | Reply / effect |
    |---|---|---|
    | `info` | | `fw` |
    | `config.get` | | `params`, `meta` (per param: `name`, `id`) |
    | `relay.test` | `k`: 1\\|2, `ms`: 50–5000 (default 500) | Pulses a relay; `busy` while one runs |
    | `radio.ping` | | A `pong` event follows |
"""


def test_commands(tmp_path):
    assert docgen.check_commands(make(tmp_path, {**BASE, "docs/console.md": COMMANDS_DOC})) == []
    doc = (COMMANDS_DOC.replace("`fw` |", "`fw`, `board` |").replace(", `ms`: 50–5000 (default 500)", "")
           .replace("`name`, `id`", "`id`").replace("| `radio.ping` | |", "| `radio.ping` | `n` |"))
    probs = docgen.check_commands(make(tmp_path / "b", {**BASE, "docs/console.md": doc}))
    assert has(probs, "`info`'s reply column names `board`")
    assert has(probs, "`relay.test` reads argument `ms`")
    assert has(probs, "`config.get` replies with `name`")
    assert has(probs, "`radio.ping` lists argument `n`, which", "never reads")
    assert len(probs) == 4, probs


def test_command_roles(tmp_path):
    ping = """          } else if (!strcmp(cmd, "radio.ping")) {"""
    cpp = BASE[f"{FW}/console.cpp"].replace(ping, """          } else if (!strcmp(cmd, "remote.diag")) {
            if (activeRole != ROLE_HOUSE) {
              res["ok"] = false;
              res["error"] = "house node only";
            }
          } else if (!strcmp(cmd, "radio.ping")) {""")
    doc = COMMANDS_DOC + "    | `remote.diag` | | House only. A `remote_diag` event follows |\n"
    assert docgen.check_commands(make(tmp_path, {**BASE, f"{FW}/console.cpp": cpp, "docs/console.md": doc})) == []
    probs = docgen.check_commands(make(tmp_path / "b", {**BASE, f"{FW}/console.cpp": cpp,
                                                         "docs/console.md": doc.replace("House only", "Gate only")}))
    assert probs == [f"docs/console.md: `remote.diag` is house only in {FW}/console.cpp, but its row says 'Gate only'"]
    probs = docgen.check_commands(make(tmp_path / "c", {**BASE, f"{FW}/console.cpp": cpp,
                                                         "docs/console.md": doc.replace("House only. ", "")}))
    assert probs == [f'docs/console.md: `remote.diag` is house only in {FW}/console.cpp: say "House only"']


def test_held_commands(tmp_path):
    cpp = """
        static bool blocksLoop(const char *cmd) {
          return !strcmp(cmd, "config.set") || !strcmp(cmd, "key.set");
        }
    """
    console = ("while a relay pulse runs (or\nwaits for its interlock start) `config.set` and `key.set` wait in the "
               "port's buffer")
    claude = "the console holds `config.set`/`config.save` and the gate queues remote config writes"
    probs = docgen.check_held_commands(make(tmp_path, {f"{FW}/console.cpp": cpp, "docs/console.md": console,
                                                       "CLAUDE.md": claude}))
    assert probs == ["CLAUDE.md: lists ['config.save', 'config.set'] as held while a relay pulses; console.cpp "
                     "blocksLoop() holds ['config.set', 'key.set']"]


def test_e2e_profile(tmp_path):
    bench = ('PROFILE_COMMON = {"hidden_s": 5, "retries": 5}\nPROFILE_HOUSE = {"in2_invert": 0}\n'
             'PROFILE_GATE = {"gone_s": 1}\nREAL_CTRL_POWER = {"pulse_ms": 400}\n')
    readme = "It pins the settings at the firmware defaults (`retries`, the input inverts; see `PROFILE_*`).\n"
    cpp = CONFIG_CPP.replace('{ 12, "hidden_s"', '{ 9, "retries", &Config::retries, 0, 10, 0 },\n'
                             '      { 15, "in2_invert", &Config::in2_invert, 0, 1, 0 },\n      { 12, "hidden_s"')
    cpp = cpp.replace("c.hidden_s = 3;", "c.hidden_s = 3;\n      c.retries = 3;")
    files = {f"{FW}/config.cpp": cpp, f"{FW}/config.h": CONFIG_H, "tests/e2e/gatelink/bench.py": bench,
             "tests/e2e/README.md": readme}
    probs = docgen.check_e2e_profile(make(tmp_path, files))
    assert has(probs, "PROFILE_COMMON sets retries = 5", "firmware default, 3")
    assert has(probs, "REAL_CTRL_POWER", "pulse_ms = 400; the default is 500")
    assert has(probs, "PROFILE_GATE sets `gone_s`, which isn't a setting")
    assert len(probs) == 3, probs


def test_history_fields(tmp_path):
    cpp = 'static const char *const FIELDS[] = {\n  "idx", "tx", "rx",\n};\n'
    doc = "## Link history\n\n| Field | Meaning |\n|---|---|\n| `idx` | n |\n| `tx`, `rx` | frames |\n\n## Events\n"
    assert docgen.check_history(make(tmp_path, {f"{FW}/history.cpp": cpp, "docs/console.md": doc})) == []
    swapped = doc.replace("`tx`, `rx`", "`rx`, `tx`")
    assert has(docgen.check_history(make(tmp_path / "b", {f"{FW}/history.cpp": cpp, "docs/console.md": swapped})),
               "another order than FIELDS")
    drift = doc.replace("`tx`, `rx`", "`tx`, `crc`")
    probs = docgen.check_history(make(tmp_path / "c", {f"{FW}/history.cpp": cpp, "docs/console.md": drift}))
    assert has(probs, "field `rx`", "isn't in the table") and has(probs, "`crc` isn't in")


# ---------------------------------------------------------------------------------------------------------------
# Checked: log events, pins, framing


def test_log_events(tmp_path):
    files = {
        f"{FW}/log.h": "enum LogCode : uint8_t {\n  EV_BOOT = 0,\n  EV_PULSE,\n  EV_SYNC,\n  EV_COUNT\n};\n",
        f"{FW}/log.cpp": 'static const char *const NAMES[] = {\n  "boot", "sync", "pulse",\n};\n',
        f"{FW}/roles.h": ROLES_H,
        f"{FW}/link.h": "enum MsgType : uint8_t {\n  MSG_HELLO = 1,\n  MSG_ACK = 3,\n};\n",
        "web/js/logdecode.js": "const MSG_TYPES = { 1: 'HELLO', 3: 'ACK' };\n",
    }
    js = {"KNOWN_EVENTS": ["boot", "pulse", "old"], "STATES": ["unknown", "closed"], "CAUSES": ["none"]}
    probs = docgen.check_logs(make(tmp_path, files, js=js))
    assert has(probs, "NAMES doesn't follow log.h LogCode at entry 1 (pulse vs sync)")
    assert has(probs, "no decoder for log event `sync`")
    assert has(probs, "decodes `old`")
    assert has(probs, "CAUSES is ['none'], but roles.h Cause is ['none', 'lora']")
    assert len(probs) == 4, probs


PINS_H = "#define PIN_K1 1\n#define PIN_K2 2\n#define PIN_IN1 A1\n#define PIN_IN2 A2\n"
HARDWARE = """
    Relays: K1 = D1, K2 = D2. Inputs: IN1–IN2 = A1–A2.

    | Terminal | House board | Gate board |
    |---|---|---|
    | IN1 (A1) | x | y |
    | IN2 (A2) | x | y |
    | K1 (D1) | x | y |
    | K2 (D2) | x | y |
"""
ROWS = [["IN1 (A1)", "a"], ["IN2 (A2)", "b"], ["K1 COM", "c"], ["K2 NO", "d"], ["VIN (5 V)", "e"], ["GND", "f"]]
LABELS = {"in1": "IN1 · x", "k1": "K1 · y"}
WIRING_JS = {"WIRING": {r: {"groups": [{"name": "g", "rows": ROWS}], "notes": []} for r in ("house", "gate")},
             "IO_LABELS": {"house": LABELS, "gate": LABELS, "unset": LABELS}}


def test_pins_clean(tmp_path):
    files = {**BASE, f"{FW}/pins.h": PINS_H, "docs/hardware.md": HARDWARE, "README.md": "K1 (D1)\n"}
    assert docgen.check_pins(make(tmp_path, files, js=WIRING_JS)) == []


def test_pins_drift(tmp_path):
    hw = HARDWARE.replace("K2 = D2", "K2 = D3").replace("| IN2 (A2) | x | y |\n", "| IN3 (A3) | x | y |\n")
    js = {"WIRING": {"house": {"groups": [{"name": "g", "rows": [*ROWS[:1], ["IN2 (A5)", "b"], *ROWS[2:],
                                                                  ["MYSTERY", "z"]]}]},
                     "gate": {"groups": [{"name": "g", "rows": ROWS[:3]}]}},
          "IO_LABELS": {"house": LABELS, "gate": {"in1": "IN1", "k1": "K2 · oops"}}}
    files = {**BASE, f"{FW}/pins.h": PINS_H, "docs/hardware.md": hw, "README.md": "wire K1 (D2)\n",
             "tools/bench-wiring/wiring.json": '{"wires": [{"from": "uno:D2", "to": "gate:IN2 (A3)"}]}\n'}
    probs = docgen.check_pins(make(tmp_path, files, js=js))
    assert has(probs, "tools/bench-wiring/wiring.json:1: IN2 (A3), but pins.h has IN2 on A2")
    assert has(probs, "docs/hardware.md:1: K2 = D3, but pins.h has D2")
    assert has(probs, "row 'IN3 (A3)' isn't `NAME (PIN)` for a pin in pins.h")
    assert has(probs, "terminal table has no row for IN2 (A2)")
    assert has(probs, "README.md:1: K1 (D2), but pins.h has K1 on D1")
    assert has(probs, "WIRING:1: IN2 (A5), but pins.h has IN2 on A2")
    assert has(probs, "WIRING.house (g): board terminal 'MYSTERY'")
    assert has(probs, "WIRING.gate has no row for K2 (D2)")
    assert has(probs, "IO_LABELS has roles ['gate', 'house'], but status `role` can be ['gate', 'house', 'unset']")
    assert has(probs, "IO_LABELS.gate.k1 = 'K2 · oops' doesn't start with K1")


def test_framing(tmp_path):
    link = "// Frame: ver | type | session(u32) | tag(8)\n"
    files = {f"{FW}/link.h": link, "docs/protocol.md": "Frame: `ver | type | session | tag`, tag = ...",
             "CLAUDE.md": "authenticated framing: `ver|type|tag`"}
    probs = docgen.check_framing(make(tmp_path, files))
    assert probs == ["CLAUDE.md: the frame layout reads ['ver', 'type', 'tag'], link.h has ['ver', 'type', 'session', "
                     "'tag']: make it `ver | type | session | tag`"]


# ---------------------------------------------------------------------------------------------------------------
# Checked: facts and quoted defaults


FACT_FILES = {
    "src.h": "#define TURN_MS 25\n#define WAIT_MS 3000\n#define DEPTH 4  // frames\n#define CAP_MS 0x40000000UL\n",
    "ver.txt": "version=0.13.0\n",
    "doc.md": ("Responses go after a 25 ms turnaround, then wait\n3 s, four deep; tops out at ~12.4 days "
               "(since 0.13.0).\n"),
}


def fact_list(doc="doc.md"):
    return [
        D("TURN_MS", "src.h", Q(doc, r"after a (\d+) ms turnaround")),
        D("WAIT_MS", "src.h", Q(doc, r"then wait (\d+) s", docgen.s)),
        D("DEPTH", "src.h", Q(doc, r"(\w+) deep")),
        D("CAP_MS", "src.h", Q(doc, r"tops out at ~([\d.]+) days", lambda v: v / 86400000, approx=True)),
        Fact("version", "ver.txt", r"version=([\d.]+)", (Q(doc, r"since ([\d.]+)"),)),
    ]


def test_facts_agree(tmp_path):
    assert docgen.check_facts(make(tmp_path, FACT_FILES), fact_list()) == []


def test_facts_drift(tmp_path):
    doc = (FACT_FILES["doc.md"].replace("25 ms", "30 ms").replace("four deep", "five deep")
           .replace("~12.4", "~12.9").replace("0.13.0", "0.12.0").replace("then wait\n3 s", "then pause 3 s"))
    probs = docgen.check_facts(make(tmp_path, {**FACT_FILES, "doc.md": doc}), fact_list())
    assert has(probs, "doc.md:1: quotes TURN_MS as '30'", "src.h gives 25: change the doc to 25")
    assert has(probs, "quotes DEPTH as 'five'", "gives 4")
    assert has(probs, "quotes CAP_MS as '12.9'", "gives 12.4")
    assert has(probs, "quotes version as '0.12.0'", "gives 0.13.0")
    assert has(probs, "doc.md: no quote of WAIT_MS", "update the quote in FACTS", "gives 3")
    assert len(probs) == 5, probs


def test_fact_source_moved(tmp_path):
    probs = docgen.check_facts(make(tmp_path, FACT_FILES), [D("GONE_MS", "src.h", Q("doc.md", r"(\d+)"))])
    assert has(probs, "src.h", "doesn't match, so GONE_MS can't be read")


def test_a_fact_that_cant_be_read_doesnt_stop_the_rest(tmp_path):
    files = {**FACT_FILES, f"{FW}/config.cpp": CONFIG_CPP, f"{FW}/config.h": CONFIG_H,
             "doc.md": FACT_FILES["doc.md"].replace("25 ms", "30 ms")}
    facts = [docgen.S("renamed_ms", Q("doc.md", r"(\d+)")), *fact_list()]
    probs = docgen.check_facts(make(tmp_path, files), facts)
    assert has(probs, "`renamed_ms` isn't a setting", "fix the docs that name it")
    assert has(probs, "quotes TURN_MS as '30'")


def test_every_fact_quote_in_the_real_repo_is_found():
    """Each quote pattern matches its doc today (the check passing means none disagrees)."""
    repo = Repo(REPO_ROOT, js={})
    for f in docgen.FACTS:
        for q in f.quotes:
            assert docgen.re.search(docgen.flex(q.pattern), repo.text(q.file), docgen.re.M), (f.name, q.file)


def test_default_quotes(tmp_path):
    hw = ("Relays pulse (`pulse_ms`, default 400 ms). Settle (`pulse_ms`\n(default 0.5 s)). Confirm `hidden_s` "
          "(default 3 s). `role`, off by default.\n")
    repo = make(tmp_path, {f"{FW}/config.cpp": CONFIG_CPP, f"{FW}/config.h": CONFIG_H, "docs/hardware.md": hw})
    probs = docgen.check_default_quotes(repo)
    assert has(probs, "docs/hardware.md:1: '`pulse_ms`, default 400 ms'", "sets 500: change it to 500 ms")
    assert len(probs) == 1, probs


@pytest.mark.parametrize("quote", [
    "`pulse_ms` (default: 400 ms)", "`pulse_ms` (400 ms by default)", "`pulse_ms` is 0.4 s by default",
    "`pulse_ms` = 400 by default", "`pulse_ms` defaults to 400 ms", "the default `pulse_ms` (0.4 s)",
    "the default `pulse_ms` of 400 ms", "the default `pulse_ms` is 400", "`hidden_s` (default 3 min)",
])
def test_default_quote_phrasings(tmp_path, quote):
    """Each way of quoting a default is checked, in any doc: wrong (pulse_ms is 500, hidden_s 3 s) is reported,
    right isn't."""
    files = {f"{FW}/config.cpp": CONFIG_CPP, f"{FW}/config.h": CONFIG_H, "tools/GateSim/README.md": f"x {quote}.\n"}
    probs = docgen.check_default_quotes(make(tmp_path, files))
    assert has(probs, "tools/GateSim/README.md:1:", "quotes the default of"), probs
    right = quote.replace("400", "500").replace("0.4", "0.5").replace("3 min", "3 s")
    files["tools/GateSim/README.md"] = f"x {right}.\n"
    assert docgen.check_default_quotes(make(tmp_path / "ok", files)) == []


def test_macro_quotes(tmp_path):
    files = {
        f"{FW}/a.cpp": "#define INTERLOCK_MS 100\n#define HOLD_S 3\n#define TWICE_MS 5\n",
        f"{FW}/b.cpp": "#define TWICE_MS 7  // another file's constant of the same name\n",
        "docs/hardware.md": ("Never within `INTERLOCK_MS` (150 ms); 0.1 s apart (`INTERLOCK_MS`). Held `HOLD_S` "
                             "(3 s), 4000 ms (`HOLD_S`). `TWICE_MS` (9 ms).\n"),
    }
    probs = docgen.check_macro_quotes(make(tmp_path, files))
    assert has(probs, "docs/hardware.md:1: '`INTERLOCK_MS` (150 ms'", "INTERLOCK_MS, which is 100 ms")
    assert has(probs, "'4000 ms (`HOLD_S`)'", "HOLD_S, which is 3000 ms")
    assert len(probs) == 2, probs


def test_names(tmp_path):
    """A backticked snake_case name in a doc must still name something in the source."""
    doc = ("`pulse_ms`, `reset_cause`, `uptime_s`, `rtt_ms`, `down_s`, `cmd_hold`, `ping_id`, `last_result`, "
           "`test_cut_at_rest` in `test_power`, `full_suite`, `cfg.key_set`, `lastCmdId`, `hist.get`\n"
           "`pulse_len_ms`, `test_gone`\n")
    files = {**BASE, f"{FW}/config.cpp": CONFIG_CPP + BASE[f"{FW}/config.cpp"], f"{FW}/config.h": CONFIG_H,
             f"{FW}/history.cpp": 'static const char *const FIELDS[] = { "idx", "down_s" };\n',
             f"{FW}/log.cpp": 'static const char *const NAMES[] = { "boot", "cmd_hold" };\n',
             "tests/e2e/test_power.py": "def test_cut_at_rest(rig):\n    pass\n",
             "tools/release_evidence.py": 'C = [Criterion("full_suite", "x")]\n', "docs/bench-testing.md": doc}
    probs = docgen.check_names(make(tmp_path, files))
    assert has(probs, "docs/bench-testing.md:2: `pulse_len_ms` names nothing in the source")
    assert has(probs, "docs/bench-testing.md:2: `test_gone` names nothing")
    assert len(probs) == 2, probs


# ---------------------------------------------------------------------------------------------------------------
# The whole run


def test_run_reports_unreadable_sources_instead_of_crashing(tmp_path):
    probs = docgen.run(make(tmp_path, {"README.md": "x\n"}, js=SETTINGS))
    assert probs and has(probs, "not found")


@pytest.mark.skipif(shutil.which("node") is None, reason="docgen reads the web console's tables through node")
def test_write_then_check_is_clean_on_the_real_repo(tmp_path):
    """--write on a copy of the repo changes nothing (the committed docs are current), and the check then passes."""
    copy = tmp_path / "repo"
    shutil.copytree(REPO_ROOT, copy, ignore=shutil.ignore_patterns(".git", "node_modules", "results", "__pycache__",
                                                                   ".pytest_cache", ".ruff_cache", "report"))
    before = {doc: (copy / doc).read_bytes() for doc, *_ in docgen.SECTIONS}
    assert docgen.run(Repo(copy), write=True) == []
    assert docgen.run(Repo(copy)) == []
    for doc, data in before.items():
        assert (copy / doc).read_bytes() == data, f"{doc} isn't current: run python tools/docgen.py --write"
