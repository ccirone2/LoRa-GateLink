# Formal verification of the firmware (docs/architecture.md, "Verification"). Needs ESBMC, Java and TLC's
# tla2tools.jar (versions in docs/development.md; tools/verify/docker/Dockerfile has them all):
#   make -C tools/verify                 # everything: the ESBMC proofs on the C++ and the TLA+ protocol model
#   make -C tools/verify esbmc | tla     # one half
#   make -C tools/verify FW=<dir>        # against another copy of the firmware (tools/mutate.py --verify)
# On Windows, in Docker:
#   docker build -t gatelink-verify tools/verify/docker
#   MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W):/src" -w /src gatelink-verify make -C tools/verify -j4
ROOT := $(abspath ../..)
FW ?= $(ROOT)/firmware/GateLink
ESBMC ?= esbmc
TLA2TOOLS ?= /opt/tla2tools.jar
PYTHON ?= python3
BUILD ?= build
# Each ESBMC run's limit, seconds (a gate segment takes ~40 minutes on a 4-core machine; mutate.py --verify passes more).
ESBMC_TIMEOUT ?= 3000
RUN := $(PYTHON) esbmc_run.py --esbmc $(ESBMC) --fw $(FW) --build $(BUILD) --timeout $(ESBMC_TIMEOUT)

# The ESBMC harnesses (esbmc/*.cpp): each is an inductive proof (the step), its base case (BOOT) and a reachability
# check (REACH, which must fail: a harness that excludes every run proves nothing). link_window has no boot case (its
# invariant is vacuous until a session is verified), nor has house_cmds (a bounded run from boot, not an induction).
HARNESSES := gate house link_window house_cmds
# gate.cpp's inductive step, one query per segment of a pass (see its main).
GATE_SEGMENTS := 1 2 3 4 5
HOUSE_SEGMENTS := 1 2 3
BOOTED := gate house
# The TLA+ model's configurations (tla/*.cfg): each says what it expects (tlc_trace.py).
MODELS := $(wildcard tla/*.cfg)

.PHONY: all esbmc tla clean $(HARNESSES:%=esbmc-%) esbmc-house_cmds-install esbmc-house_cmds-toggle \
        esbmc-house_cmds-stalls esbmc-house_cmds-worst $(GATE_SEGMENTS:%=esbmc-gate-%) \
        $(HOUSE_SEGMENTS:%=esbmc-house-%) $(BOOTED:%=esbmc-boot-%) $(HARNESSES:%=esbmc-reach-%) \
        $(MODELS:tla/%.cfg=tla-%)
all: esbmc tla
esbmc: $(HARNESSES:%=esbmc-%) $(BOOTED:%=esbmc-boot-%) $(HARNESSES:%=esbmc-reach-%)
tla: $(MODELS:tla/%.cfg=tla-%)

$(BUILD)/params.h: gen_params.py $(FW)/config.cpp
	@mkdir -p $(BUILD)
	$(PYTHON) gen_params.py --fw $(FW) $@

# link_window.cpp one frame length per query (LEN_MIN..LEN_MAX; those under a header and tag in one): a few seconds
# each, where all lengths in one query didn't finish in 2 hours.
LINK_LENGTHS := 0-20 $(shell seq 21 37)
esbmc-link_window: $(BUILD)/params.h
	@for s in $(LINK_LENGTHS); do $(RUN) -D LEN_MIN=$${s%-*} -D LEN_MAX=$${s#*-} esbmc/link_window.cpp || exit 1; done
# house_cmds.cpp in the install's setup (the Shelly following K1, the bench's timings, the house polling its power
# through a dip), then in the variants that each show what a property depends on. A variant expected to fail is a
# finding still open or a dependency the docs name (architecture.md, "Verification").
INSTALL := -D SHELLY_MODE=0 -D BROWNOUT_AFTER_PG
esbmc-house_cmds: esbmc-house_cmds-install esbmc-house_cmds-toggle esbmc-house_cmds-stalls esbmc-house_cmds-worst
esbmc-house_cmds-install: $(BUILD)/params.h
	@$(RUN) $(INSTALL) esbmc/house_cmds.cpp
# Any Shelly mode, toggling on SW edges included (finding 3, fixed in 0.20.5).
esbmc-house_cmds-toggle: $(BUILD)/params.h
	@$(RUN) -D BROWNOUT_AFTER_PG esbmc/house_cmds.cpp
# A loop stall (a flash save) anywhere, over a dip too (finding 4, TODO.md).
esbmc-house_cmds-stalls: $(BUILD)/params.h
	@$(RUN) $(INSTALL) -D STALLS_ANYWHERE --expect fail --expect-msg owner-only esbmc/house_cmds.cpp
# A dip may reboot the Shelly without dropping the board's power good (unmeasured: the bench saw it drop first): the
# relay's drop and the Shelly's boot back to its last state are then edges the house takes for the owner's, whatever
# ctrl_confirm_ms (finding 5, TODO.md). The install's cover is the order (BROWNOUT_AFTER_PG), a measurement.
esbmc-house_cmds-worst: $(BUILD)/params.h
	@$(RUN) -D SHELLY_MODE=0 --expect fail --expect-msg owner-only esbmc/house_cmds.cpp
esbmc-gate: $(GATE_SEGMENTS:%=esbmc-gate-%)
esbmc-house: $(HOUSE_SEGMENTS:%=esbmc-house-%)
$(HOUSE_SEGMENTS:%=esbmc-house-%): esbmc-house-%: $(BUILD)/params.h
	@$(RUN) -D SEGMENT=$* esbmc/house.cpp
$(GATE_SEGMENTS:%=esbmc-gate-%): esbmc-gate-%: $(BUILD)/params.h
	@$(RUN) -D SEGMENT=$* esbmc/gate.cpp
$(BOOTED:%=esbmc-boot-%): esbmc-boot-%: $(BUILD)/params.h
	@$(RUN) -D BOOT esbmc/$*.cpp
esbmc-reach-link_window: $(BUILD)/params.h
	@for s in $(LINK_LENGTHS); do \
	  $(RUN) -D REACH -D LEN_MIN=$${s%-*} -D LEN_MAX=$${s#*-} --expect fail --expect-msg reach: esbmc/link_window.cpp \
	  || exit 1; done
esbmc-reach-house_cmds: $(BUILD)/params.h
	@$(RUN) $(INSTALL) -D REACH --expect fail --expect-msg reach: esbmc/house_cmds.cpp
esbmc-reach-gate: $(GATE_SEGMENTS:%=esbmc-reach-gate-%)
esbmc-reach-house: $(HOUSE_SEGMENTS:%=esbmc-reach-house-%)
$(HOUSE_SEGMENTS:%=esbmc-reach-house-%): esbmc-reach-house-%: $(BUILD)/params.h
	@$(RUN) -D SEGMENT=$* -D REACH --expect fail --expect-msg reach: esbmc/house.cpp
$(GATE_SEGMENTS:%=esbmc-reach-gate-%): esbmc-reach-gate-%: $(BUILD)/params.h
	@$(RUN) -D SEGMENT=$* -D REACH --expect fail --expect-msg reach: esbmc/gate.cpp

$(MODELS:tla/%.cfg=tla-%): tla-%:
	@$(PYTHON) tlc_trace.py --jar $(TLA2TOOLS) tla/$*.cfg

clean:
	rm -rf $(BUILD)
