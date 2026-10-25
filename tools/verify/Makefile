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
RUN := $(PYTHON) esbmc_run.py --esbmc $(ESBMC) --fw $(FW) --build $(BUILD)

# The ESBMC harnesses (esbmc/*.cpp): each is an inductive proof (the step), its base case (BOOT) and a reachability
# check (REACH, which must fail: a harness that excludes every run proves nothing). link_window has no boot case (its
# invariant is vacuous until a session is verified), nor has house_cmds (a bounded run from boot, not an induction).
HARNESSES := gate house link_window house_cmds
BOOTED := gate house
# The TLA+ model's configurations (tla/*.cfg): each says what it expects (tlc_trace.py).
MODELS := $(wildcard tla/*.cfg)

.PHONY: all esbmc tla clean $(HARNESSES:%=esbmc-%) $(BOOTED:%=esbmc-boot-%) $(HARNESSES:%=esbmc-reach-%) \
        $(MODELS:tla/%.cfg=tla-%)
all: esbmc tla
esbmc: $(HARNESSES:%=esbmc-%) $(BOOTED:%=esbmc-boot-%) $(HARNESSES:%=esbmc-reach-%)
tla: $(MODELS:tla/%.cfg=tla-%)

$(BUILD)/params.h: gen_params.py $(FW)/config.cpp
	@mkdir -p $(BUILD)
	$(PYTHON) gen_params.py --fw $(FW) $@

$(HARNESSES:%=esbmc-%): esbmc-%: $(BUILD)/params.h
	@$(RUN) esbmc/$*.cpp
$(BOOTED:%=esbmc-boot-%): esbmc-boot-%: $(BUILD)/params.h
	@$(RUN) -D BOOT esbmc/$*.cpp
$(HARNESSES:%=esbmc-reach-%): esbmc-reach-%: $(BUILD)/params.h
	@$(RUN) -D REACH --expect fail esbmc/$*.cpp

$(MODELS:tla/%.cfg=tla-%): tla-%:
	@$(PYTHON) tlc_trace.py --jar $(TLA2TOOLS) tla/$*.cfg

clean:
	rm -rf $(BUILD) tla/states
