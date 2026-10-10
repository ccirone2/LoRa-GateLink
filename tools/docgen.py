#!/usr/bin/env python3
"""Reference docs from source: the tables that can be generated are, and the rest of what the docs say about the
code is checked against it, so a doc can't drift from the code without failing CI.

    python tools/docgen.py           check (CI runs it): exit 1 listing every drift with its fix, 0 if all agree
    python tools/docgen.py --write   regenerate the generated sections in place, then check the rest

Generated, between `<!-- docgen:NAME begin -->` and `<!-- docgen:NAME end -->` (edit the source, then --write):
- docs/config.md `params`: every setting, in the Config tab's groups (config.cpp PARAMS, configDefaults and
  paramValid; web/js/settings.js GROUPS, HELP, MUST_MATCH, SELECTS);
- docs/protocol.md `messages`, `status`, `diag`: message types and ACK results (link.h), the STATUS payload, its
  input bits and enums (roles.h ST_*, STI_*), the DIAG payload (roles.h DIAG_*, DiagCounter).

Checked (the doc stays hand-written):
- status fields (appFillStatus, houseStatus, gateStatus) and console events' fields against docs/console.md;
- link history fields (history.cpp FIELDS) against docs/console.md;
- log events: log.h order against log.cpp NAMES; NAMES, gate states, causes and message types against
  web/js/logdecode.js (NAMES against docs/console.md is tools/check_contract.py's);
- pins (pins.h) against every doc, the bench wiring record and web/js/wiring.js (WIRING, IO_LABELS);
- console commands against docs/console.md: the arguments each reads (req["..."]), the keys its reply carries,
  whether it is house or gate only, and the ones held while a relay pulses (blocksLoop, also in CLAUDE.md);
- the frame layout (link.h), radio settings, and the e2e suite's profile (tests/e2e/gatelink/bench.py);
- every backticked snake_case name in the docs still names something (a setting, field, argument, log event, e2e
  test or release criterion), so a rename can't live on in the prose;
- FACTS (below): constants and setting defaults the docs quote, each found in the source by one regex and in the
  docs by others; plus, without an entry, every quote of a setting's default (_DEFAULT_QUOTES: "`setting`
  (default X)", "`setting` is X by default", ...) and of a time constant next to its name ("`INTERLOCK_MS` (100
  ms)").

Stdlib only. The web console's tables are read by importing web/js/settings.js, wiring.js and logdecode.js in node.
"""
import argparse
import ast
import json
import operator
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FW = "firmware/GateLink"
# Docs scanned for quotes of a setting's default ("`setting` (default X)") or of a time constant
# ("`INTERLOCK_MS` (100 ms)"). Not TODO.md, ROADMAP.md or docs/releases/: they record what was true then.
DOC_GLOBS = ("README.md", "CLAUDE.md", "docs/*.md", "tests/*/README.md", "tools/*/README.md",
             ".claude/skills/*/SKILL.md")
FIX = "python tools/docgen.py --write"


class DocgenError(Exception):
    """The source or a doc isn't laid out the way docgen reads it: reported as a problem, not a crash."""


# ---------------------------------------------------------------------------------------------------------------
# Reading the checkout


class Repo:
    """A checkout (the real one, or a test's synthetic tree). `js` stands in for the web console's tables (tests)."""

    def __init__(self, root=ROOT, js=None):
        self.root = Path(root)
        self._js = js
        self._text = {}
        self._names = None

    def text(self, rel):
        """A file's text with \\n line ends (the Windows checkout has \\r\\n)."""
        if rel not in self._text:
            path = self.root / rel
            if not path.is_file():
                raise DocgenError(f"{rel} not found")
            with open(path, encoding="utf-8", newline="") as f:
                self._text[rel] = f.read().replace("\r\n", "\n")
        return self._text[rel]

    def write(self, rel, text):
        """Writes text back with the line ends the file had."""
        path = self.root / rel
        with open(path, encoding="utf-8", newline="") as f:
            crlf = "\r\n" in f.read()
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(text.replace("\n", "\r\n") if crlf else text)
        self._text[rel] = text

    def glob(self, pattern):
        return sorted(p.relative_to(self.root).as_posix() for p in self.root.glob(pattern) if p.is_file())

    def fw(self, name):
        return self.text(f"{FW}/{name}")

    def fw_files(self):
        return [r for r in self.glob(f"{FW}/*") if r.endswith((".ino", ".cpp", ".h"))]

    def function(self, name):
        """(file, body, parameter text) of a firmware function, wherever it is defined (functions move between
        files: appFillStatus is in GateLink.ino or app.cpp)."""
        for rel in self.fw_files():
            found = func_def(self.text(rel), name)
            if found:
                return (rel, *found)
        raise DocgenError(f"function {name}() not found in {FW}/")

    def names(self):
        """Numeric #defines and enum members of the firmware, for evaluating C constant expressions."""
        if self._names is None:
            raw, names = {}, {}
            for rel in self.fw_files():
                src = self.text(rel)
                raw.update({k: v for k, (v, _) in defines(src).items()})
                for m in re.finditer(r"\benum\s+(\w+)\b[^{;]*\{", strip_comments(src)):
                    names.update({k: v for k, v, _ in enum_members(src, m.group(1)) or []})
            for _ in range(4):  # a #define may use one defined after it
                for k, v in raw.items():
                    if k not in names:
                        try:
                            names[k] = c_eval(v, names)
                        except (ValueError, SyntaxError, TypeError, ZeroDivisionError):
                            pass
            self._names = names
        return self._names

    @property
    def js(self):
        if self._js is None:
            self._js = load_js(self.root)
        return self._js


JS_LOADER = r"""
import { pathToFileURL } from 'node:url';
import { join } from 'node:path';
const load = (f) => import(pathToFileURL(join(process.env.DOCGEN_ROOT, 'web', 'js', f)).href);
const [s, w, l] = await Promise.all([load('settings.js'), load('wiring.js'), load('logdecode.js')]);
process.stdout.write(JSON.stringify({
  GROUPS: s.GROUPS, HELP: s.HELP, MUST_MATCH: [...s.MUST_MATCH], SELECTS: s.SELECTS,
  WIRING: w.WIRING, IO_LABELS: w.IO_LABELS, KNOWN_EVENTS: l.KNOWN_EVENTS, STATES: l.STATES, CAUSES: l.CAUSES,
}));
"""


def load_js(root):
    """The web console's tables, by importing its modules in node (they don't touch the page at import)."""
    node = shutil.which("node")
    if not node:
        raise DocgenError("node not found: docgen reads web/js/settings.js, wiring.js and logdecode.js by importing "
                          "them in Node.js; install it (CI's ubuntu runners have it)")
    r = subprocess.run([node, "--input-type=module", "-e", JS_LOADER], capture_output=True, text=True,
                       encoding="utf-8", cwd=root, env={**os.environ, "DOCGEN_ROOT": str(root)}, check=False)
    if r.returncode:
        raise DocgenError(f"node couldn't import the web/js tables: {r.stderr.strip()}")
    return json.loads(r.stdout)


# ---------------------------------------------------------------------------------------------------------------
# C source


_LITERALS = re.compile(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|//[^\n]*|/\*.*?\*/', re.S)


def strip_comments(src):
    """C/C++ source without comments (string and char literals kept; a comment becomes a space)."""
    return _LITERALS.sub(lambda m: m.group(0) if m.group(0)[0] in "\"'" else " ", src)


def func_def(src, name):
    """(body, parameters) of the definition of `name` in src, comments stripped; None if not defined there."""
    code = strip_comments(src)
    masked = _LITERALS.sub(lambda m: m.group(0)[0] + " " * (len(m.group(0)) - 2) + m.group(0)[-1], code)
    for m in re.finditer(rf"\b{re.escape(name)}\s*\(((?:[^;{{}}()]|\([^()]*\))*)\)\s*(?:const\s*)?\{{", masked):
        start, depth = m.end() - 1, 0
        for i in range(start, len(masked)):
            if masked[i] == "{":
                depth += 1
            elif masked[i] == "}":
                depth -= 1
                if depth == 0:
                    return code[start + 1:i], m.group(1)
        return code[start + 1:], m.group(1)
    return None


def func_body(src, name):
    found = func_def(src, name)
    return found[0] if found else None


def defines(src):
    """{name: (value text, // comment)} of the object-like #defines in src."""
    out = {}
    for line in src.split("\n"):
        m = re.match(r"\s*#define\s+(\w+)\s+(.*?)\s*(?://\s*(.*?))?\s*$", line)
        if m:
            out[m.group(1)] = (m.group(2), m.group(3) or "")
    return out


def enum_members(src, name):
    """[(member, value, comment)] of `enum name ... { ... };`, implicit values counted up as in C. A member's
    comment is the // comment on its line, plus comment-only lines indented after their // (continuations)."""
    m = re.search(r"\benum\s+" + re.escape(name) + r"\b[^{;]*\{(.*?)\};", src, re.S)
    if not m:
        return None
    out, nxt = [], 0
    for line in m.group(1).split("\n"):
        code, _, comment = line.partition("//")
        items = [s.strip() for s in code.split(",") if s.strip()]
        if not items:
            if comment.startswith("  ") and out:
                out[-1][2] = f"{out[-1][2]} {comment.strip()}".strip()
            continue
        for item in items:
            k, _, v = (p.strip() for p in item.partition("="))
            nxt = c_eval(v) if v else nxt
            out.append([k, nxt, ""])
            nxt += 1
        out[-1][2] = comment.strip()
    return [tuple(x) for x in out]


def c_strings(text):
    """The string literals in text, unescaped."""
    return [re.sub(r"\\(.)", r"\1", s) for s in re.findall(r'"((?:\\.|[^"\\\n])*)"', text)]


def c_array(src, name):
    """String literals of `name[] = { ... }` in src."""
    m = re.search(rf"\b{re.escape(name)}\[\]\s*=\s*\{{(.*?)\}};", strip_comments(src), re.S)
    if not m:
        raise DocgenError(f"array {name}[] not found")
    return c_strings(m.group(1))


_OPS = {ast.Add: operator.add, ast.Sub: operator.sub, ast.Mult: operator.mul, ast.FloorDiv: operator.floordiv,
        ast.LShift: operator.lshift, ast.RShift: operator.rshift, ast.BitOr: operator.or_, ast.BitAnd: operator.and_}


def c_eval(expr, names=None):
    """Value of a C integer constant expression: literals (suffixes too), casts, + - * / << >> | &, known names."""
    e = re.sub(r"\b(0[xX][0-9a-fA-F]+|\d+)[uUlL]+\b", r"\1", expr.strip())
    e = re.sub(r"\(\s*(?:u?int\d+_t|unsigned|int|long|size_t)\s*\)", "", e).replace("/", "//")
    return _ev(ast.parse(e, mode="eval").body, names or {})


def _ev(n, names):
    if isinstance(n, ast.Constant) and type(n.value) is int:
        return n.value
    if isinstance(n, ast.Name) and n.id in names:
        return names[n.id]
    if isinstance(n, ast.UnaryOp) and isinstance(n.op, ast.USub):
        return -_ev(n.operand, names)
    if isinstance(n, ast.BinOp) and type(n.op) in _OPS:
        return _OPS[type(n.op)](_ev(n.left, names), _ev(n.right, names))
    raise ValueError(f"not a constant expression: {ast.unparse(n)}")


_JSON_KEY = re.compile(r'JsonObject\s+(?P<obj>\w+)\s*=\s*(?P<op>\w+)\["(?P<ok>\w+)"\]\.to<JsonObject>\(\)'
                       r'|JsonArray\s+(?P<arr>\w+)\s*=\s*(?P<ap>\w+)\["(?P<ak>\w+)"\]\.to<JsonArray>\(\)'
                       r'|JsonObject\s+(?P<item>\w+)\s*=\s*(?P<ia>\w+)\.add<JsonObject>\(\)'
                       r'|(?P<var>\w+)\["(?P<key>\w+)"\]'
                       r'|(?P<nvar>\w+)\[(?P<names>\w+)\[\w+\]\]'
                       r'|\b(?P<call>[A-Za-z_]\w*)\(\s*(?P<carg>\w+)\s*\)')


def json_tree(body, root, resolve=None):
    """Keys a function writes into the JsonObject `root`: {key: None, or {child keys} for a nested object or an
    array of objects}. One filled only with computed keys (params[def->name]) is an empty dict. With `resolve`
    (function name -> its key tree, or None), a helper handed one of the objects (linkStatus(l)) adds its keys."""
    names = {n: c_strings(v) for n, v in re.findall(r"(\w+)\[\]\s*=\s*\{([^}]*)\}", body)}
    tree = {}
    objs, arrays = {root: tree}, {}

    def child(parent, key):
        if not isinstance(parent.get(key), dict):
            parent[key] = {}
        return parent[key]

    def merge(into, sub):
        for k, v in sub.items():
            if isinstance(v, dict):
                merge(child(into, k), v)
            else:
                into.setdefault(k, None)

    for m in _JSON_KEY.finditer(body):
        if m["obj"] and m["op"] in objs:
            objs[m["obj"]] = child(objs[m["op"]], m["ok"])
        elif m["arr"] and m["ap"] in objs:
            objs[m["ap"]].setdefault(m["ak"], None)
            arrays[m["arr"]] = (objs[m["ap"]], m["ak"])
        elif m["item"] and m["ia"] in arrays:
            objs[m["item"]] = child(*arrays[m["ia"]])
        elif m["var"] and m["var"] in objs:
            objs[m["var"]].setdefault(m["key"], None)
        elif m["nvar"] in objs and m["names"] in names:
            for k in names[m["names"]]:
                objs[m["nvar"]].setdefault(k, None)
        elif m["call"] and m["carg"] in objs and resolve:
            sub = resolve(m["call"])
            if sub:
                merge(objs[m["carg"]], sub)
    return tree


def flatten(tree):
    """Every key of a key tree, at any depth."""
    return set(tree) | {x for v in tree.values() if isinstance(v, dict) for x in flatten(v)}


def fw_json(repo, func, skip=(), _seen=()):
    """(file, key tree) of a firmware function that fills a JsonObject or JsonDocument parameter, with the keys
    of the helpers it hands that object (or one nested in it) to, except those in `skip`."""
    rel, body, params = repo.function(func)
    m = re.search(r"Json(?:Object|Document)\s*&?\s*(\w+)", params)
    if not m:
        raise DocgenError(f"{rel}: {func}() takes no JsonObject")
    return rel, json_tree(body, m.group(1), helpers(repo, skip, {*_seen, func}))


def helpers(repo, skip=(), seen=()):
    """json_tree's `resolve`: the key tree of a firmware function that fills the JsonObject it is handed."""
    def resolve(name):
        if name in skip or name in seen:
            return None
        try:
            return fw_json(repo, name, skip, seen)[1]
        except DocgenError:
            return None  # not a firmware function that fills a JsonObject (a library call, a name lookup)
    return resolve


# ---------------------------------------------------------------------------------------------------------------
# Markdown


def md_section(text, heading):
    """Lines under `heading` (e.g. "## Status") up to the next heading of the same or a higher level; None if the
    doc has no such heading. Code fences are skipped (a `# comment` in one isn't a heading)."""
    level = len(heading) - len(heading.lstrip("#"))
    lines, out, inside, fence = text.split("\n"), [], False, False
    for line in lines:
        if line.startswith("```"):
            fence = not fence
        m = None if fence else re.match(r"(#+)\s", line)
        if inside:
            if m and len(m.group(1)) <= level:
                break
            out.append(line)
        elif not fence and line.strip() == heading:
            inside = True
    return "\n".join(out) if inside else None


def cells(line):
    return [c.strip() for c in re.split(r"(?<!\\)\|", line.strip())[1:-1]]


def md_table(text, header):
    """Rows (lists of cells) of the first table whose header row starts with `header`; None if there is none."""
    lines = text.split("\n")
    for i, line in enumerate(lines):
        if line.strip().startswith(header):
            rows = []
            for row in lines[i + 2:]:
                if not row.startswith("|"):
                    break
                rows.append(cells(row))
            return rows
    return None


_RANGE = re.compile(r"`([a-z_]*?)(\d+)`\s*[–-]\s*`\1(\d+)`")


def ticks(text):
    """Backticked tokens in text; a range such as `in1`–`in4` stands for all four."""
    text = _RANGE.sub(lambda m: ", ".join(f"`{m.group(1)}{i}`" for i in range(int(m.group(2)), int(m.group(3)) + 1)),
                      text)
    return re.findall(r"`([^`\n]+)`", text)


def match_paren(text, i):
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "(":
            depth += 1
        elif text[j] == ")":
            depth -= 1
            if depth == 0:
                return j
    return len(text)


def split_groups(text):
    """(text with the parenthesised group after each `token` taken out, {token: [group texts]})."""
    top, groups, pos = [], {}, 0
    pat = re.compile(r"`([^`\n]+)`\s*\(")
    while True:
        m = pat.search(text, pos)
        if not m:
            top.append(text[pos:])
            return "".join(top), groups
        close = match_paren(text, m.end() - 1)
        top.append(text[pos:m.end() - 1])
        groups.setdefault(m.group(1), []).append(text[m.end():close])
        pos = close + 1


def flex(pattern):
    """A doc regex with each literal space (outside [...]) matching any run of whitespace, line breaks included:
    the docs are wrapped by hand, and rewrapping one mustn't break a check."""
    out, cls, esc = [], False, False
    for ch in pattern:
        if esc:
            esc = False
        elif ch == "\\":
            esc = True
        elif ch == "[":
            cls = True
        elif ch == "]":
            cls = False
        elif ch == " " and not cls:
            out.append(r"\s+")
            continue
        out.append(ch)
    return "".join(out)


def line_of(text, index):
    return text.count("\n", 0, index) + 1


def code_ids(text, names=()):
    """Markdown text with C identifiers (STI_*, ST_RSSI) and the given names backticked; pipes escaped."""
    text = text.replace("|", "\\|")
    text = re.sub(r"(?<![`\w])([A-Z][A-Z0-9]*_[A-Z0-9_]*\*?|[a-z]\w*\(\))(?![`\w])", r"`\1`", text)
    if names:
        alt = "|".join(sorted((re.escape(n) for n in names), key=len, reverse=True))
        text = re.sub(rf"(?<![`\w])({alt})(?![`\w])", r"`\1`", text)
    return text


# ---------------------------------------------------------------------------------------------------------------
# Settings (config.cpp, settings.js)


@dataclass(frozen=True)
class Param:
    id: int
    name: str
    field: str
    min: int
    max: int
    flags: frozenset


_PARAM = re.compile(r'\{\s*(\d+)\s*,\s*"(\w+)"\s*,\s*&Config::(\w+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*([^}]*?)\s*\}')


def parse_params(repo):
    m = re.search(r"\bPARAMS\[\]\s*=\s*\{(.*?)\n\s*\};", strip_comments(repo.fw("config.cpp")), re.S)
    params = [Param(int(i), n, f, int(lo), int(hi), frozenset(re.findall(r"\bP_(RADIO|REMOTE|REBOOT)\b", fl)))
              for i, n, f, lo, hi, fl in _PARAM.findall(m.group(1) if m else "")]
    if not params:
        raise DocgenError(f"{FW}/config.cpp: PARAMS[] not found")
    return params


def parse_defaults(repo):
    """{Config field: (value, as written)} from configDefaults(); fields it doesn't set are 0 (it memsets)."""
    body = func_body(repo.fw("config.cpp"), "configDefaults")
    if body is None:
        raise DocgenError(f"{FW}/config.cpp: configDefaults() not found")
    vals = {m.group(1): (c_eval(m.group(2), repo.names()), m.group(2).strip())
            for m in re.finditer(r"\bc\.(\w+)\s*=\s*([^;]+);", body)}
    if not re.search(r"memset\(\s*&c\s*,\s*0\s*,", body):
        raise DocgenError(f"{FW}/config.cpp: configDefaults() no longer memsets c to 0: docgen takes unset fields as 0")
    return vals


def default_of(repo, name):
    field = next((p.field for p in parse_params(repo) if p.name == name), None)
    if field is None:
        raise DocgenError(f"`{name}` isn't a setting in {FW}/config.cpp PARAMS any more: fix the docs that name it "
                          "(and its entry in FACTS, tools/docgen.py)")
    return parse_defaults(repo).get(field, (0, "0"))[0]


def allowed_sets(repo):
    """{Config field: [values]} for the params paramValid() limits to a set (bw_hz)."""
    body = func_body(repo.fw("config.cpp"), "paramValid") or ""
    return {m.group(1): [c_eval(v, repo.names()) for v in re.findall(r"value\s*!=\s*(\w+)", m.group(2))]
            for m in re.finditer(r"p->field\s*==\s*&Config::(\w+)((?:\s*&&\s*value\s*!=\s*\w+)+)", body)}


def _human(name, v):
    """A value in the unit people read it in (500000 Hz is 500 kHz, 500 ms is 0.5 s); None if it is that already."""
    if name.endswith("_hz"):
        return f"{v / 1e6:g} MHz" if v >= 1e6 else f"{v / 1e3:g} kHz"
    if name.endswith("_ms") and (v == 0 or v >= 100):
        return f"{v / 1000:g} s"
    return None


PARAMS_LEGEND = """\
**Range** is what `config.set` accepts. **Applies:** *at once*; *radio restart* (applied at once, the radio
re-initialises); *after reboot* (the board keeps running as before until it restarts). *remote*: the house can
write the gate's over LoRa (`remote.set`). *both boards*: must be the same on both boards (marked * on the Config
tab). **What it does** is the Config tab's help text."""


def gen_params(repo):
    probs, s = [], "web/js/settings.js"
    params = parse_params(repo)
    by_name = {p.name: p for p in params}
    js = repo.js
    groups, helps, must, selects = js["GROUPS"], js["HELP"], set(js["MUST_MATCH"]), js["SELECTS"]
    grouped = [n for _, names in groups for n in names]
    for p in params:
        if p.name not in grouped:
            probs.append(f"setting `{p.name}` ({FW}/config.cpp PARAMS) isn't in GROUPS in {s}, so the Config tab "
                         "doesn't show it: add it to a group")
        if p.name not in helps:
            probs.append(f"setting `{p.name}` has no HELP text in {s}: add one (the Config tab's tooltip and its "
                         "description in docs/config.md)")
    for n in sorted({*grouped, *helps, *must, *selects} - set(by_name)):
        probs.append(f"{s} names `{n}` (GROUPS, HELP, MUST_MATCH or SELECTS), which isn't a setting in "
                     f"{FW}/config.cpp PARAMS: remove it or fix the name")
    for n in sorted({n for n in grouped if grouped.count(n) > 1}):
        probs.append(f"{s}: `{n}` is in GROUPS more than once: keep one")
    ids = [p.id for p in params]
    for i in sorted({i for i in ids if ids.count(i) > 1}):
        probs.append(f"{FW}/config.cpp PARAMS: id {i} is used by "
                     f"{', '.join(p.name for p in params if p.id == i)}: ids are permanent and unique")
    defaults, allowed = parse_defaults(repo), allowed_sets(repo)

    def values(p):
        if p.field in allowed:
            return allowed[p.field]
        if p.name in selects or p.max - p.min <= 2:
            return list(range(p.min, p.max + 1))
        return None

    for name, opts in selects.items():
        p = by_name.get(name)
        if p and sorted(v for v, _ in opts) != sorted(values(p) or []):
            probs.append(f"{s} SELECTS.{name} offers {sorted(v for v, _ in opts)}, but the firmware accepts "
                         f"{values(p) or f'{p.min}..{p.max}'}: make them the same")

    def row(p):
        v, written = defaults.get(p.field, (0, "0"))
        sel = {k: label for k, label in selects.get(p.name, [])}
        ok = p.min <= v <= p.max and (p.field not in allowed or v in allowed[p.field])
        if not ok:
            probs.append(f"{FW}/config.cpp: the default {p.name} = {v} is outside what paramValid() accepts")
        toggle = not sel and p.min == 0 and p.max == 1
        vals = values(p)
        if toggle:
            rng, dflt = "0–1", f"{v} ({'on' if v else 'off'})"
        else:
            if vals is not None and len(vals) <= 8:
                rng = ", ".join(f"{x} ({sel[x]})" if x in sel else str(x) for x in vals)
            else:
                lo, hi = _human(p.name, p.min), _human(p.name, p.max)
                rng = f"{p.min}–{p.max}" + (f" ({lo.rsplit(' ', 1)[0]}–{hi})" if lo and hi else "")
            extra = sel.get(v) or (written if re.fullmatch(r"0[xX][0-9a-fA-F]+", written) else v and _human(p.name, v))
            dflt = f"{v} ({extra})" if extra else str(v)
        when = "radio restart" if "RADIO" in p.flags else "after reboot" if "REBOOT" in p.flags else "at once"
        applies = ", ".join([when] + ["remote"] * ("REMOTE" in p.flags) + ["both boards"] * (p.name in must))
        help_text = code_ids(helps.get(p.name, "—"), [q.name for q in params if "_" in q.name])
        return f"| `{p.name}` | {p.id} | {rng} | {dflt} | {applies} | {help_text} |"

    head = ["| Setting | Id | Range | Default | Applies | What it does |", "|---|---|---|---|---|---|"]
    lines = [PARAMS_LEGEND]
    for title, names in groups:
        lines += ["", f"## {title}", "", *head, *(row(by_name[n]) for n in names if n in by_name)]
    hidden = [p for p in params if p.name not in grouped]
    if hidden:
        lines += ["", "## Not on the Config tab", "", *head, *(row(p) for p in hidden)]
    return "\n".join(lines), probs


# ---------------------------------------------------------------------------------------------------------------
# Wire formats (link.h, roles.h)

SIZES = {"u8": 1, "i8": 1, "u16": 2, "i16": 2, "u32": 4, "i32": 4}
# Meanings for STATUS fields and input bits roles.h has no comment for (or only a type and unit). A key that
# isn't a macro in roles.h any more is reported.
ST_MEANING = {
    "ST_STATE": "gate state (`GateState`, below)",
    "ST_INPUTS": "the gate's inputs and relays (bits below)",
    "ST_CAUSE": "cause of the last state change (`Cause`)",
    "ST_RESULT": "how the last command ended (`TravelResult`)",
    "ST_CMD_ID": "id of the last command the gate took",
    "ST_UPTIME": "gate uptime, s",
    "ST_SNR": "SNR at the gate of its last frame from the house, dB",
    "ST_TARGET": "where the gate's pulse is taking it (`GateState`; 0 = none)",
}
STI_MEANING = {
    "STI_K1": "K1 on (OPEN)",
    "STI_K2": "K2 on (CLOSE)",
    "STI_IN4": "IN4 (spare)",
    "STI_AC_LOST": "no AC power: IN3 off with `power_sense` on",
}
MSG_PAYLOAD = {"see roles.h": "STATUS payload, below", "see sendDiag() in role_gate.cpp": "DIAG payload, below"}
ACK_MEANING = {"RES_OK": "done"}


def _typed(comment):
    """(type, rest) of a field comment such as "u16, gate link retries"; type None if it names none."""
    m = re.match(r"([ui](?:8|16|32))\b[,:]?\s*(.*)", comment)
    return (m.group(1), m.group(2)) if m else (None, comment)


def gen_messages(repo):
    probs, src = [], repo.fw("link.h")
    types, results = enum_members(src, "MsgType"), enum_members(src, "AckResult")
    if not types or not results:
        raise DocgenError(f"{FW}/link.h: enum MsgType or AckResult not found")
    lines = ["### Message types", "",
             "**Reliable**: acknowledged (`MSG_ACK`) and resent until it is, or its lifetime ends. **Direction** and "
             "**Reliable** come from the comments in `link.h`; a type they give no direction for is sent by either "
             "board.", "",
             "| Type | Message | Payload | Direction | Reliable |", "|---|---|---|---|---|"]
    for name, value, comment in types:
        m = re.match(r"(.*?)\s*(?:\(([^()]*)\))?\s*$", comment)
        payload, notes = MSG_PAYLOAD.get(m.group(1), m.group(1)), [n.strip() for n in (m.group(2) or "").split(",")]
        way = next((n.replace("->", " → ") for n in notes if "->" in n), "either")
        unknown = [n for n in notes if n and n != "reliable" and "->" not in n]
        if unknown:
            probs.append(f"{FW}/link.h: {name}'s comment says ({m.group(2)}); docgen reads only `reliable` and a "
                         "direction (house->gate) there: update gen_messages in tools/docgen.py")
        lines.append(f"| {value} | `{name}` | {code_ids(payload) or '—'} | {way} | "
                     f"{'yes' if 'reliable' in notes else 'no'} |")
    lines += ["", "ACK result codes (`AckResult`; a gap is a retired code, never reused):", "",
              "| Code | Result | Meaning |", "|---|---|---|"]
    for name, value, comment in results:
        meaning = comment or ACK_MEANING.get(name)
        if not meaning:
            probs.append(f"{FW}/link.h: {name} has no comment (or ACK_MEANING entry in tools/docgen.py): add one")
        lines.append(f"| {value} | `{name}` | {code_ids(meaning or '—')} |")
    return "\n".join(lines), probs


def gen_status(repo):
    probs, src, names = [], repo.fw("roles.h"), repo.names()
    d = defines(src)
    where = f"{FW}/roles.h"
    try:
        settings = [p.name for p in parse_params(repo) if "_" in p.name]  # backticked where a meaning names one
    except DocgenError:
        settings = []
    fields = sorted(((k, c_eval(v, names), c) for k, (v, c) in d.items()
                     if k.startswith("ST_") and not k.startswith("ST_LEN")), key=lambda f: f[1])
    if "ST_LEN" not in d or not fields:
        raise DocgenError(f"{where}: ST_* / ST_LEN not found")
    length = c_eval(d["ST_LEN"][0], names)
    # STATUS lengths of older gates: ST_LEN_V1 16 // up to 0.3.x; the fields below were added in 0.4.0
    eras = sorted((c_eval(v, names), c) for k, (v, c) in d.items() if re.fullmatch(r"ST_LEN_V\d+", k))
    for k, (_, c) in d.items():
        if re.fullmatch(r"ST_LEN_V\d+", k) and not re.search(r"added in \d+\.\d+\.\d+", c):
            probs.append(f"{where}: {k}'s comment doesn't say which version added the fields after it "
                         "(\"... added in x.y.z\"): add it")

    def since(offset):
        version = "—"
        for end, comment in eras:
            m = re.search(r"added in (\d+\.\d+\.\d+)", comment)
            if offset >= end and m:
                version = m.group(1)
        return version

    lines = ["### STATUS payload (gate → house)", "",
             f"{length} bytes, little-endian. **Since**: the gate firmware that added the field (— = every "
             "version); an older gate's shorter STATUS still works.", "",
             "| Offset | Size | Type | Field | Meaning | Since |", "|---|---|---|---|---|---|"]
    for i, (k, off, comment) in enumerate(fields):
        nxt = fields[i + 1][1] if i + 1 < len(fields) else length
        size = nxt - off
        typ, rest = _typed(comment)
        typ = typ or ("u8" if size == 1 else None)
        if SIZES.get(typ) != size:
            probs.append(f"{where}: {k} at {off} is {typ or 'untyped'}, but the next field starts {size} byte(s) "
                         "later: fix the offsets or the type comment")
        meaning = ST_MEANING.get(k) or rest
        if not meaning:
            probs.append(f"{where}: {k} has no description: add one after its type in its comment (or to "
                         "ST_MEANING in tools/docgen.py)")
        lines.append(f"| {off} | {size} | {typ or '?'} | `{k}` | {code_ids(meaning or '—', settings)} | {since(off)} |")
    bits = sorted(((k, c_eval(v, names), c) for k, (v, c) in d.items() if k.startswith("STI_")), key=lambda b: b[1])
    lines += ["", "`ST_INPUTS` bits:", "", "| Bit | Mask | Name | Meaning |", "|---|---|---|---|"]
    for k, mask, comment in bits:
        version = re.search(r"\b(\d+\.\d+\.\d+)\b", comment)
        meaning = STI_MEANING.get(k) or comment
        if version:
            meaning += f" (since {version.group(1)})"
        lines.append(f"| {mask.bit_length() - 1} | 0x{mask:02x} | `{k}` | {meaning} |")
    for k in sorted(set(ST_MEANING) - {f[0] for f in fields}) + sorted(set(STI_MEANING) - {b[0] for b in bits}):
        probs.append(f"tools/docgen.py: ST_MEANING/STI_MEANING describes {k}, which isn't in {where} any more: "
                     "remove it")
    lines += ["", "Enums in STATUS (also the `status` reply's names):", ""]
    for enum, prefix in (("GateState", "GS_"), ("Cause", "CAUSE_"), ("TravelResult", "TR_")):
        members = enum_members(src, enum)
        if not members:
            raise DocgenError(f"{where}: enum {enum} not found")
        items = ", ".join(f"{v} `{k[len(prefix):].lower()}`" for k, v, _ in members if not k.endswith("_COUNT"))
        lines.append(f"- `{enum}`: {items}")
    return "\n".join(lines), probs


def gen_diag(repo):
    probs, src, names = [], repo.fw("roles.h"), repo.names()
    d = defines(src)
    where = f"{FW}/roles.h"
    counters = [(k, v) for k, v, _ in enum_members(src, "DiagCounter") or [] if k != "DC_COUNT"]
    if not counters or not {"DIAG_FW", "DIAG_UPTIME", "DIAG_COUNTERS", "DIAG_HDR"} <= set(d):
        raise DocgenError(f"{where}: DIAG_* or DiagCounter not found")
    off = {k: c_eval(d[k][0], names) for k in ("DIAG_FW", "DIAG_UPTIME", "DIAG_COUNTERS", "DIAG_HDR")}
    header = off["DIAG_HDR"]
    m = re.search(r"static_assert\(\s*DIAG_HDR\s*==\s*(\d+)", src)
    if m and int(m.group(1)) != header:
        probs.append(f"{where}: DIAG_HDR works out to {header} but its static_assert says {m.group(1)}")
    labels = [k[3:].lower() for k, _ in counters]
    try:
        console_names = c_array(func_body(repo.fw("console.cpp"), "consoleEventDiag") or "", "names")
    except DocgenError:
        console_names = None
    if console_names != labels:
        probs.append(f"{FW}/console.cpp consoleEventDiag names the DIAG counters {console_names}, but DiagCounter "
                     f"is {labels}: list them in DiagCounter order")
    remote = [p for p in parse_params(repo) if "REMOTE" in p.flags]
    lines = ["### DIAG payload (gate → house, answering `MSG_DIAG_REQ`)", "",
             "| Offset | Size | Type | Field | Meaning |", "|---|---|---|---|---|"]
    for k, meaning in (("DIAG_FW", "firmware version"), ("DIAG_UPTIME", "gate uptime")):
        nxt = min(v for v in off.values() if v > off[k])
        typ, rest = _typed(d[k][1])
        typ = re.sub(r"\s*x\s*(\d+)", r" ×\1", typ + re.match(r"(\s*x\s*\d+)?", rest).group(0)) if typ else "?"
        rest = re.sub(r"^x\s*\d+[:,]?\s*", "", rest)
        lines.append(f"| {off[k]} | {nxt - off[k]} | {typ} | `{k}` | {meaning}{': ' + rest if rest else ''} |")
    typ, rest = _typed(d["DIAG_COUNTERS"][1])  # "u16 x DC_COUNT, saturating, in DiagCounter order"
    size = SIZES.get(typ, 0)
    if off["DIAG_COUNTERS"] + size * len(counters) != header:
        probs.append(f"{where}: {len(counters)} DIAG counters of {typ or 'no type'} (DIAG_COUNTERS' comment) from "
                     f"{off['DIAG_COUNTERS']} don't end at DIAG_HDR ({header}): fix the comment or DIAG_HDR")
    note = " (saturating)" if "saturating" in rest else ""
    for k, v in counters:  # a DiagCounter's value is its index
        lines.append(f"| {off['DIAG_COUNTERS'] + size * v} | {size} | {typ} | `{k}` | counter "
                     f"`{k[3:].lower()}`{note} |")
    params = ", ".join(f"`{p.name}` ({p.id})" for p in remote)
    lines.append(f"| {header} | 5 each | u8 + i32 | — | (param id, value) per remote-writable param, in `PARAMS` "
                 f"order: {params} |")
    return "\n".join(lines), probs


SECTIONS = [
    ("docs/config.md", "params", gen_params,
     f"{FW}/config.cpp (PARAMS, configDefaults, paramValid) and web/js/settings.js (GROUPS, HELP, MUST_MATCH, "
     "SELECTS)"),
    ("docs/protocol.md", "messages", gen_messages, f"{FW}/link.h (MsgType, AckResult)"),
    ("docs/protocol.md", "status", gen_status, f"{FW}/roles.h (ST_*, STI_*, enums)"),
    ("docs/protocol.md", "diag", gen_diag, f"{FW}/roles.h (DIAG_*, DiagCounter) and config.cpp (PARAMS)"),
]


def block(name, body, sources):
    return (f"<!-- docgen:{name} begin -->\n"
            f"<!-- Generated by tools/docgen.py from {sources}: edit the source, then run {FIX}. -->\n\n"
            f"{body.strip()}\n\n<!-- docgen:{name} end -->")


def sync(repo, doc, name, text, write):
    """Compare (or with write, replace) the generated section `name` in doc."""
    src = repo.text(doc)
    m = re.search(rf"<!-- docgen:{name} begin -->.*?<!-- docgen:{name} end -->", src, re.S)
    if not m:
        return [f"{doc}: no `<!-- docgen:{name} begin -->` / `<!-- docgen:{name} end -->` markers: add them where "
                f"the generated {name} section goes, then run {FIX}"]
    if m.group(0) == text:
        return []
    if write:
        repo.write(doc, src[:m.start()] + text + src[m.end():])
        return []
    old, new = m.group(0).split("\n"), text.split("\n")
    i = next((i for i, (a, b) in enumerate(zip(old, new, strict=False)) if a != b), min(len(old), len(new)))
    have = old[i] if i < len(old) else "(end)"
    want = new[i] if i < len(new) else "(end)"
    return [f"{doc}:{line_of(src, m.start()) + i}: the generated `{name}` section is out of date (the source changed, "
            f"or it was edited by hand): run {FIX}. First difference: has {have[:100]!r}, source gives {want[:100]!r}"]


# ---------------------------------------------------------------------------------------------------------------
# Checks of hand-written docs


def status_trees(repo):
    """{"common"/"gate"/"house": (file, key tree)} of the status object."""
    return {"common": fw_json(repo, "appFillStatus", skip={"houseStatus", "gateStatus"}),
            "house": fw_json(repo, "houseStatus"), "gate": fw_json(repo, "gateStatus")}


def enum_values(repo):
    """The names an enumerated status field can take, from the functions that name them."""
    def names(func):  # array initialisers and return values, not static_assert messages
        body = repo.function(func)[1]
        found = re.findall(r"\[\]\s*=\s*\{([^}]*)\}", body) + re.findall(r"\breturn\b([^;]*);", body)
        return {s for text in found for s in c_strings(text) if s != "?"}
    return {"reset_cause": names("resetCauseName"), "cfg_store": names("configStoreName"),
            "gate": names("gateStateName"), "cause": names("causeName"), "last_result": names("resultName")}


def compare_tree(where, doc_text, tree, values, refs, fix):
    """Fields named in doc_text against the source's key tree; groups after a nested object's name are compared
    with its keys, groups after an enumerated field with its names."""
    probs = []
    top, groups = split_groups(doc_text)
    doc_keys = set(ticks(top))
    for k in sorted(set(tree) - doc_keys):
        probs.append(f"{where}: `{k}` isn't documented: {fix}")
    for k in sorted(doc_keys - set(tree)):
        probs.append(f"{where}: `{k}` isn't a field the firmware writes: remove it or fix the name")
    for k, texts in groups.items():
        sub = "\n".join(texts)
        if isinstance(tree.get(k), dict) and tree[k]:
            probs += compare_tree(f"{where} › `{k}`", sub, tree[k], values, refs, fix)
        elif k in tree and k in values:
            named = set(ticks(split_groups(sub)[0]))
            for v in sorted(values[k] - named):
                probs.append(f"{where}: `{k}` can be `{v}`, which isn't listed with it: add it")
            for v in sorted(named - values[k] - refs):
                probs.append(f"{where}: `{k}` lists `{v}`, which the firmware never reports: remove it")
    return probs


def check_status(repo):
    doc = "docs/console.md"
    text = repo.text(doc)
    sec = md_section(text, "## Status")
    if sec is None:
        return [f"{doc}: no `## Status` section"]
    m_gate, m_house = re.search(r"^- \*\*Gate:\*\*", sec, re.M), re.search(r"^- \*\*House:\*\*", sec, re.M)
    if not m_gate or not m_house:
        return [f"{doc} Status: the `- **Gate:**` and `- **House:**` bullets weren't found"]
    parts = {"common": sec[:m_gate.start()], "gate": sec[m_gate.start():m_house.start()],
             "house": sec[m_house.start():]}
    values = enum_values(repo)
    refs = set(re.findall(r'strcmp\(cmd,\s*"([^"]+)"\)', repo.fw("console.cpp")))
    probs = []
    for part, (rel, tree) in status_trees(repo).items():
        refs |= set(tree)
        heading = {"common": "Common fields", "gate": "Gate", "house": "House"}[part]
        probs += compare_tree(f"{doc} Status ({heading})", parts[part], tree, values, refs,
                              f"{rel} writes it; describe it there")
    return probs


def console_events(repo):
    """{event name: (function, key tree)} of the unsolicited lines console.cpp sends."""
    src, events = repo.fw("console.cpp"), {}
    for name in re.findall(r"\bvoid\s+(\w+)\s*\([^;{)]*\)\s*\{", strip_comments(src)):
        body = func_body(src, name)
        m = re.search(r'doc\["event"\]\s*=\s*"(\w+)"', body or "")
        if m:
            tree = json_tree(body, "doc", helpers(repo, skip={"send"}, seen={name}))
            tree.pop("event", None)
            events[m.group(1)] = (name, tree)
    return events


def check_events(repo):
    doc = "docs/console.md"
    rows = md_table(repo.text(doc), "| Event | Fields | When |")
    if rows is None:
        return [f"{doc}: no `| Event | Fields | When |` table"]
    events = console_events(repo)
    probs, documented = [], {}
    for r in rows:
        for e in ticks(r[0]):
            documented[e] = r[1]
    for e in sorted(set(events) - set(documented)):
        probs.append(f"{doc}: console event `{e}` ({FW}/console.cpp {events[e][0]}) isn't in the events table: add it")
    for e in sorted(set(documented) - set(events)):
        probs.append(f"{doc}: the events table lists `{e}`, which {FW}/console.cpp never sends: remove it")
    for e in sorted(set(events) & set(documented)):
        probs += compare_tree(f"{doc} event `{e}`", documented[e], events[e][1], {}, set(),
                              f"{events[e][0]}() in console.cpp sends it; add it to the Fields column")
    return probs


def check_history(repo):
    doc = "docs/console.md"
    fields = c_array(repo.fw("history.cpp"), "FIELDS")
    sec = md_section(repo.text(doc), "## Link history")
    rows = md_table(sec or "", "| Field | Meaning |")
    if rows is None:
        return [f"{doc}: no `| Field | Meaning |` table under `## Link history`"]
    listed = [t for r in rows for t in ticks(r[0])]
    probs = [f"{doc} link history: field `{f}` ({FW}/history.cpp FIELDS) isn't in the table: add a row"
             for f in fields if f not in listed]
    probs += [f"{doc} link history: `{f}` isn't in {FW}/history.cpp FIELDS: remove it"
              for f in listed if f not in fields]
    if not probs and listed != fields:
        probs.append(f"{doc} link history: the table lists the fields in another order than FIELDS (the reply's "
                     f"`fields`): {', '.join(fields)}")
    return probs


def check_logs(repo):
    probs, js = [], repo.js
    names = c_array(repo.fw("log.cpp"), "NAMES")
    codes = [k[3:].lower() for k, _, _ in enum_members(repo.fw("log.h"), "LogCode") or [] if k != "EV_COUNT"]
    if codes != names:
        i = next((i for i, (a, b) in enumerate(zip(codes, names, strict=False)) if a != b), min(len(codes), len(names)))
        probs.append(f"{FW}/log.cpp NAMES doesn't follow log.h LogCode at entry {i} "
                     f"({codes[i] if i < len(codes) else '-'} vs {names[i] if i < len(names) else '-'}): "
                     "the board would log events under the wrong names")
    known = js["KNOWN_EVENTS"]
    probs += [f"web/js/logdecode.js has no decoder for log event `{n}` ({FW}/log.cpp): add one to DECODE"
              for n in names if n not in known]
    probs += [f"web/js/logdecode.js decodes `{n}`, which {FW}/log.cpp never logs: remove it" for n in known
              if n not in names]
    roles, link = repo.fw("roles.h"), repo.fw("link.h")
    for label, enum, prefix, have in (("STATES", "GateState", "GS_", js["STATES"]),
                                      ("CAUSES", "Cause", "CAUSE_", js["CAUSES"])):
        want = [k[len(prefix):].lower() for k, _, _ in enum_members(roles, enum) or [] if not k.endswith("_COUNT")]
        if have != want:
            probs.append(f"web/js/logdecode.js {label} is {have}, but roles.h {enum} is {want}: make them the same")
    m = re.search(r"const MSG_TYPES = \{(.*?)\};", repo.text("web/js/logdecode.js"), re.S)
    have = {int(k): v for k, v in re.findall(r"(\d+):\s*'(\w+)'", m.group(1) if m else "")}
    want = {v: k[4:] for k, v, _ in enum_members(link, "MsgType") or []}
    if have != want:
        probs.append(f"web/js/logdecode.js MSG_TYPES is {have}, but link.h MsgType is {want}: make them the same")
    return probs


def check_pins(repo):
    probs, js = [], repo.js
    pins = {}
    for k, (v, _) in defines(repo.fw("pins.h")).items():
        if k.startswith("PIN_"):
            pins[k[4:]] = f"D{v}" if v.isdigit() else v
    if not pins:
        return [f"{FW}/pins.h: no PIN_* defines"]
    hw = "docs/hardware.md"
    rows = md_table(repo.text(hw), "| Terminal |")
    if rows is None:
        probs.append(f"{hw}: no `| Terminal | ...` table")
        rows = []
    seen = set()
    for r in rows:
        m = re.fullmatch(r"(\w+) \((\w+)\)", r[0])
        if not m or m.group(1) not in pins:
            probs.append(f"{hw}: terminal table row {r[0]!r} isn't `NAME (PIN)` for a pin in pins.h")
            continue
        seen.add(m.group(1))
    probs += [f"{hw}: the terminal table has no row for {t} ({pins[t]}): add one" for t in pins if t not in seen]

    def labels(where, text):
        for m in re.finditer(r"\b(K\d|IN\d) \(([AD]\d+)\)", text):
            if pins.get(m.group(1)) != m.group(2):
                probs.append(f"{where}:{line_of(text, m.start())}: {m.group(0)}, but pins.h has {m.group(1)} on "
                             f"{pins.get(m.group(1), 'no pin')}")
        for m in re.finditer(r"(?<![–\w-])(K\d|IN\d) = ([AD]\d+)\b", text):  # not the IN4 of IN1–IN4 = A1–A4
            if pins.get(m.group(1)) != m.group(2):
                probs.append(f"{where}:{line_of(text, m.start())}: {m.group(0)}, but pins.h has {pins.get(m.group(1))}")
        for m in re.finditer(r"\b([A-Z]+)(\d)–\1(\d) = ([AD])(\d+)–\4(\d+)", text):
            for i in range(int(m.group(3)) - int(m.group(2)) + 1):
                t, p = f"{m.group(1)}{int(m.group(2)) + i}", f"{m.group(4)}{int(m.group(5)) + i}"
                if pins.get(t) != p:
                    probs.append(f"{where}:{line_of(text, m.start())}: {m.group(0)} puts {t} on {p}, but pins.h has "
                                 f"{pins.get(t)}")

    # Every doc, and the bench wiring record (its terminals are the board's, e.g. "gate:IN1 (A1)").
    for doc in sorted({r for g in (*DOC_GLOBS, "tools/bench-wiring/wiring.json") for r in repo.glob(g)}):
        labels(doc, repo.text(doc))
    labels("web/js/wiring.js WIRING", json.dumps(js["WIRING"], ensure_ascii=False))
    power = {"3.3 V (VCC)", "GND", "VIN (5 V)"}
    for role, w in js["WIRING"].items():
        used = set()
        for g in w["groups"]:
            for board, _ in g["rows"]:
                m = re.fullmatch(r"(\w+) \((\w+)\)", board) or re.fullmatch(r"(K\d) (?:COM|NO|NC)", board)
                if m and m.group(1) in pins:
                    used.add(m.group(1))
                elif board not in power:
                    probs.append(f"web/js/wiring.js WIRING.{role} ({g['name']}): board terminal {board!r} is neither a "
                                 f"pins.h terminal nor one of {sorted(power)}")
        probs += [f"web/js/wiring.js WIRING.{role} has no row for {t} ({pins[t]}): add it (spare inputs too)"
                  for t in pins if t not in used]
    rel, body, _ = repo.function("appFillStatus")
    io = fw_json(repo, "appFillStatus", skip={"houseStatus", "gateStatus"})[1].get("io") or {}
    role = re.search(r'\bo\["role"\]\s*=\s*([^;]+);', body)
    roles = set(c_strings(role.group(1))) if role else set()
    if set(js["IO_LABELS"]) != roles:
        probs.append(f"web/js/wiring.js IO_LABELS has roles {sorted(js['IO_LABELS'])}, but status `role` can be "
                     f"{sorted(roles)} ({rel})")
    for role, lab in js["IO_LABELS"].items():
        if set(lab) != set(io):
            probs.append(f"web/js/wiring.js IO_LABELS.{role} has {sorted(lab)}, but status `io` has {sorted(io)}")
        probs += [f"web/js/wiring.js IO_LABELS.{role}.{k} = {v!r} doesn't start with {k.upper()}"
                  for k, v in lab.items() if not v.startswith(k.upper())]
    return probs


GENERIC_KEYS = {"id", "ok", "error"}  # in every request or reply


def command_blocks(repo):
    """{command: its branch of console.cpp handle()}."""
    body = func_body(repo.fw("console.cpp"), "handle")
    if body is None:
        raise DocgenError(f"{FW}/console.cpp: handle() not found")
    parts = re.split(r'strcmp\(cmd,\s*"([^"]+)"\)', body)
    return dict(zip(parts[1::2], parts[2::2], strict=True))


def reply_tree(repo, block):
    """Keys a command's reply carries: res["..."] in its branch, and what the helpers it hands `res` write."""
    tree = json_tree(block, "res")
    for m in re.finditer(r"\b(\w+)\(\s*res\s*(?:\.as<JsonObject>\(\)\s*)?[,)]", block):
        try:
            tree.update(fw_json(repo, m.group(1))[1])
        except DocgenError:
            pass  # not a function that fills it (send())
    return tree


def check_commands(repo):
    """Each command's documented arguments against what handle() reads (req["..."]), and the keys its reply
    column names against what the reply carries. Command names themselves are tools/check_contract.py's."""
    doc = "docs/console.md"
    rows = md_table(repo.text(doc), "| Command | Arguments |")
    if rows is None:
        return [f"{doc}: no `| Command | Arguments |` table"]
    blocks = command_blocks(repo)
    other = set(blocks) | set(console_events(repo)) | GENERIC_KEYS
    probs = []
    for r in rows:
        for cmd in ticks(r[0]):
            if cmd not in blocks or len(r) < 3:
                continue
            blk = blocks[cmd]
            reads = set(re.findall(r'req\["(\w+)"\]', blk))
            documented = set(ticks(split_groups(r[1])[0]))
            for a in sorted(reads - documented):
                probs.append(f"{doc}: `{cmd}` reads argument `{a}` ({FW}/console.cpp), which its row doesn't list: "
                             "document it")
            for a in sorted(documented - reads):
                probs.append(f"{doc}: `{cmd}` lists argument `{a}`, which {FW}/console.cpp never reads: remove it")
            keys = flatten(reply_tree(repo, blk)) - GENERIC_KEYS
            named = set(ticks(r[2]))
            errors = {s for e in re.findall(r'res\["error"\]\s*=\s*([^;]+);', blk) for s in c_strings(e)}
            for k in sorted(keys - named):
                probs.append(f"{doc}: `{cmd}` replies with `{k}` ({FW}/console.cpp), which its row doesn't name: "
                             "describe it")
            for k in sorted(named - keys - other - reads - errors):
                if not re.fullmatch(r"[0-9a-f]+", k):  # a value, e.g. flash_id `000000`
                    probs.append(f"{doc}: `{cmd}`'s reply column names `{k}`, which isn't in its reply, an argument, "
                                 "an error, an event or a command: remove it or fix the name")
            only = next((role for role in ("house", "gate")
                         if re.search(rf"activeRole\s*!=\s*ROLE_{role.upper()}", blk)), None)
            said = re.search(r"\b(House|Gate) only\b", r[2])
            if (said.group(1).lower() if said else None) != only:
                fix = f", but its row says {said.group(0)!r}" if said else f": say \"{only.capitalize()} only\""
                probs.append(f"{doc}: `{cmd}` is " + (f"{only} only" if only else "for both roles")
                             + f" in {FW}/console.cpp{fix}")
    return probs


def check_held_commands(repo):
    """The commands that wait while a relay pulses (console.cpp blocksLoop()), as docs/console.md and CLAUDE.md
    list them."""
    body = func_body(repo.fw("console.cpp"), "blocksLoop")
    if body is None:
        return [f"{FW}/console.cpp: blocksLoop() not found"]
    held = set(re.findall(r'strcmp\(cmd,\s*"([^"]+)"\)', body))
    probs = []
    for doc, pat in (("docs/console.md", r"waits for its interlock start\) (.+?) wait in the port's"),
                     ("CLAUDE.md", r"the console holds (\S+) and the gate queues")):
        m = re.search(flex(pat), repo.text(doc), re.S)
        if not m:
            probs.append(f"{doc}: the list of commands held during a relay pulse (/{pat}/) wasn't found: update "
                         "check_held_commands in tools/docgen.py")
        elif set(ticks(m.group(1))) != held:
            probs.append(f"{doc}: lists {sorted(ticks(m.group(1)))} as held while a relay pulses; console.cpp "
                         f"blocksLoop() holds {sorted(held)}")
    return probs


def check_framing(repo):
    m = re.search(r"//\s*Frame:\s*(.+)", repo.fw("link.h"))
    if not m:
        return [f"{FW}/link.h: the `// Frame: ...` comment wasn't found"]
    want = [re.sub(r"\(.*?\)", "", f).strip() for f in m.group(1).split("|")]
    probs = []
    for doc, pat in (("docs/protocol.md", r"Frame: `([^`]+)`"), ("CLAUDE.md", r"authenticated framing: `([^`]+)`")):
        found = re.search(pat, repo.text(doc))
        have = [f.strip() for f in found.group(1).split("|")] if found else None
        if have != want:
            probs.append(f"{doc}: the frame layout reads {have}, link.h has {want}: make it `{' | '.join(want)}`")
    return probs


def check_radio_settings(repo):
    """docs/protocol.md names the settings that must match on both boards and the radio ones nobody writes over
    LoRa; CLAUDE.md says radio params and the input inverts are never remote-writable."""
    params, must = parse_params(repo), set(repo.js["MUST_MATCH"])
    radio = {p.name for p in params if "RADIO" in p.flags}
    probs = [f"{FW}/config.cpp: `{p.name}` is P_REMOTE, but radio params and the input inverts are never "
             "remote-writable (CLAUDE.md, docs/protocol.md)" for p in params
             if "REMOTE" in p.flags and ("RADIO" in p.flags or re.fullmatch(r"in\d_invert", p.name))]
    doc = "docs/protocol.md"
    m = re.search(flex(r"Radio settings \(([^)]*)\) must match on both boards \(([^)]*) may differ\)"), repo.text(doc))
    if not m:
        return probs + [f"{doc}: the sentence \"Radio settings (...) must match on both boards (... may differ)\" "
                        "wasn't found"]
    if set(ticks(m.group(1))) != radio & must:
        probs.append(f"{doc}: lists {ticks(m.group(1))} as radio settings that must match; the firmware's radio params "
                     f"in MUST_MATCH are {sorted(radio & must)}")
    if set(ticks(m.group(2))) != radio - must:
        probs.append(f"{doc}: says {ticks(m.group(2))} may differ; the radio params outside MUST_MATCH are "
                     f"{sorted(radio - must)}")
    return probs


def check_e2e_profile(repo):
    """tests/e2e/README.md: the settings the suite pins "at the firmware defaults" are, and so is REAL_CTRL_POWER."""
    src, doc = "tests/e2e/gatelink/bench.py", "tests/e2e/README.md"
    profiles = {}
    for node in ast.parse(repo.text(src)).body:
        if isinstance(node, ast.Assign) and isinstance(node.targets[0], ast.Name) and isinstance(node.value, ast.Dict):
            profiles[node.targets[0].id] = ast.literal_eval(node.value)
    text = repo.text(doc)
    m = re.search(r"at the firmware defaults \(", text)
    if not m or "PROFILE_COMMON" not in profiles:
        return [f"{doc}: \"at the firmware defaults (...)\" or {src} PROFILE_COMMON wasn't found"]
    group = text[m.end():match_paren(text, m.end() - 1)].split(";")[0]  # not "; see `PROFILE_*` in ..."
    pinned = set(ticks(group))
    if "input inverts" in group:
        pinned |= {k for p in profiles.values() for k in p if re.fullmatch(r"in\d_invert", k)}
    names = {p.name for p in parse_params(repo)}
    probs = [f"{src}: {prof} sets `{n}`, which isn't a setting in {FW}/config.cpp PARAMS: fix the name"
             for prof, p in profiles.items() if prof.startswith("PROFILE_") or prof == "REAL_CTRL_POWER"
             for n in p if n not in names]
    for n in sorted(pinned):
        if n not in names:
            probs.append(f"{doc}: `{n}` (pinned at the firmware defaults) isn't a setting")
            continue
        for prof in ("PROFILE_COMMON", "PROFILE_HOUSE", "PROFILE_GATE", "REAL_CTRL_POWER"):
            if n in profiles.get(prof, {}) and profiles[prof][n] != default_of(repo, n):
                probs.append(f"{src}: {prof} sets {n} = {profiles[prof][n]}, but {doc} says the suite pins it at the "
                             f"firmware default, {default_of(repo, n)}")
        if not any(n in p for p in profiles.values()):
            probs.append(f"{doc}: says the suite pins `{n}` at its default, but no PROFILE_* in {src} sets it")
    for n, v in profiles.get("REAL_CTRL_POWER", {}).items():
        if n in names and v != default_of(repo, n):
            probs.append(f"{src}: REAL_CTRL_POWER (\"firmware defaults\") sets {n} = {v}; the default is "
                         f"{default_of(repo, n)}")
    return probs


# ---------------------------------------------------------------------------------------------------------------
# Facts: constants and defaults the docs quote

WORDS = {"a": 1, "an": 1, "one": 1, "two": 2, "three": 3, "four": 4, "five": 5, "six": 6, "seven": 7, "eight": 8,
         "nine": 9, "ten": 10, "eleven": 11, "twelve": 12}


@dataclass
class Quote:
    """Where a doc quotes a fact: the first group of `pattern` (every match in `file`) is the quoted value. `conv`
    turns the source value into the doc's unit (ms -> s); `approx` compares at the quote's precision (~12.4 days)."""
    file: str
    pattern: str
    conv: object = None
    approx: bool = False


@dataclass
class Fact:
    """A value in the source: the first group of `pattern` in `file` (None: any firmware file), as a C constant
    expression or else a string; or `value(repo)` when it has to be worked out. `guard` patterns must still match
    in `file` for the worked-out value to hold. `where` names the source in a problem (default: `file`)."""
    name: str
    file: str = None
    pattern: str = None
    quotes: tuple = ()
    value: object = None
    guard: tuple = ()
    where: str = None


def Q(file, pattern, conv=None, approx=False):
    return Quote(file, pattern, conv, approx)


def D(name, file, *quotes):
    """A #define's value."""
    return Fact(name, file, rf"#define\s+{name}\s+([^\n]*?)\s*(?://|$)", quotes)


def S(name, *quotes):
    """A setting's default (configDefaults)."""
    return Fact(f"default {name}", value=lambda repo, n=name: default_of(repo, n), quotes=quotes,
                where=f"configDefaults() in {FW}/config.cpp")


def remote_writable(repo, name):
    p = next((p for p in parse_params(repo) if p.name == name), None)
    if p is None:
        raise DocgenError(f"`{name}` isn't a setting in {FW}/config.cpp PARAMS any more: fix the docs that name it "
                          "(and its entry in FACTS, tools/docgen.py)")
    return "REMOTE" in p.flags


def RW(name, *quotes):
    """A setting the docs say the house can write on the gate (P_REMOTE in PARAMS); each quote's `said` phrase."""
    return Fact(f"{name} remote-writable", value=lambda repo, n=name: remote_writable(repo, n), quotes=quotes,
                where=f"PARAMS in {FW}/config.cpp (P_REMOTE)")


def said(phrase):
    """A quote's conv for a yes/no fact: the doc's phrase while it holds."""
    return lambda v: phrase if v else "(no longer true: reword it)"


def s(v):
    return v / 1000


def onoff(v):
    return "on" if v else "off"


def retry_schedule(repo):
    """When a command's resends go at the default cmd_ttl_s and retries, in s: link.cpp retryDelay() halves the gap
    back from the TTL, gap n = ttl >> (retries - n) (airtime and jitter aside)."""
    ttl, retries = default_of(repo, "cmd_ttl_s") * 1000, default_of(repo, "retries")
    at, out = 0, []
    for n in range(retries):
        at += ttl >> (retries - n)
        out.append(at / 1000)
    return out


def hello_base_max(repo):
    src = repo.fw("link.cpp")
    cap = re.search(r"HELLO_INTERVAL_MS << \(helloRound < (\d+) \?", src)
    if not cap:
        raise DocgenError(f"{FW}/link.cpp: the HELLO back-off (HELLO_INTERVAL_MS << ...) wasn't found")
    return repo.names()["HELLO_INTERVAL_MS"] << int(cap.group(1))


def history_days(repo):
    return repo.names()["HIST_DEPTH"] * repo.names()["HIST_PERIOD_S"] / 86400


C, CON, PRO, HW, BT, DEV = ("CLAUDE.md", "docs/console.md", "docs/protocol.md", "docs/hardware.md",
                            "docs/bench-testing.md", "docs/development.md")
R, E2E, SET = "README.md", "tests/e2e/README.md", "web/js/settings.js"
CI, REL, BENCH = ".github/workflows/ci.yml", ".github/workflows/release.yml", "tests/e2e/gatelink/bench.py"
LIBS = (("LoRa", "`LoRa` (sandeepmistry)"), ("Crypto", "`Crypto` (rweather)"),
        ("FlashStorage", "`FlashStorage` (cmaglie)"), ("ArduinoJson", "`ArduinoJson`"),
        ("Adafruit SleepyDog Library", "`Adafruit SleepyDog Library`"))
NUM = r"(\d+(?:\.\d+)?)"


def bit(v):
    return v.bit_length() - 1


FACTS = [
    # Gate, relays and inputs
    D("INTERLOCK_MS", f"{FW}/role_gate.cpp", Q(C, r"`INTERLOCK_MS` \((\d+) ms\)")),
    D("BOOT_SETTLE_MS", f"{FW}/role_gate.cpp",
      Q(C, r"`BOOT_SETTLE_MS` \((\d+) s, at most", s),
      Q(CON, r"steady for (\d+) s, at most", s),
      Q(HW, r"\((\d+) s out of `no_power`\)", s)),
    D("BOOT_SETTLE_MAX_MS", f"{FW}/role_gate.cpp",
      Q(C, r"`BOOT_SETTLE_MS` \(\d+ s, at most (\d+) s", s),
      Q(CON, r"steady for \d+ s, at most (\d+) s", s)),
    D("BETWEEN_HOLD_MS", f"{FW}/role_gate.cpp", Q(HW, rf"a move into `between` is reported only after {NUM} s", s)),
    D("STATUS_TTL_MS", f"{FW}/role_gate.cpp",
      Q(PRO, r"STATUS: `heartbeat_s` capped at (\d+) s", s),
      Q(C, r"STATUS's TTL is `heartbeat_s` capped at (\d+) s", s)),
    Fact("watchdog", None, r"Watchdog\.enable\((\d+)\)",
         (Q(C, r"Hardware watchdog is (\d+) s", s),
          Q(CON, r"come to the (\d+) s watchdog", s),
          Q(R, r"the firmware froze for (\d+) s", s))),
    Fact("LED breathing period", None, r"ledBump\(now % (\d+), 0,", (Q(R, rf"Dim breathing \({NUM} s\)", s),)),
    Fact("supply poll", f"{FW}/supply.cpp", r"POLL_MS = (\d+);", (Q(C, r"polled every (\d+) ms before the roles"),)),
    # Link
    D("TURNAROUND_MS", f"{FW}/link.cpp",
      Q(PRO, r"after a (\d+) ms turnaround"),
      Q(C, r"after the (\d+) ms turnaround"),
      Q(C, r"the peer answers (\d+) ms after our frame")),
    D("CHALLENGE_LIFE_MS", f"{FW}/link.cpp", Q(PRO, r"A challenge is good for (\d+) s", s)),
    D("HELLO_ANSWER_GAP_MS", f"{FW}/link.cpp",
      Q(PRO, r"answers at most one HELLO (\w+) second", s),
      Q(C, r"answers at most one (\w+) second from the verified session", s)),
    Fact("HELLO interval cap", f"{FW}/link.cpp", value=hello_base_max,
         quotes=(Q(C, r"the base doubling to (\d+) s", s),)),
    D("TXQ_LEN", f"{FW}/link.cpp", Q(PRO, r"queued (\w+) deep"), Q(C, r"wait in a (\d+)-deep queue")),
    D("TAG_LEN", f"{FW}/link.cpp", Q(PRO, r"truncated to (\d+) bytes"), Q(C, r"truncated to (\d+) bytes")),
    D("RX_AGE_CAP_MS", f"{FW}/link.cpp", Q(CON, rf"tops out at ~{NUM} days", lambda v: v / 86400000, approx=True)),
    Fact("LBT cap (longest frames)", f"{FW}/link.cpp", r"\((\d+) \* radioAirtimeMs\(MAX_FRAME\)\)",
         (Q(C, r"after (\d+)× the longest frame"),)),
    Fact("TX deadline margin", f"{FW}/radio.cpp", r"radioAirtimeMs\(len\) \+ (\d+);",
         (Q(C, r"\(airtime \+ (\d+) ms\)"),)),
    Fact("link timeout heartbeats (x1000)", f"{FW}/role_house.cpp", r"gateHeartbeat \* (\d+);",
         (Q(PRO, rf"max\(`link_timeout_s`, {NUM} × the gate's heartbeat\)", s),
          Q(C, rf"max\(`link_timeout_s`, {NUM} × that heartbeat\)", s),
          Q(BT, rf"at least {NUM} × the gate's `heartbeat_s`", s),
          Q(R, rf"at least {NUM} gate heartbeats", s),
          Q(SET, rf"at least {NUM} heartbeats", s),
          Q(SET, rf"never shorter than {NUM} gate heartbeats", s))),
    Fact("CFG_SET TTL", f"{FW}/role_house.cpp", r"MSG_CFG_SET, p, \d+, (\d+)\)",
         (Q(PRO, r"config writes: (\d+) s\)", s),)),
    Fact("command retry schedule", f"{FW}/link.cpp", value=retry_schedule, guard=(r"s\.ttl >> \(cfg\.retries - n\)",),
         quotes=(Q(PRO, r"retries about ((?:[\d.]+,\s*)+[\d.]+\s+and\s+[\d.]+) s;", approx=True),)),
    Fact("replay window (frames)", f"{FW}/link.cpp", r"static uint(\d+)_t peerWindow;",
         (Q(PRO, r"A (\d+)-frame sliding window"), Q(C, r"then a (\d+)-frame sliding window"))),
    Fact("boot counter slot", f"{FW}/config.cpp", r"uint8_t b\[(\d+)\], check\[\d+\];",
         (Q(PRO, r"one (\d+)-byte slot per boot"),)),
    Fact("starting seq (bits)", f"{FW}/link.cpp", r"txSeq = radioRandom32\(\) & (0x[0-9A-Fa-f]+);",
         (Q(C, r"seq starts at a random (\d+)-bit value", lambda v: v.bit_length()),)),
    D("NODE_HOUSE", f"{FW}/config.h", Q(PRO, r"\(house = (\d+), gate = \d+\)")),
    D("NODE_GATE", f"{FW}/config.h", Q(PRO, r"\(house = \d+, gate = (\d+)\)")),
    Fact("key bytes", f"{FW}/config.h", r"uint8_t key\[(\d+)\];",
         (Q(PRO, r"\(shared\s+(\d+)-bit key\)", lambda v: v * 8),
          Q(CON, r"`key`: (\d+) hex chars", lambda v: v * 2))),
    # STATUS
    D("ST_LEN", f"{FW}/roles.h", Q(PRO, r"STATUS is (\d+) bytes: it ends with")),
    D("ST_LEN_V2", f"{FW}/roles.h",
      Q(PRO, r"An older gate's (\d+)-byte STATUS"),
      Q(PRO, r"Since [\d.]+, STATUS is (\d+) bytes: the gate also")),
    D("ST_LEN_V1", f"{FW}/roles.h", Q(PRO, r"the (\d+)-byte STATUS of")),
    Fact("STATUS travel_timeout_s since", f"{FW}/roles.h", r"#define ST_LEN_V2 .*added in ([\d.]+)",
         (Q(PRO, r"Since ([\d.]+), STATUS is \d+ bytes: it ends"),
          Q(CON, r"from gate firmware ([\d.]+) also its\s+`travel_timeout_s`"),
          Q(C, r"carried in STATUS since ([\d.]+);"))),
    Fact("STATUS counters since", f"{FW}/roles.h", r"#define ST_LEN_V1 .*added in ([\d.]+)",
         (Q(PRO, r"Since ([\d.]+), STATUS is \d+ bytes: the gate also"),
          Q(CON, r"from gate firmware ([\d.]+) also its `retries`"))),
    Fact("STATUS v1 firmware", f"{FW}/roles.h", r"#define ST_LEN_V1 .*up to ([\d.x]+)",
         (Q(PRO, r"STATUS of ([\d.x]+) gates"),)),
    Fact("STI_AC_LOST since", f"{FW}/roles.h", r"#define STI_AC_LOST\s+\S+\s*//\s*([\d.]+)",
         (Q(PRO, r"Since ([\d.]+), bit \d+ of the STATUS inputs byte"),
          Q(CON, r"`ac_power` \(from gate firmware ([\d.]+);"))),
    D("STI_AC_LOST", f"{FW}/roles.h",
      Q(PRO, r"bit (\d+) of the STATUS inputs byte", bit),
      Q(C, r"STATUS bit (\d+) = AC lost", bit)),
    D("STI_IN3", f"{FW}/roles.h", Q(C, r"reported in STATUS bits (\d+)–\d+", bit)),
    D("STI_IN4", f"{FW}/roles.h", Q(C, r"reported in STATUS bits \d+–(\d+)", bit)),
    Fact("RES_OK", f"{FW}/link.h", r"RES_OK = (\d+)", (Q(CON, r"ACK result: (\d+) ok,"),)),
    Fact("RES_ALREADY", f"{FW}/link.h", r"RES_ALREADY = (\d+)", (Q(CON, r"ACK result: \d+ ok, (\d+) already"),)),
    Fact("RES_BAD", f"{FW}/link.h", r"RES_BAD = (\d+)", (Q(CON, r"ACK result: [^;]*?(\d+) rejected"),)),
    Fact("RES_NO_POWER", f"{FW}/link.h", r"RES_NO_POWER = (\d+)", (Q(CON, r"ACK result: [^;]*?(\d+) no AC power"),)),
    # Console
    Fact("USB baud", f"{FW}/console.cpp", r"Serial\.begin\((\d+)\)", (Q(CON, r"over USB serial \((\d+) baud\)"),)),
    D("UART_BAUD", f"{FW}/console.cpp",
      Q(CON, r"(\d[\d,]*) baud, 8N1"),
      Q(CON, r"Even at (\d+) kbaud", s),
      Q(C, r"on Serial1 at (\d+) kbaud", s),
      Q(SET, r"3\.3 V, (\d+) kbaud", s)),
    D("LINE_MAX", f"{FW}/console.cpp", Q(CON, r"one over (\d+) characters", lambda v: v - 1)),
    D("USB_TX_TIMEOUT_MS", f"{FW}/console.cpp",
      Q(CON, r"untaken for more than (\d+) ms"),
      Q(C, r"dropped the rest of the line after (\d+) ms")),
    Fact("USB packet", f"{FW}/console.cpp", r"uint8_t buf\[(\d+)\];",
         (Q(CON, r"one (\d+)-byte packet at a time"), Q(C, r"written in (\d+)-byte pieces"))),
    D("LOG_SIZE", f"{FW}/log.h", Q(CON, r"the ring buffer \((\d+) entries")),
    Fact("config.set chunk (web)", "web/js/config.js", r"const CFG_CHUNK = (\d+);",
         (Q(CON, r"Send at most ~(\d+) params per request"),)),
    Fact("config.set chunk (Python client)", "tools/gatelink_client/board.py", r"CONFIG_CHUNK = (\d+)",
         (Q(CON, r"Send at most ~(\d+) params per request"),)),
    Fact("relay.test ms min", f"{FW}/console.cpp", r"ms < (\d+) \|\| ms > \d+",
         (Q(CON, r"`ms`: (\d+)–\d+ \(default"),)),
    Fact("relay.test ms max", f"{FW}/console.cpp", r"ms < \d+ \|\| ms > (\d+)",
         (Q(CON, r"`ms`: \d+–(\d+) \(default"), Q(CON, r"(\d+) s for a `relay\.test`", s))),
    Fact("relay.test ms default", f"{FW}/console.cpp", r'req\["ms"\]\.isNull\(\) \? (\d+)',
         (Q(CON, r"`ms`: \d+–\d+ \(default (\d+)\)"),)),
    Fact("identify default", f"{FW}/console.cpp", r'uint32_t ms = req\["ms"\] \| (\d+);\s*appIdentify',
         (Q(CON, r"`identify` \| `ms` \(default (\d+),"),
          Q(R, r"\*\*Identify\*\* strobes the connected board's LED for (\d+) s", s),
          Q(R, r"Fast bright strobe \((\d+) s\)", s),
          Q("web/app.js", r"LED strobing for (\d+) s", s),
          Q("web/js/serial.js", r"LED for (\d+) s'", s))),
    Fact("identify max", f"{FW}/console.cpp", r"appIdentify\(ms > (\d+)",
         (Q(CON, r"`identify` \| `ms` \(default \d+, max (\d+)\)"),)),
    Fact("debug.mute max", f"{FW}/console.cpp", r"linkDebugMute\(ms > (\d+)",
         (Q(CON, r"`debug\.mute` \| `ms` \(max (\d+);"),)),
    # Link history
    D("HIST_DEPTH", f"{FW}/history.h",
      Q(CON, r"the last (\d+) plus the one in progress"),
      Q(C, r"hourly buckets \((\d+) \+ the one in progress")),
    Fact("history span (days)", f"{FW}/history.h", value=history_days,
         quotes=(Q(CON, r"plus the one in progress\s+\((\w+) days\)"), Q(R, r"up to (\w+) days since its"))),
    D("HIST_PERIOD_S", f"{FW}/history.h",
      Q(CON, r"by default (\w+) hour each", lambda v: v / 3600),
      Q(CON, r"lasts until the next boot \(then (\d+)\)")),
    D("HIST_PAGE", f"{FW}/history.h", Q(CON, r"`n` \(1–(\d+), default"), Q(CON, r"`n` \(1–\d+, default (\d+)\)")),
    Fact("hist.clear period min", f"{FW}/history.cpp", r"periodS < (\d+) \|\| periodS > \d+",
         (Q(CON, r"`period_s` \((\d+)–\d+, default"),)),
    Fact("hist.clear period max", f"{FW}/history.cpp", r"periodS < \d+ \|\| periodS > (\d+)",
         (Q(CON, r"`period_s` \(\d+–(\d+), default"),)),
    D("NOISE_EVERY_MS", f"{FW}/history.cpp", Q(CON, r"read (\d+)× a second", lambda v: 1000 / v)),
    # Config storage and firmware image
    D("EXTFLASH_PAGE", f"{FW}/extflash.h", Q(C, r"fit one (\d+)-byte page")),
    D("REC_MAX_PARAMS", f"{FW}/config.cpp", Q(C, r"static_assert, (\d+) params")),
    Fact("FW_MARKER_PREFIX", f"{FW}/config.h", r'#define FW_MARKER_PREFIX "([^"]+)"',
         (Q(C, r"`(\w+=)x\.y\.z`"), Q(DEV, r"`(\w+=)x\.y\.z`"))),
    # Web console
    Fact("RECONNECT_MS", "web/js/serial.js", r"const RECONNECT_MS = (\d+);",
         (Q(C, r"reopen the already-granted port for (\d+) s", s),
          Q(R, r"reconnects to it automatically for (\d+) s", s))),
    Fact("BOOT_PID", "web/js/samba.js", r"BOOT_PID = (0x[0-9a-fA-F]+);",
         (Q(C, r"bootloader PID (0x[0-9a-fA-F]+)"), Q(DEV, r"USB PID (0x[0-9a-fA-F]+)"))),
    Fact("APP_START", "web/js/samba.js", r"APP_START = (0x[0-9a-fA-F]+);",
         (Q(C, r"`Z` at (0x[0-9a-fA-F]+)"), Q(DEV, r"erase from (0x[0-9a-fA-F]+)"))),
    # Tools
    Fact("rftest seconds", "tools/gatelink.py", r'"--seconds", type=float, default=(\d+)',
         (Q(BT, r"\((\d+) minutes of pings both ways", lambda v: v / 60),)),
    Fact("rftest max loss", "tools/gatelink.py", r'"--max-loss", type=float, default=([\d.]+)',
         (Q(BT, rf"at most {NUM} % of pongs lost"),)),
    # Setting defaults quoted in the docs ("`setting` (default X)" quotes are found by check_default_quotes)
    S("freq_hz",
      Q(PRO, rf"^{NUM} MHz, \d+ kHz bandwidth", lambda v: v / 1e6),
      Q(R, r"LoRa (\d+) MHz, HMAC", lambda v: v / 1e6)),
    S("bw_hz", Q(PRO, r"MHz, (\d+) kHz bandwidth", s)),
    S("sf", Q(PRO, r"kHz bandwidth, SF(\d+)")),
    S("cr", Q(PRO, r"SF\d+, CR 4/(\d)")),
    Fact("cr min", f"{FW}/config.cpp", r'"cr", &Config::cr, (\d+),',
         (Q(SET, r"Extra error correction, 4/(\d) to"),)),
    Fact("cr max", f"{FW}/config.cpp", r'"cr", &Config::cr, \d+, (\d+),',
         (Q(SET, r"Extra error correction, 4/\d to 4/(\d)"),)),
    S("tx_power", Q(PRO, r"CR 4/\d, (\d+) dBm"), Q(BT, r"`tx_power` (\d+); `power_sense`")),
    S("power_sense", Q(BT, r"`power_sense`, `ctrl_power_sense` and `ctrl_power_pmic` (on|off)", onoff)),
    S("ctrl_power_sense",
      Q(BT, r"`power_sense`, `ctrl_power_sense` and `ctrl_power_pmic` (on|off)", onoff),
      Q(C, r"both default (on|off): while either is off", onoff)),
    S("ctrl_power_pmic",
      Q(BT, r"`power_sense`, `ctrl_power_sense` and `ctrl_power_pmic` (on|off)", onoff),
      Q(C, r"both default (on|off): while either is off", onoff)),
    S("ctrl_confirm_ms",
      Q(BT, r"`ctrl_confirm_ms` (\d+)\)"),
      Q(BT, rf"only `ctrl_confirm_ms` \({NUM} s, OFF", s),
      Q(R, rf"waits `ctrl_confirm_ms` \({NUM} s\)", s),
      Q(SET, rf"so {NUM} s is plenty", s)),
    S("pulse_ms",
      Q(HW, r"only ever pulsed \(default (\d+) ms\)"),
      Q(R, r"\*\*only ever pulsed\*\* \(default (\d+) ms\)")),
    S("uart_console", Q(CON, r"it is (on|off) by default", onoff)),
    S("cmd_ttl_s", Q(PRO, r"`cmd_ttl_s` for commands: at (\d+) s and")),
    S("retries", Q(PRO, r"`cmd_ttl_s` for commands: at \d+ s and (\d+) retries")),
    # Settings the docs say the house can write on the gate (P_REMOTE)
    RW("power_sense", Q(HW, r"`power_sense` toggle off \((also possible remotely) over LoRa\)",
                        said("also possible remotely"))),
    RW("travel_timeout_s",
       Q(PRO, r"\((only the gate's copy can be changed remotely),", said("only the gate's copy can be changed remotely")),
       Q(SET, r"Set it on the gate \((here or with a remote write)\)", said("here or with a remote write"))),
    # The e2e suite's profile (tests/e2e/gatelink/bench.py)
    *(Fact(f"e2e profile {n}", BENCH, rf'{prof} = \{{[^}}]*"{n}": (\d+)', (Q(E2E, rf"`{n}` (\d+)[,)]"),))
      for prof, n in (("PROFILE_COMMON", "heartbeat_s"), ("PROFILE_COMMON", "link_timeout_s"),
                      ("PROFILE_COMMON", "travel_timeout_s"), ("PROFILE_HOUSE", "mismatch_timeout_s"),
                      ("PROFILE_COMMON", "cmd_ttl_s"))),
    Fact("e2e profile ctrl_confirm_ms", BENCH, r'PROFILE_HOUSE = \{[^}]*"ctrl_confirm_ms": (\d+)',
         (Q(E2E, r"`ctrl_confirm_ms` (\d+), as IN2 alone needs"),)),
    Fact("e2e simulator travel", BENCH, r"SIM_TRAVEL_S = (\d+)", (Q(E2E, r"simulator travel (\d+) s"),)),
    Fact("soak loop_max_us limit", "tests/e2e/test_soak.py", r"LOOP_MAX_US = ([\d_]+)",
         (Q(C, r"the soak fails it past (\d+) s", lambda v: v / 1e6),)),
    # Toolchain versions: CI is the source; the docs and the release workflow must say the same
    Fact("arduino:samd core", CI, r"arduino:samd@([\d.]+)",
         (Q(DEV, r"\| `arduino:samd` core \| ([\d.]+) \|"),
          Q(REL, r"arduino:samd@([\d.]+)"),
          Q(R, r"arduino:samd@([\d.]+)"))),
    Fact("arduino:avr core", CI, r"arduino:avr@([\d.]+)",
         (Q(DEV, r"\| `arduino:avr` core \(GateSim\) \| ([\d.]+) \|"),)),
    *(Fact(f"library {lib}", CI, rf'"{re.escape(lib)}@([\d.]+)"',
           (Q(DEV, rf"\| {re.escape(label)} \| ([\d.]+) \|"),
            Q(REL, rf'"{re.escape(lib)}@([\d.]+)"'),
            Q(R, rf'"{re.escape(lib)}@([\d.]+)"')))
      for lib, label in LIBS),
    Fact("ArduinoJson major", CI, r'"ArduinoJson@(\d+)\.', (Q(C, r"`ArduinoJson` v(\d+)"),)),
    Fact("ruff", CI, r"ruff==([\d.]+)", (Q(DEV, r"ruff==([\d.]+)"),)),
    Fact("Node.js", CI, r"node-version: '(\d+)'", (Q(DEV, r"need Node\.js (\d+)"),)),
]


def parse_quoted(text):
    t = text.strip().lower().replace(",", "")
    if t in WORDS:
        return WORDS[t]
    return float(int(t, 16)) if t.startswith("0x") else float(t)


def _fmt(v):
    if isinstance(v, list):
        return ", ".join(_fmt(x) for x in v)
    return f"{v:g}" if isinstance(v, float) else str(v)


def _agrees(expected, quoted, approx):
    if isinstance(expected, str):
        return " ".join(quoted.split()) == expected  # a quote may be wrapped over lines
    if isinstance(expected, list):
        nums = re.findall(r"\d+(?:\.\d+)?", quoted)
        return len(nums) == len(expected) and all(_agrees(e, n, approx) for e, n in zip(expected, nums, strict=True))
    try:
        q = parse_quoted(quoted)
    except ValueError:
        return False
    if approx:
        places = len(quoted.split(".")[1]) if "." in quoted else 0
        return round(expected, places) == q
    return abs(expected - q) < 1e-9


def fact_value(repo, f):
    if f.value:
        for g in f.guard:
            if not re.search(g, repo.text(f.file)):
                raise DocgenError(f"{f.file} no longer matches /{g}/, which {f.name} is worked out from: update FACTS "
                                  "in tools/docgen.py")
        return f.value(repo)
    files = [f.file] if f.file else repo.fw_files()
    for rel in files:
        m = re.search(f.pattern, repo.text(rel), re.M)
        if m:
            try:
                return c_eval(m.group(1), repo.names())
            except (ValueError, SyntaxError, TypeError):
                pass
            try:
                return float(m.group(1))
            except ValueError:
                return m.group(1)  # a version, a name
    raise DocgenError(f"{f.file or FW + '/'}: /{f.pattern}/ doesn't match, so {f.name} can't be read: the source "
                      "changed; update its pattern in FACTS (tools/docgen.py)")


def check_facts(repo, facts=None):
    probs = []
    for f in FACTS if facts is None else facts:
        try:
            v = fact_value(repo, f)
        except DocgenError as e:
            probs.append(str(e))
            continue
        except PARSE_ERRORS as e:  # one fact that can't be read mustn't stop the others being checked
            probs.append(f"{f.name}: {type(e).__name__}: {e} (the source moved: update FACTS in tools/docgen.py)")
            continue
        where = f.where or f.file or f"{FW}/"
        for q in f.quotes:
            expected = q.conv(v) if q.conv else v
            text = repo.text(q.file)
            matches = list(re.finditer(flex(q.pattern), text, re.M))
            if not matches:
                probs.append(f"{q.file}: no quote of {f.name} matching /{q.pattern}/ (the doc was reworded, or the "
                             "quote removed): update the quote in FACTS (tools/docgen.py); "
                             f"{where} gives {_fmt(expected)}")
            for m in matches:
                if not _agrees(expected, m.group(1), q.approx):
                    probs.append(f"{q.file}:{line_of(text, m.start())}: quotes {f.name} as {m.group(1)!r} "
                                 f"({m.group(0).strip()!r}), but {where} gives {_fmt(expected)}: change the doc to "
                                 f"{_fmt(expected)}, or fix the source")
    return probs


_VALUE = r"(?P<val>\d+(?:\.\d+)?|on|off)(?:\s?(?P<unit>ms|s|min|dBm|MHz|kHz)\b)?"
_SETTING = r"`(?P<name>[a-z][a-z0-9_]*)`"
# The ways the docs quote a setting's default: "`x` (default 0.5 s)", "`x`, default on", "`x` (default: 3)",
# "`x`, off by default", "`x` is 60 s by default", "`x` defaults to 90 s", "the default `x` (45 s)", "the default
# `x` of 80 ms".
_DEFAULT_QUOTES = [re.compile(p) for p in (
    rf"{_SETTING}[,(\s]{{1,3}}default:?\s{_VALUE}",
    rf"{_SETTING}[,(\s]{{1,3}}(?:is\s|=\s?)?{_VALUE}\sby\sdefault",
    rf"{_SETTING}\sdefaults\sto\s{_VALUE}",
    rf"\bdefault\s{_SETTING}[,(\s]{{1,3}}(?:of\s|is\s|=\s?)?{_VALUE}",
)]


def check_default_quotes(repo):
    """Every quote of a setting's default in the docs, in any of the _DEFAULT_QUOTES phrasings."""
    params = {p.name: p for p in parse_params(repo)}
    defaults = parse_defaults(repo)
    probs = []
    for rel in sorted({r for g in DOC_GLOBS for r in repo.glob(g)}):
        text = repo.text(rel)
        for m in sorted((m for pat in _DEFAULT_QUOTES for m in pat.finditer(text)), key=lambda m: m.start()):
            p = params.get(m["name"])
            if not p:
                continue
            v = defaults.get(p.field, (0, "0"))[0]
            quoted, unit = m["val"], m["unit"]
            if quoted in ("on", "off"):
                ok, want = (v != 0) == (quoted == "on"), onoff(v)
            else:
                scale = {"s": 1000 if p.name.endswith("_ms") else 1, "ms": 0.001 if p.name.endswith("_s") else 1,
                         "min": 60000 if p.name.endswith("_ms") else 60, "MHz": 1e6, "kHz": 1e3}.get(unit, 1)
                want = f"{v / scale:g}" + (f" {unit}" if unit else "")
                ok = abs(float(quoted) * scale - v) < 1e-9
            if not ok:
                probs.append(f"{rel}:{line_of(text, m.start())}: {m.group(0)!r} quotes the default of `{p.name}`, but "
                             f"configDefaults() sets {v}: change it to {want}")
    return probs


_MACRO = r"`(?P<name>[A-Z][A-Z0-9]*(?:_[A-Z0-9]+)*_(?P<suffix>MS|S|US))`"
_TIME = r"~?(?P<val>\d+(?:\.\d+)?)\s?(?P<unit>µs|us|ms|s|min)\b"
# "`INTERLOCK_MS` (100 ms)", and "100 ms (`INTERLOCK_MS`)" or "100 ms apart (`INTERLOCK_MS`)".
_MACRO_QUOTES = [re.compile(rf"{_MACRO}\s\({_TIME}"), re.compile(rf"{_TIME}[^.;()`\n]{{0,20}}\({_MACRO}\)")]
_IN_US = {"µs": 1, "us": 1, "ms": 1e3, "s": 1e6, "min": 6e7}


def check_macro_quotes(repo):
    """Every time a doc quotes a firmware time constant next to its name ("`INTERLOCK_MS` (100 ms)"), FACTS or
    not. A name #defined to different values in different files is skipped (it isn't one constant)."""
    values = {}
    for rel in repo.fw_files():
        for k, (v, _) in defines(repo.text(rel)).items():
            try:
                values.setdefault(k, set()).add(c_eval(v, repo.names()))
            except (ValueError, SyntaxError, TypeError, ZeroDivisionError):
                values.setdefault(k, set()).add(None)
    probs = []
    for rel in sorted({r for g in DOC_GLOBS for r in repo.glob(g)}):
        text = repo.text(rel)
        for m in sorted((m for pat in _MACRO_QUOTES for m in pat.finditer(text)), key=lambda m: m.start()):
            vals = values.get(m["name"], set())
            if len(vals) != 1 or None in vals:
                continue
            v = next(iter(vals)) * {"US": 1, "MS": 1e3, "S": 1e6}[m["suffix"]]  # in µs
            quoted = float(m["val"]) * _IN_US[m["unit"]]
            if abs(quoted - v) > 1e-6 * max(v, 1):
                probs.append(f"{rel}:{line_of(text, m.start())}: {m.group(0)!r} quotes {m['name']}, which is "
                             f"{_fmt(v / _IN_US[m['unit']])} {m['unit']} in {FW}/: change the doc, or fix the source")
    return probs


# snake_case names the docs may use that aren't a setting, field, argument, event or test (none yet).
OTHER_NAMES = set()


def known_names(repo):
    """Every snake_case name the source gives the docs to use: settings, status/event/history fields, command
    arguments and reply keys, log events, the names an enumerated field takes, e2e tests and release criteria."""
    names = {p.name for p in parse_params(repo)} | OTHER_NAMES
    for _, tree in status_trees(repo).values():
        names |= flatten(tree)
    for event, (_, tree) in console_events(repo).items():
        names |= flatten(tree) | {event}
    names |= set(c_array(repo.fw("history.cpp"), "FIELDS")) | set(c_array(repo.fw("log.cpp"), "NAMES"))
    for blk in command_blocks(repo).values():
        names |= set(re.findall(r'req\["(\w+)"\]', blk)) | flatten(reply_tree(repo, blk))
    for values in enum_values(repo).values():
        names |= values
    for rel in repo.glob("tests/e2e/**/*.py"):
        names.add(Path(rel).stem)
        names |= set(re.findall(r"^\s*def (test_\w+)", repo.text(rel), re.M))
    for rel in repo.glob("tools/release_evidence.py"):
        names |= set(re.findall(r'\bCriterion\(\s*"(\w+)"', repo.text(rel)))
    return names


def check_names(repo):
    """A backticked snake_case name in a doc (`ctrl_confirm_ms`, `link_up`, `test_power_loss_at_rest`) must still
    exist: a renamed setting, field or test otherwise lives on in the prose."""
    known, probs = known_names(repo), []
    for rel in sorted({r for g in DOC_GLOBS for r in repo.glob(g)}):
        text = repo.text(rel)
        for m in re.finditer(r"`([a-z][a-z0-9]*(?:_[a-z0-9]+)+)`", text):
            if m.group(1) not in known:
                probs.append(f"{rel}:{line_of(text, m.start())}: `{m.group(1)}` names nothing in the source (a "
                             "setting, field, argument, log event, e2e test or release criterion): fix the name, or "
                             "add it to OTHER_NAMES in tools/docgen.py")
    return probs


CHECKS = [check_status, check_events, check_history, check_logs, check_pins, check_commands, check_held_commands,
          check_framing, check_radio_settings, check_e2e_profile, check_facts, check_default_quotes,
          check_macro_quotes, check_names]


PARSE_ERRORS = (DocgenError, KeyError, ValueError, StopIteration, AttributeError, TypeError, IndexError, SyntaxError)


def run(repo, write=False):
    """Every problem found (after regenerating the generated sections, with write), each once."""
    probs = []
    for doc, name, gen, sources in SECTIONS:
        try:
            body, found = gen(repo)
            probs += found + sync(repo, doc, name, block(name, body, sources), write)
        except PARSE_ERRORS as e:
            probs.append(f"{doc} `{name}` section: {type(e).__name__}: {e} (the source or the doc moved: update "
                         "tools/docgen.py)")
    for check in CHECKS:
        try:
            probs += check(repo)
        except PARSE_ERRORS as e:
            probs.append(f"{check.__name__}: {type(e).__name__}: {e} (the source or a doc moved: update "
                         "tools/docgen.py)")
    return list(dict.fromkeys(probs))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--write", action="store_true", help="regenerate the generated sections in place, then check")
    args = ap.parse_args(argv)
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")  # a Windows console can't print every character the docs use
    probs = run(Repo(), write=args.write)
    if probs:
        print(f"Docs out of step with the source ({len(probs)}):\n" + "\n".join(f"- {p}" for p in probs))
        return 1
    print(f"Docs OK: {len(SECTIONS)} generated sections {'written' if args.write else 'current'}, "
          f"{len(CHECKS)} checks and {sum(len(f.quotes) for f in FACTS)} quoted facts agree with the source.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
