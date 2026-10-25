---------------------------- MODULE GateLink ----------------------------
(***************************************************************************)
(* The link between the two boards, with an attacker on the air: both     *)
(* boards' sessions, sequence numbers, replay windows, commands and their  *)
(* deadlines, STATUS reports, the contact sensor, reboots and the gate     *)
(* moved by others. Checked with TLC (tools/verify/Makefile).              *)
(*                                                                         *)
(* Each action cites the firmware it stands for. The boards' own step      *)
(* properties (relays, interlock, a command's deadline check, the sensor   *)
(* from the last STATUS taken) are proved on the C++ by the ESBMC          *)
(* harnesses (tools/verify/cbmc); this model takes them as given and       *)
(* checks what only the two boards and the air together decide.           *)
(*                                                                         *)
(* The attacker is Dolev-Yao without the key: every frame either board     *)
(* sends is recorded (`air`) and may be delivered to the other board any   *)
(* number of times, at any later time, in any order, or never. It can't    *)
(* forge a frame (HMAC-SHA256, link.cpp computeTag) nor answer a 128-bit   *)
(* challenge, so a handshake only ever verifies a board's current session  *)
(* (link.cpp MSG_HELLO / MSG_HELLO_ACK).                                    *)
(*                                                                         *)
(* Time is whole units (`now`); the clocks are exact (no crystal drift: see *)
(* docs/architecture.md "Verification" for the 100 ppm the house allows).   *)
(***************************************************************************)
EXTENDS Integers, FiniteSets, TLC

CONSTANTS
  L,            \* house link timeout (houseLinkTimeoutMs)
  TTL,          \* house cmd_ttl_s: a command's deadline is the gate's clock + TTL (role_house.cpp sendCommand)
  SLACK,        \* gate: CMD_LATE_SLACK_MS + two frames' airtime (role_gate.cpp handleCmd)
  AHEAD,        \* gate: CMD_AHEAD_MAX_MS
  STALE,        \* house, FixStatusAge: a STATUS whose clock is further behind the house's reckoning isn't taken
  LAG,       \* SensorTruth: how long after the gate leaves closed the sensor may still show closed
  MISSED,       \* MISSED_OPEN_MS: a missed move opens the sensor this long
  MaxTime, MaxSeq, MaxBoots, MaxCmds,
  FixCmdOrder,  \* gate refuses a command framed before the last one it took (this house session)
  FixStatusAge, \* house refuses a STATUS older than STALE by its reckoning of the gate's clock
  FirstFresh    \* assumption: the first STATUS a house takes in a gate session is at most STALE old

\* A boot's clock starts far from every other boot's (role_gate.cpp clockOrigin: 47 random bits).
Epoch == MaxTime + AHEAD + TTL + SLACK + STALE + 10
None == [sess |-> 0, val |-> 0, at |-> 0]
NoSlot == [active |-> FALSE, id |-> 0, act |-> "open", dl |-> 0, exp |-> 0]
Other(p) == IF p = "closed" THEN "open" ELSE "closed"

VARIABLES
  now,
  pos, openSince,             \* the gate as it is (its limits are taken as truthful), and when it last left closed
  gBoot, gSeq, gPeer, gBase, gAcc, gHaveCmd, gLastId, gLastSeq, gLeft,
  hBoot, hSeq, hNextId, hSlot, hPeer, hBase, hAcc,
  hState, hStatusSess, hStatusSeq, hStatusAt, hClk, hGateLeft, hLeftWhen, hSensor, hMissedUntil,
  air,                        \* every frame ever sent: the attacker's recording
  cmds,                       \* ghost: when each command was given, and its action
  gNewest,                    \* ghost: the latest given command the gate has taken this house session (-1: none)
  pulsed,                     \* ghost: commands that have pulsed a relay (any gate boot)
  badOrder, badLate, badTwice \* ghost: a property broken by the last step

gateVars == <<gBoot, gSeq, gPeer, gBase, gAcc, gHaveCmd, gLastId, gLastSeq, gLeft>>
houseVars == <<hBoot, hSeq, hNextId, hSlot, hPeer, hBase, hAcc,
               hState, hStatusSess, hStatusSeq, hStatusAt, hClk, hGateLeft, hLeftWhen, hSensor, hMissedUntil>>
ghostVars == <<cmds, gNewest, pulsed, badOrder, badLate, badTwice>>
vars == <<now, pos, openSince, gateVars, houseVars, air, ghostVars>>

GClock(boot, t) == boot * Epoch + t                       \* role_gate.cpp gateClock
Est == hClk.val + (now - hClk.at)                          \* role_house.cpp gateClockNow
LinkUp(at, t) == at >= 0 /\ t - at < L                     \* app.cpp appLinkUp (house: houseStatusAt)
\* role_house.cpp applyOutputs, with linkloss_open on and sensor_invert off: K2 closed.
K2(st, at, mu, t) == st = "closed" /\ LinkUp(at, t) /\ t >= mu
SensorClosed == K2(hState, hStatusAt, hMissedUntil, now)

Init ==
  /\ now = 0
  /\ pos \in {"closed", "open"}
  /\ openSince = 0
  /\ gBoot = 1 /\ gSeq = 0 /\ gPeer = 0 /\ gBase = 0 /\ gAcc = {} /\ gHaveCmd = FALSE /\ gLastId = 0
  /\ gLastSeq = 0 /\ gLeft = 0
  /\ hBoot = 1 /\ hSeq = 0 /\ hNextId = 1 /\ hSlot = NoSlot /\ hPeer = 0 /\ hBase = 0 /\ hAcc = {}
  /\ hState = "unknown" /\ hStatusSess = 0 /\ hStatusSeq = 0 /\ hStatusAt = -1 /\ hClk = None
  /\ hGateLeft = 0 /\ hLeftWhen = 0 /\ hSensor = FALSE /\ hMissedUntil = 0
  /\ air = {}
  /\ cmds = [i \in {} |-> 0] /\ gNewest = -1 /\ pulsed = {}
  /\ badOrder = FALSE /\ badLate = FALSE /\ badTwice = FALSE

\* The house's outputs each loop pass (applyOutputs): the sensor as decided, and the gate's departure count noted when
\* it goes closed (leftWhenClosed).
Outputs(st, at, mu, t, left) ==
  LET k2 == K2(st, at, mu, t) IN
  /\ hSensor' = k2
  /\ hLeftWhen' = IF k2 /\ ~hSensor THEN left ELSE hLeftWhen

Tick ==
  /\ now < MaxTime
  /\ now' = now + 1
  /\ Outputs(hState, hStatusAt, hMissedUntil, now + 1, hGateLeft)
  /\ UNCHANGED <<pos, openSince, gateVars, hBoot, hSeq, hNextId, hSlot, hPeer, hBase, hAcc, hState, hStatusSess,
                 hStatusSeq, hStatusAt, hClk, hGateLeft, hMissedUntil, air, ghostVars>>

\* Anyone else moves the gate: its local button, the AES controller, the siren (role_gate.cpp: cause external).
\* Leaving closed counts a departure (leftClosed, ST_LEFT_CLOSED).
EnvMove ==
  /\ pos' = Other(pos)
  /\ openSince' = IF pos = "closed" THEN now ELSE openSince
  /\ gLeft' = IF pos = "closed" THEN gLeft + 1 ELSE gLeft
  /\ UNCHANGED <<now, gBoot, gSeq, gPeer, gBase, gAcc, gHaveCmd, gLastId, gLastSeq, houseVars, air, ghostVars>>

\* The gate reports (role_gate.cpp sendStatus: heartbeat, a state change, a new house session). Whenever: safety can't
\* depend on how often. Its resends are the same frame, already recorded.
GateStatus ==
  /\ gSeq < MaxSeq
  /\ gSeq' = gSeq + 1
  /\ air' = air \cup {[t |-> "ST", s |-> gBoot, q |-> gSeq + 1, st |-> pos, c |-> GClock(gBoot, now), left |-> gLeft]}
  /\ UNCHANGED <<now, pos, openSince, gBoot, gPeer, gBase, gAcc, gHaveCmd, gLastId, gLastSeq, gLeft, houseVars,
                 ghostVars>>

\* The owner switches (role_house.cpp sendCommand), dated in the gate's clock as the house reckons it: only with a
\* reading from the gate's verified session (it waits otherwise).
UserCmd(a) ==
  /\ hNextId <= MaxCmds /\ hSeq < MaxSeq
  /\ hClk.sess # 0 /\ hClk.sess = hPeer
  /\ LET id == hNextId
         dl == Est + TTL IN
     /\ hSlot' = [active |-> TRUE, id |-> id, act |-> a, dl |-> dl, exp |-> now + TTL]
     /\ air' = air \cup {[t |-> "CMD", s |-> hBoot, q |-> hSeq + 1, id |-> id, act |-> a, dl |-> dl]}
     /\ cmds' = cmds @@ (id :> [at |-> now, act |-> a])
  /\ hNextId' = hNextId + 1 /\ hSeq' = hSeq + 1
  /\ UNCHANGED <<now, pos, openSince, gateVars, hBoot, hPeer, hBase, hAcc, hState, hStatusSess, hStatusSeq, hStatusAt,
                 hClk, hGateLeft, hLeftWhen, hSensor, hMissedUntil, gNewest, pulsed, badOrder, badLate, badTwice>>

\* A pending command renumbered (link.cpp reframePending: whenever the house answers a HELLO, replayed ones too), within
\* its TTL. Plain resends are the same frame.
HouseRenumber ==
  /\ hSlot.active /\ now < hSlot.exp /\ hSeq < MaxSeq
  /\ hSeq' = hSeq + 1
  /\ air' = air \cup {[t |-> "CMD", s |-> hBoot, q |-> hSeq + 1, id |-> hSlot.id, act |-> hSlot.act, dl |-> hSlot.dl]}
  /\ UNCHANGED <<now, pos, openSince, gateVars, hBoot, hNextId, hSlot, hPeer, hBase, hAcc, hState, hStatusSess,
                 hStatusSeq, hStatusAt, hClk, hGateLeft, hLeftWhen, hSensor, hMissedUntil, ghostVars>>

\* The slot ends: ACKed, or given up at its TTL (link.cpp linkPoll).
HouseSlotEnd ==
  /\ hSlot.active
  /\ hSlot' = NoSlot
  /\ UNCHANGED <<now, pos, openSince, gateVars, hBoot, hSeq, hNextId, hPeer, hBase, hAcc, hState, hStatusSess,
                 hStatusSeq, hStatusAt, hClk, hGateLeft, hLeftWhen, hSensor, hMissedUntil, air, ghostVars>>

\* A recorded command reaches the gate. The link takes it if it is from the verified house session, after the
\* handshake, and new to the window (link.cpp acceptSeq: within 32 of the newest, any order); then handleCmd.
DeliverCmd(f) ==
  /\ f.t = "CMD" /\ gPeer # 0 /\ f.s = gPeer /\ f.q > gBase /\ f.q \notin gAcc
  /\ gAcc' = gAcc \cup {f.q}
  /\ LET late == GClock(gBoot, now) - f.dl
         dup == gHaveCmd /\ f.id = gLastId
         superseded == FixCmdOrder /\ gHaveCmd /\ f.q < gLastSeq
         take == ~dup /\ ~superseded /\ late <= SLACK /\ late >= -AHEAD
         target == IF f.act = "open" THEN "open" ELSE "closed"
         pulse == take /\ pos # target IN
     /\ gHaveCmd' = (gHaveCmd \/ take)
     /\ gLastId' = IF take THEN f.id ELSE gLastId
     /\ gLastSeq' = IF take THEN f.q ELSE gLastSeq
     /\ pos' = IF pulse THEN target ELSE pos
     /\ openSince' = IF pulse /\ pos = "closed" THEN now ELSE openSince
     /\ gLeft' = IF pulse /\ pos = "closed" THEN gLeft + 1 ELSE gLeft
     /\ gNewest' = IF take /\ cmds[f.id].at > gNewest THEN cmds[f.id].at ELSE gNewest
     /\ pulsed' = IF pulse THEN pulsed \cup {f.id} ELSE pulsed
     \* The properties, judged on what the owner did, not on the firmware's own bookkeeping:
     /\ badOrder' = (badOrder \/ (pulse /\ cmds[f.id].at < gNewest))     \* an older command after a newer one
     /\ badLate' = (badLate \/ (pulse /\ now - cmds[f.id].at > TTL + SLACK))  \* past the owner's TTL
     /\ badTwice' = (badTwice \/ (pulse /\ f.id \in pulsed))              \* one press, two pulses
  /\ UNCHANGED <<now, gBoot, gSeq, gPeer, gBase, houseVars, air, cmds>>

\* A recorded STATUS reaches the house. The link takes it as above; then handleStatus: only a STATUS later than the last
\* one taken in this gate session (0.13.9), and with FixStatusAge only one no older than STALE by the house's reckoning.
DeliverStatus(f) ==
  /\ f.t = "ST" /\ hPeer # 0 /\ f.s = hPeer /\ f.q > hBase /\ f.q \notin hAcc
  /\ FirstFresh /\ hClk.sess # f.s => GClock(f.s, now) - f.c <= STALE
  /\ hAcc' = hAcc \cup {f.q}
  /\ LET old == hStatusSess = f.s /\ f.q <= hStatusSeq
         stale == FixStatusAge /\ hClk.sess = f.s /\ f.c < Est - STALE
         same == hStatusSess = f.s
         missed == same /\ f.left # hLeftWhen /\ f.st = "closed" /\ hSensor
         mu == IF missed THEN now + MISSED ELSE hMissedUntil
         lw == IF same THEN hLeftWhen ELSE f.left IN
     IF old \/ stale
     THEN UNCHANGED <<hState, hStatusSess, hStatusSeq, hStatusAt, hClk, hGateLeft, hLeftWhen, hSensor, hMissedUntil>>
     ELSE /\ hState' = f.st
          /\ hStatusSess' = f.s /\ hStatusSeq' = f.q /\ hStatusAt' = now
          /\ hClk' = IF hClk.sess # f.s \/ f.c >= Est THEN [sess |-> f.s, val |-> f.c, at |-> now] ELSE hClk
          /\ hGateLeft' = f.left
          /\ hMissedUntil' = mu
          /\ LET k2 == K2(f.st, now, mu, now) IN
             /\ hSensor' = k2
             /\ hLeftWhen' = IF k2 /\ ~hSensor THEN f.left ELSE lw
  /\ UNCHANGED <<now, pos, openSince, gateVars, hBoot, hSeq, hNextId, hSlot, hPeer, hBase, air, ghostVars>>

\* The house verifies the gate's current session (its HELLO answered by a HELLO_ACK: link.cpp). Nothing at or before
\* the HELLO_ACK is accepted after it (peerLastSeq, a full window).
HouseVerify ==
  /\ hPeer # gBoot /\ gSeq < MaxSeq
  /\ hPeer' = gBoot /\ hBase' = gSeq + 1 /\ gSeq' = gSeq + 1 /\ hAcc' = {}
  /\ UNCHANGED <<now, pos, openSince, gBoot, gPeer, gBase, gAcc, gHaveCmd, gLastId, gLastSeq, gLeft, hBoot, hSeq,
                 hNextId, hSlot, hState, hStatusSess, hStatusSeq, hStatusAt, hClk, hGateLeft, hLeftWhen, hSensor,
                 hMissedUntil, air, ghostVars>>

\* The gate verifies the house's current session; a new one forgets the last command (role_gate.cpp gateLoop).
GateVerify ==
  /\ gPeer # hBoot /\ hSeq < MaxSeq
  /\ gPeer' = hBoot /\ gBase' = hSeq + 1 /\ hSeq' = hSeq + 1 /\ gAcc' = {}
  /\ gHaveCmd' = FALSE /\ gNewest' = -1
  /\ UNCHANGED <<now, pos, openSince, gBoot, gSeq, gLastId, gLastSeq, gLeft, hBoot, hNextId, hSlot, hPeer, hBase, hAcc,
                 hState, hStatusSess, hStatusSeq, hStatusAt, hClk, hGateLeft, hLeftWhen, hSensor, hMissedUntil, air,
                 cmds, pulsed, badOrder, badLate, badTwice>>

\* A reboot (power cut, watchdog): a new session (never one used before under the key: link.cpp drawSession), RAM gone.
\* The gate doesn't move for it (nothing is pulsed at boot).
GateReboot ==
  /\ gBoot < MaxBoots
  /\ gBoot' = gBoot + 1 /\ gPeer' = 0 /\ gBase' = 0 /\ gAcc' = {} /\ gHaveCmd' = FALSE /\ gLastId' = 0
  /\ gLastSeq' = 0 /\ gLeft' = 0 /\ gNewest' = -1
  /\ UNCHANGED <<now, pos, openSince, gSeq, houseVars, air, cmds, pulsed, badOrder, badLate, badTwice>>

HouseReboot ==
  /\ hBoot < MaxBoots
  /\ hBoot' = hBoot + 1 /\ hPeer' = 0 /\ hBase' = 0 /\ hAcc' = {} /\ hSlot' = NoSlot
  /\ hState' = "unknown" /\ hStatusSess' = 0 /\ hStatusSeq' = 0 /\ hStatusAt' = -1 /\ hClk' = None
  /\ hGateLeft' = 0 /\ hLeftWhen' = 0 /\ hSensor' = FALSE /\ hMissedUntil' = 0
  /\ UNCHANGED <<now, pos, openSince, gateVars, hSeq, hNextId, air, ghostVars>>

Next ==
  \/ Tick \/ EnvMove \/ GateStatus \/ HouseRenumber \/ HouseSlotEnd \/ HouseVerify \/ GateVerify
  \/ GateReboot \/ HouseReboot
  \/ \E a \in {"open", "close"} : UserCmd(a)
  \/ \E f \in air : DeliverCmd(f)
  \/ \E f \in air : DeliverStatus(f)

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
(* The properties.                                                         *)

\* Truth: the contact sensor shows closed only while the gate is closed, or for at most LAG after it left closed.
SensorTruth == SensorClosed => (pos = "closed" \/ now - openSince <= LAG)
\* A command never pulses after a later one has been taken: the owner's last word stands.
CmdOrder == ~badOrder
\* A command never pulses later than its TTL (plus the gate's slack) after the owner gave it.
CmdFresh == ~badLate
\* One command, at most one pulse, across gate reboots too.
CmdOnce == ~badTwice

\* The state as the command properties see it (VIEW CmdView in their .cfg files): the gate's departures and the contact
\* sensor's bookkeeping, and what a STATUS says of the gate, never decide whether or when a command pulses, so states
\* that differ only there are one state to TLC. The rest of the house's STATUS handling (which one it takes, its
\* reckoning of the gate's clock, which dates commands) stays in.
CmdView == <<now, pos, gBoot, gSeq, gPeer, gBase, gAcc, gHaveCmd, gLastId, gLastSeq,
             hBoot, hSeq, hNextId, hSlot, hPeer, hBase, hAcc, hStatusSess, hStatusSeq, hClk,
             {IF f.t = "ST" THEN [t |-> "ST", s |-> f.s, q |-> f.q, c |-> f.c] ELSE f : f \in air}, ghostVars>>

TypeOK ==
  /\ pos \in {"closed", "open"}
  /\ hState \in {"unknown", "closed", "open"}
=============================================================================
