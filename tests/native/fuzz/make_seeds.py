"""Writes the seed corpora in fuzz/corpus/<target>/ (README.md: Seeds).

    python tests/native/fuzz/make_seeds.py [--images DIR]

The frames and console seeds are built here from the targets' input formats (the comments at the top of
fuzz_frames.cpp and fuzz_console.cpp). The config seeds start from the flash images the firmware itself provisions:
run any target with GATELINK_FUZZ_DUMP_IMAGES=DIR first (it writes DIR/house.bin and DIR/gate.bin), then pass
--images DIR. Without it the config corpus is left as it is.

Seeds are small, readable starting points; a fuzzing run minimises what it finds into the corpus later
(make -f fuzz/Makefile corpus-merge).
"""

import argparse
import struct
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
CORPUS = HERE / "corpus"

# fuzz_frames record ops
(OP_AUTH, OP_RAW, OP_TIME, OP_PINS, OP_CONSOLE, OP_ACK, OP_RESTART, OP_ANSWER, OP_REPLAY, OP_CRCERR,
 OP_RADIO) = range(11)
# link.h MsgType
HELLO, HELLO_ACK, ACK, CMD, STATUS, PING, PONG, DIAG_REQ, DIAG, CFG_SET = range(1, 11)
# roles.h
GS_UNKNOWN, GS_CLOSED, GS_OPEN, GS_BETWEEN, GS_FAULT, GS_NO_POWER = range(6)
CAUSE_NONE, CAUSE_LORA, CAUSE_EXTERNAL = range(3)
TR_NONE, TR_REACHED, TR_TIMEOUT, TR_ALREADY = range(4)
STI_IN1, STI_IN2, STI_K1, STI_K2, STI_IN3, STI_IN4, STI_AC_LOST = 1, 2, 4, 8, 16, 32, 64
OPEN, CLOSE = 1, 2
# config.cpp PARAMS: index (console templates) and id (CFG_SET, records)
PARAM_INDEX = {name: i for i, name in enumerate([
    "role", "net_id", "freq_hz", "sf", "bw_hz", "cr", "tx_power", "sync_word", "retries", "heartbeat_s",
    "link_timeout_s", "cmd_ttl_s", "debounce_ms", "in1_invert", "in2_invert", "pulse_ms", "travel_timeout_s",
    "ctrl_sync", "sync_window_ms", "resync_ms", "mismatch_timeout_s", "sensor_invert", "linkloss_open",
    "in3_invert", "in4_invert", "power_sense", "ctrl_power_sense", "ctrl_confirm_ms", "ctrl_settle_ms",
    "ctrl_power_pmic", "uart_console"])}
PARAM_ID = {"retries": 9, "heartbeat_s": 10, "link_timeout_s": 11, "debounce_ms": 13, "pulse_ms": 16,
            "travel_timeout_s": 17, "power_sense": 26, "sf": 4, "in1_invert": 14}
# Console templates (fuzz_frames.cpp consoleTemplate)
T_RAW, T_RELAY_TEST, T_REMOTE_SET, T_REMOTE_DIAG, T_PING, T_CONFIG_SET, T_CONFIG_SAVE, T_MUTE, T_REPLAY, \
    T_REBOOT_AFTER_CMD, T_STATUS, T_CONFIG_GET, T_LOG_GET, T_HIST_GET, T_HIST_CLEAR, T_IDENTIFY, T_KEY_SET, \
    T_CONFIG_RESET, T_REBOOT, T_INFO = range(20)


# --- fuzz_frames ---------------------------------------------------------------------------------------------------
def head(gate, cold=False, wrap=False, pins=0):
    return bytes([(1 if gate else 0) | (2 if cold else 0) | (4 if wrap else 0) | (pins << 3)])


def auth(mtype, payload=b"", flags=0, delta=0, seq=0, session=0, rssi=-60, snr=9.0):
    b = bytes([OP_AUTH, mtype, flags])
    if flags & 3 == 2:
        b += struct.pack("<b", delta)
    if flags & 3 == 3:
        b += struct.pack("<I", seq)
    if flags & 4:
        b += struct.pack("<I", session)
    if flags & 0x20:
        b += bytes([rssi + 157, int(snr * 4) & 0xFF])
    return b + bytes([len(payload)]) + payload


def raw(data, flags=0):
    return bytes([OP_RAW, flags, len(data)]) + data


def wait(ms):
    out = b""
    while ms > 0:
        out += bytes([OP_TIME]) + struct.pack("<H", min(ms, 65535))
        ms -= 65535
    return out


def pins(in1=0, in2=0, in3=0, in4=0, supply_lost=0):
    return bytes([OP_PINS, in1 | in2 << 1 | in3 << 2 | in4 << 3 | supply_lost << 4])


def console(template, args=b"", param="role", uart=False, crc=False):
    return bytes([OP_CONSOLE, template << 2 | (2 if crc else 0) | (1 if uart else 0), PARAM_INDEX[param]]) + args


def ack(which_type, result=0, offset=None):
    types = [CMD, STATUS, CFG_SET, PING, HELLO, DIAG, ACK, PONG]
    which = types.index(which_type) | (0x80 if offset is not None else 0)
    return bytes([OP_ACK, which]) + (struct.pack("<b", offset) if offset is not None else b"") + bytes([result])


def restart(session=0, seq=0x2000, challenge=0):
    return bytes([OP_RESTART]) + struct.pack("<III", session, seq, challenge)


def answer(previous=False, wait_hello=True):
    return bytes([OP_ANSWER, (1 if previous else 0) | (2 if wait_hello else 0)])


def replay(index, board=False):
    return bytes([OP_REPLAY, (0x80 if board else 0) | index])


def radio(absent=False, jammed=False, noise=None, reset=False):
    flags = (1 if absent else 0) | (2 if jammed else 0) | (4 if noise is not None else 0) | (8 if reset else 0)
    return bytes([OP_RADIO, flags]) + (bytes([noise + 157]) if noise is not None else b"")


def cmd(cmd_id, action):
    return struct.pack("<HB", cmd_id, action)


def cfg_set(param, value):
    return struct.pack("<Bi", PARAM_ID[param], value)


def status(state, inputs, cause=CAUSE_NONE, result=TR_NONE, cmd_id=0, target=GS_UNKNOWN, heartbeat=30, travel=60,
           v1=False):
    p = struct.pack("<BBBBHIhbBH", state, inputs, cause, result, cmd_id, 120, -70, 8, target, heartbeat)
    if v1:
        return p
    return p + struct.pack("<HHHbbH", 1, 0, 2, -112, -104, travel)


def diag(params=(("retries", 5), ("heartbeat_s", 30), ("pulse_ms", 500))):
    p = bytes([0, 13, 6]) + struct.pack("<I", 3600) + struct.pack("<6H", 10, 9, 0, 1, 2, 0)
    for name, value in params:
        p += struct.pack("<Bi", PARAM_ID[name], value)
    return p


AC = STI_IN3
CLOSED_IN = STI_IN2 | AC
BAD_CRC_TEST = b'{"cmd":"relay.test","k":1,"ms":50,"crc":"00000000"}'

FRAMES = {
    # Gate: commands, interlock, duplicates, retransmits, power, external moves
    "gate-cmd-open": head(True) + auth(CMD, cmd(0x1234, OPEN)) + wait(1000) + ack(STATUS),
    "gate-cmd-open-then-close": head(True) + auth(CMD, cmd(1, OPEN)) + wait(200) + auth(CMD, cmd(2, CLOSE))
    + wait(1000),
    "gate-cmd-same-direction-twice": head(True) + auth(CMD, cmd(7, OPEN)) + wait(250) + auth(CMD, cmd(8, OPEN))
    + wait(1000),
    "gate-cmd-dup-and-retransmit": head(True) + auth(CMD, cmd(9, OPEN)) + auth(CMD, cmd(9, OPEN), flags=1)
    + auth(CMD, cmd(9, OPEN)) + wait(800),
    "gate-cmd-bad": head(True) + auth(CMD, b"\x01") + auth(CMD, cmd(3, 7)) + auth(CMD, cmd(4, CLOSE)),
    "gate-cfg-set": head(True) + auth(CFG_SET, cfg_set("pulse_ms", 300)) + auth(CFG_SET, cfg_set("sf", 10))
    + auth(CFG_SET, cfg_set("pulse_ms", 99999)) + ack(STATUS),
    "gate-cfg-set-during-pulse": head(True) + auth(CMD, cmd(5, OPEN)) + auth(CFG_SET, cfg_set("heartbeat_s", 60))
    + auth(CFG_SET, cfg_set("heartbeat_s", 60), flags=1) + wait(1000) + ack(STATUS),
    "gate-diag-ping": head(True) + auth(DIAG_REQ) + auth(PING, struct.pack("<H", 77)) + auth(PONG, b"\x01\x00"),
    "gate-ac-lost": head(True) + pins(in2=1) + wait(2000) + auth(CMD, cmd(11, OPEN)) + pins() + wait(4000)
    + pins(in2=1, in3=1) + wait(4000),
    "gate-external-move": head(True) + pins(in3=1) + wait(800) + pins(in1=1, in3=1) + wait(300) + ack(STATUS)
    + auth(CMD, cmd(12, CLOSE)) + wait(300) + pins(in3=1) + wait(600) + pins(in1=1, in2=1, in3=1) + wait(200),
    "gate-relay-test-and-held-save": head(True) + console(T_RELAY_TEST, b"\x01" + struct.pack("<H", 2000))
    + console(T_CONFIG_SAVE) + wait(2500),
    "gate-reboot-after-cmd": head(True) + console(T_REBOOT_AFTER_CMD) + auth(CMD, cmd(13, OPEN)) + wait(1500),
    "gate-peer-restart": head(True) + auth(CMD, cmd(14, OPEN)) + restart() + answer() + auth(CMD, cmd(1, CLOSE))
    + wait(1500),
    "gate-replays": head(True) + auth(CMD, cmd(15, OPEN)) + wait(700) + replay(0) + replay(1, board=True)
    + auth(STATUS, status(GS_OPEN, 0), flags=2, delta=-5),
    "gate-bad-frames": head(True) + auth(CMD, cmd(16, OPEN), flags=8) + auth(CMD, cmd(16, OPEN), flags=0x10)
    + raw(bytes(range(40)), flags=1) + raw(b"\x01\x04") + auth(ACK, struct.pack("<IB", 1, 0), flags=4,
                                                               session=0xBAD5E55)
    + auth(CMD, bytes(108)),
    "gate-cold-boot-handshake": head(True, cold=True, pins=0b0110) + wait(700) + answer() + wait(4000) + ack(STATUS),
    "gate-wrap-pulse": head(True, wrap=True) + wait(30800) + auth(CMD, cmd(17, OPEN)) + wait(1000)
    + auth(CMD, cmd(18, CLOSE)) + wait(1000),
    "gate-radio-faults": head(True) + radio(jammed=True) + auth(CMD, cmd(19, OPEN)) + wait(800) + radio(reset=True)
    + radio(absent=True, reset=True) + wait(6000) + radio() + wait(6000) + answer(),
    "gate-uart-console": head(True) + console(T_CONFIG_SET, struct.pack("<IB", 1, 0), param="uart_console")
    + console(T_STATUS, uart=True, crc=True) + console(T_STATUS, uart=True),
    # Relay monitor regressions. A relay.test of K1 waits out the interlock after K2, and four refused ones (bad crc)
    # follow before it starts: they mustn't push its allowance out. A relay.test read in the very pass a delayed
    # pulse starts is refused busy (0.13.9; it used to restart that pulse, and the allowance was the longer of the two).
    "gate-relay-test-refused-after-one-that-waits": head(True)
    + console(T_RELAY_TEST, b"\x02" + struct.pack("<H", 100)) + console(T_RELAY_TEST, b"\x01" + struct.pack("<H", 600))
    + console(T_RAW, bytes([len(BAD_CRC_TEST)]) + BAD_CRC_TEST) * 4 + wait(2000),
    "gate-relay-test-as-a-delayed-pulse-starts": head(True)
    + console(T_RELAY_TEST, b"\x02" + struct.pack("<H", 100)) + console(T_RELAY_TEST, b"\x01" + struct.pack("<H", 200))
    + wait(95) + console(T_RELAY_TEST, b"\x01" + struct.pack("<H", 3000)) + wait(5000),
    # House: STATUS handling, user commands, power, link loss, peer restarts
    "house-status-open": head(False) + auth(STATUS, status(GS_OPEN, STI_IN1 | AC, CAUSE_EXTERNAL)) + wait(4000)
    + auth(STATUS, status(GS_BETWEEN, AC, CAUSE_EXTERNAL)) + wait(1000)
    + auth(STATUS, status(GS_CLOSED, CLOSED_IN, CAUSE_EXTERNAL)),
    "house-status-v1-and-short": head(False) + auth(STATUS, status(GS_OPEN, STI_IN1, v1=True)) + auth(STATUS, b"\x01")
    + auth(STATUS, status(GS_FAULT, STI_IN1 | STI_IN2) + b"\x00" * 10),
    "house-status-no-power": head(False) + auth(STATUS, status(GS_NO_POWER, STI_AC_LOST)) + wait(2000)
    + auth(STATUS, status(GS_CLOSED, STI_IN2 | STI_AC_LOST)),
    "house-user-open": head(False) + pins(in1=1, in2=1) + wait(100) + ack(CMD, 0)
    + auth(STATUS, status(GS_BETWEEN, AC, CAUSE_LORA, cmd_id=1, target=GS_OPEN)) + wait(3000)
    + auth(STATUS, status(GS_OPEN, STI_IN1 | AC, CAUSE_LORA, TR_REACHED)) + wait(4000) + pins(in2=1) + wait(700)
    + ack(CMD, 1),
    "house-user-open-refused": head(False) + pins(in1=1, in2=1) + wait(100) + ack(CMD, 4) + wait(4000),
    "house-ctrl-power-loss": head(False) + pins(in1=1, in2=1, supply_lost=1) + wait(300) + pins(in2=0, supply_lost=1)
    + wait(3000) + pins(in1=0, in2=1) + wait(12000),
    "house-remote-diag-set": head(False) + console(T_REMOTE_DIAG) + auth(DIAG, diag())
    + console(T_REMOTE_SET, struct.pack("<i", 700), param="pulse_ms") + wait(200) + ack(CFG_SET, 0)
    + console(T_REMOTE_SET, struct.pack("<i", 1), param="sf") + auth(DIAG, b"\x00\x01"),
    "house-link-loss": head(False) + wait(65535) + wait(50000) + auth(STATUS, status(GS_CLOSED, CLOSED_IN)),
    "house-cmd-held-peer-restart": head(False) + pins(in1=1, in2=1) + wait(100) + restart() + answer()
    + wait(2000),
    "house-cmd-held-replayed-hello": head(False) + pins(in1=1, in2=1) + wait(100)
    + auth(HELLO, struct.pack("<I", 0x1111), flags=4, session=0x0DDBA11) + answer(wait_hello=True) + wait(1000),
    "house-wrap": head(False, wrap=True) + wait(19000) + auth(STATUS, status(GS_OPEN, STI_IN1 | AC, CAUSE_EXTERNAL))
    + wait(4000) + pins(in2=1) + wait(2000),
    "house-crc-noise-mute": head(False) + bytes([OP_CRCERR]) + radio(noise=-90)
    + console(T_MUTE, struct.pack("<H", 500)) + auth(STATUS, status(GS_OPEN, STI_IN1 | AC)) + wait(600)
    + console(T_REPLAY, b"\x01") + console(T_PING) + auth(PONG, struct.pack("<HhB", 1, -80, 4)),
    "house-cold-boot": head(False, cold=True, pins=0b0011) + wait(700) + answer()
    + auth(STATUS, status(GS_OPEN, STI_IN1 | AC)) + wait(15000) + pins(in2=1) + wait(1000),
}


# --- fuzz_console ---------------------------------------------------------------------------------------------------
def with_crc(line):
    return line[:-1] + ',"crc":"%08x"}' % zlib.crc32(line.encode())


def lines(*reqs, crc_too=True):
    out = []
    for r in reqs:
        out.append(r)
        if crc_too:
            out.append(with_crc(r))
    return "\n".join(out) + "\n"


HOUSE, GATE, BLANK, GATE_WRAP = 0, 1, 2, 3
UART, SIGN = 4, 8

CONSOLE = {
    "info": (GATE, lines('{"id":1,"cmd":"info"}')),
    "status": (GATE, lines('{"id":2,"cmd":"status"}')),
    "config-get": (GATE, lines('{"id":3,"cmd":"config.get"}')),
    "config-set": (GATE, lines('{"id":4,"cmd":"config.set","params":{"pulse_ms":700,"debounce_ms":30}}',
                               '{"id":5,"cmd":"config.set","params":{"nope":1,"sf":99,"pulse_ms":"x"}}')),
    "config-set-radio": (HOUSE, lines('{"id":6,"cmd":"config.set","params":{"sf":10,"bw_hz":250000}}')),
    "config-save": (GATE, lines('{"id":7,"cmd":"config.save"}')),
    "config-reset": (GATE, lines('{"id":8,"cmd":"config.reset"}')),
    "key-set": (GATE, lines('{"id":9,"cmd":"key.set","key":"00112233445566778899aabbccddeeff"}',
                            '{"id":10,"cmd":"key.set","key":"xyz"}')),
    "relay-test": (GATE, lines('{"id":11,"cmd":"relay.test","k":1,"ms":600}', '{"id":12,"cmd":"relay.test","k":2}',
                               '{"id":13,"cmd":"relay.test","k":257,"ms":4294967796}')),
    "radio-ping": (GATE, lines('{"id":14,"cmd":"radio.ping"}')),
    "remote-diag": (HOUSE, lines('{"id":15,"cmd":"remote.diag"}')),
    "remote-set": (HOUSE, lines('{"id":16,"cmd":"remote.set","name":"pulse_ms","value":700}',
                                '{"id":17,"cmd":"remote.set","name":"sf","value":10}')),
    "log-get": (GATE, lines('{"id":18,"cmd":"log.get"}')),
    "hist-get": (HOUSE, lines('{"id":19,"cmd":"hist.get","from":0,"n":3}', '{"id":20,"cmd":"hist.get"}')),
    "hist-clear": (GATE, lines('{"id":21,"cmd":"hist.clear","period_s":60}', '{"id":22,"cmd":"hist.clear"}')),
    "reboot": (GATE, lines('{"id":23,"cmd":"reboot"}', crc_too=False)),
    "reboot-crc": (GATE, lines(with_crc('{"id":24,"cmd":"reboot"}'), crc_too=False)),
    "identify": (GATE, lines('{"id":25,"cmd":"identify","ms":100000}')),
    "debug-replay": (GATE, lines('{"id":26,"cmd":"debug.replay"}', '{"id":27,"cmd":"debug.replay","hello":true}')),
    "debug-mute": (HOUSE, lines('{"id":28,"cmd":"debug.mute","ms":1000}', '{"id":29,"cmd":"debug.mute"}')),
    "debug-reboot-after-cmd": (GATE, lines('{"id":30,"cmd":"debug.reboot_after_cmd"}')),
    "held-save-during-pulse": (GATE, lines('{"id":31,"cmd":"relay.test","k":2,"ms":1500}',
                                           '{"id":32,"cmd":"config.save"}', '{"id":33,"cmd":"status"}',
                                           crc_too=False)),
    "uart-with-crc": (GATE | UART, lines(with_crc('{"id":34,"cmd":"status"}'),
                                         with_crc('{"id":35,"cmd":"relay.test","k":1}'), crc_too=False)),
    "uart-without-crc": (GATE | UART, lines('{"id":36,"cmd":"status"}', crc_too=False)),
    "signed-by-harness": (GATE_WRAP | SIGN, lines('{"id":37,"cmd":"relay.test","k":1,"ms":300}',
                                                  '{"id":38,"cmd":"config.save"}', crc_too=False)),
    "bad-lines": (HOUSE, '{"id":39,"cmd":\n{"id":40,"cmd":"nope"}\n' + with_crc('{"id":41,"cmd":"status"}')[:-3]
                  + '0"}\n\r\n{"id":[[[1]]],"cmd":"status"}\n'),
    "line-too-long": (GATE, '{"id":42,"cmd":"status","pad":"' + "x" * 1100 + '"}\n'),
    "blank-board": (BLANK, lines('{"id":43,"cmd":"relay.test","k":1}', '{"id":44,"cmd":"radio.ping"}',
                                 '{"id":45,"cmd":"config.set","params":{"role":2}}', '{"id":46,"cmd":"status"}',
                                 crc_too=False)),
}


# --- fuzz_config ----------------------------------------------------------------------------------------------------
KEY = bytes(range(16))


def record(params, seq=1, key=KEY, key_set=1, fix_crc=True):
    body = key + bytes([key_set]) + b"".join(struct.pack("<Bi", i, v) for i, v in params)
    head_ = b"GLC1" + bytes([1, len(params)]) + struct.pack("<I", seq)
    crc = zlib.crc32(body) if fix_crc else 0x12345678
    return head_ + struct.pack("<I", crc) + body


def parse_record(img):
    n = img[5]
    return [struct.unpack_from("<Bi", img, 31 + 5 * i) for i in range(n)], img[14:30]


def flash(*parts):
    """parts: (offset, bytes); erased elsewhere. Byte 256 (the harness's CRC fix-up switch) stays erased unless
    a part sets it."""
    end = max(off + len(b) for off, b in parts)
    img = bytearray(b"\xff" * end)
    for off, b in parts:
        img[off:off + len(b)] = b
    return bytes(img)


def config_seeds(images):
    gate = images / "gate.bin"
    house = images / "house.bin"
    g, h = gate.read_bytes(), house.read_bytes()
    gp, gkey = parse_record(g)
    hp, _ = parse_record(h)
    no_fix = (256, b"\x00")  # byte 256 even: the record's CRC stays as written
    return {
        "gate-record": g[:256],
        "house-record": h[:256],
        "cut-record": flash((0, g[:100]), no_fix),
        "bad-crc-kept": flash((0, record(gp, fix_crc=False)), no_fix),
        "two-sectors": flash((0, g[:256]), (4096, record(hp, seq=2, key=gkey))),
        "two-sectors-seq-wraps": flash((0, record(gp, seq=0xFFFFFFFF)), (4096, record(hp, seq=0))),
        "unknown-and-out-of-range": flash((0, record(gp + [(200, 5), (99, -1), (16, 1), (4, 13), (5, 300000)]))),
        "duplicate-ids": flash((0, record([(1, 2), (1, 1), (16, 800), (16, 900)]))),
        # Sector 2 only: one in sector 3 would make a 12 KB seed (the fuzzer grows inputs there itself).
        "boot-counter": flash((0, g[:256]), (8192, struct.pack("<4I", 5, 6, 7, 0xFFFFFFFF))),
    }


def write(target, seeds):
    d = CORPUS / target
    d.mkdir(parents=True, exist_ok=True)
    for name, data in seeds.items():
        (d / name).write_bytes(data.encode() if isinstance(data, str) else data)
    print(f"{target}: {len(seeds)} seeds, {sum(len(v) for v in seeds.values())} bytes")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--images", type=Path, help="directory with house.bin and gate.bin (GATELINK_FUZZ_DUMP_IMAGES)")
    args = ap.parse_args()
    write("fuzz_frames", FRAMES)
    write("fuzz_console", {name: bytes([sel]) + text.encode() for name, (sel, text) in CONSOLE.items()})
    if args.images:
        write("fuzz_config", config_seeds(args.images))


if __name__ == "__main__":
    main()
