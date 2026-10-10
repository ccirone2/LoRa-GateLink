# Fuzz targets for the firmware's parsers (fuzz/README.md). Run from tests/native:
#   make -f fuzz/Makefile LIBS=<Arduino libraries> <goal>
# Goals:
#   replay       g++ build; runs the committed corpus (must pass) and fuzz/crashes (must still crash). No clang.
#   build-fuzz   libFuzzer binaries (clang): $(BUILD)/fuzz/fuzz_frames, fuzz_console, fuzz_config
#   fuzz-smoke   each target for FUZZ_SECONDS (60) from its corpus; new inputs go to $(BUILD)/fuzz/work/<target>,
#                crashes to $(BUILD)/fuzz/artifacts/
#   corpus-merge minimise what fuzzing found into fuzz/corpus/<target> (keeps only inputs that add coverage)
#   coverage     line coverage of the firmware by the corpus and what fuzzing found (clang source-based coverage)
FW := ../../firmware/GateLink
LIBS ?= $(HOME)/Arduino/libraries
CRYPTO ?= $(LIBS)/Crypto/src
JSON ?= $(LIBS)/ArduinoJson/src
BUILD ?= build
FB := $(BUILD)/fuzz
CLANGXX ?= clang++
OBJDUMP ?= llvm-objdump
LLVM_PROFDATA ?= llvm-profdata
LLVM_COV ?= llvm-cov
CXX ?= g++
FUZZ_SECONDS ?= 60
FUZZ_FLAGS ?=

TARGETS := fuzz_frames fuzz_console fuzz_config
FW_SRCS := app link role_gate role_house io log history console config
CRYPTO_SRCS := Crypto Hash SHA256
HARNESS := hal harness
HEADERS := $(wildcard fuzz/*.h stubs/*.h $(FW)/*.h)

INCS := -Istubs -Ifuzz -I$(FW) -isystem $(CRYPTO) -isystem $(JSON)
SAN := -fsanitize=address,undefined -fno-sanitize-recover=undefined
WARN := -Wall -Wextra -Werror

# --- libFuzzer build (clang): firmware RAM restored per input from the gl_bss/gl_data snapshot (sections.h) ---------
LF := $(FB)/lf
LF_FLAGS := -std=gnu++17 -g -O1 -DHOST_BUILD -DFUZZ_SNAPSHOT=1 $(SAN)
LF_FW := $(FW_SRCS:%=$(LF)/fw/%.o)
LF_LIB := $(CRYPTO_SRCS:%=$(LF)/crypto_%.o)
FUZZERS := $(TARGETS:%=$(FB)/%)

# --- replay build (g++, or any compiler): one fork per input ---------------------------------------------------------
RP := $(FB)/gcc
RP_FLAGS := -std=gnu++17 -g -O1 -DHOST_BUILD $(SAN)
# GCC 13 reports maybe-uninitialized inside ArduinoJson's inlined iterators (as in ../Makefile).
RP_WARN := $(WARN) -Wno-maybe-uninitialized
RP_FW := $(FW_SRCS:%=$(RP)/fw/%.o)
RP_LIB := $(CRYPTO_SRCS:%=$(RP)/crypto_%.o)
REPLAYS := $(TARGETS:fuzz_%=$(FB)/replay_%)

# --- coverage build (clang, no sanitizers): the replay driver with source-based coverage -------------------------
CV := $(FB)/cov
CV_FLAGS := -std=gnu++17 -g -O0 -DHOST_BUILD -fprofile-instr-generate -fcoverage-mapping
CV_FW := $(FW_SRCS:%=$(CV)/fw/%.o)
CV_LIB := $(CRYPTO_SRCS:%=$(CV)/crypto_%.o)
COVS := $(TARGETS:fuzz_%=$(CV)/replay_%)

# Per target: input size limit and dictionary.
MAXLEN_fuzz_frames := 4096
MAXLEN_fuzz_console := 8192
MAXLEN_fuzz_config := 16384
DICT_fuzz_frames :=
DICT_fuzz_console := -dict=fuzz/console.dict
DICT_fuzz_config := -dict=fuzz/config.dict

# ASan in GCC 12 can loop on DEADLYSIGNAL at startup under kernels with vm.mmap_rnd_bits = 32 (../Makefile).
NORAND := $(shell setarch $$(uname -m) -R true 2>/dev/null && echo setarch $$(uname -m) -R)

# Keep the objects pattern rules build on the way to a binary (make would delete them as intermediates).
.SECONDARY:

.PHONY: replay build-replay build-fuzz fuzz-smoke corpus-merge coverage check-sections clean-fuzz
replay: build-replay
	@for t in $(TARGETS:fuzz_%=%); do \
	  $(NORAND) $(FB)/replay_$$t fuzz/corpus/fuzz_$$t || exit 1; \
	  if [ -d fuzz/crashes/fuzz_$$t ] && [ -n "$$(ls fuzz/crashes/fuzz_$$t)" ]; then \
	    GATELINK_FUZZ_KNOWN_BUGS=1 $(NORAND) $(FB)/replay_$$t --expect-crash fuzz/crashes/fuzz_$$t || exit 1; \
	  fi; \
	done

build-replay: $(REPLAYS)

build-fuzz: $(FUZZERS) check-sections

# Every firmware global must sit in gl_bss/gl_data, or it would keep its value from one input to the next.
check-sections: $(LF_FW)
	@sh fuzz/check_sections.sh $(OBJDUMP) $(LF_FW)

fuzz-smoke: build-fuzz
	@mkdir -p $(FB)/artifacts
	@for t in $(TARGETS); do \
	  mkdir -p $(FB)/work/$$t; \
	  echo "== $$t: $(FUZZ_SECONDS) s"; \
	  $(FB)/$$t -max_total_time=$(FUZZ_SECONDS) -timeout=25 -rss_limit_mb=2048 -print_final_stats=1 \
	    -artifact_prefix=$(FB)/artifacts/$$t- $$(case $$t in \
	      fuzz_frames) echo "-max_len=$(MAXLEN_fuzz_frames) $(DICT_fuzz_frames)";; \
	      fuzz_console) echo "-max_len=$(MAXLEN_fuzz_console) $(DICT_fuzz_console)";; \
	      fuzz_config) echo "-max_len=$(MAXLEN_fuzz_config) $(DICT_fuzz_config)";; esac) \
	    $(FUZZ_FLAGS) $(FB)/work/$$t fuzz/corpus/$$t || exit 1; \
	done

corpus-merge: build-fuzz
	@for t in $(TARGETS); do \
	  [ -d $(FB)/work/$$t ] || continue; \
	  $(FB)/$$t -merge=1 fuzz/corpus/$$t $(FB)/work/$$t || exit 1; \
	done

# Every input of the corpus and of work/ (what fuzzing found) runs once; the report covers the firmware files.
coverage: $(COVS)
	@rm -rf $(CV)/raw && mkdir -p $(CV)/raw
	@for t in $(TARGETS:fuzz_%=%); do \
	  w=$(FB)/work/fuzz_$$t; [ -d $$w ] || w=; \
	  LLVM_PROFILE_FILE=$(CV)/raw/$$t-%p.profraw $(CV)/replay_$$t fuzz/corpus/fuzz_$$t $$w > /dev/null 2>&1; \
	done; true
	@$(LLVM_PROFDATA) merge -sparse $(CV)/raw/*.profraw -o $(CV)/all.profdata
	@$(LLVM_COV) report $(firstword $(COVS)) $(addprefix -object ,$(wordlist 2,9,$(COVS))) \
	  -instr-profile=$(CV)/all.profdata $(FW_SRCS:%=$(FW)/%.cpp)

# libFuzzer binaries
$(FB)/fuzz_%: $(LF)/fuzz_%.o $(HARNESS:%=$(LF)/%.o) $(LF_FW) $(LF_LIB)
	$(CLANGXX) $(LF_FLAGS) -fsanitize=fuzzer -o $@ $^

$(LF)/fw/%.o: $(FW)/%.cpp $(HEADERS) | $(LF)/fw
	$(CLANGXX) $(LF_FLAGS) -fsanitize=fuzzer-no-link $(WARN) $(INCS) -include fuzz/sections.h -c -o $@ $<

$(LF)/%.o: fuzz/%.cpp $(HEADERS) | $(LF)
	$(CLANGXX) $(LF_FLAGS) -fsanitize=fuzzer-no-link $(WARN) $(INCS) -c -o $@ $<

# Third-party: no warning flags, no coverage (the fuzzer has nothing to learn inside SHA-256).
$(LF)/crypto_%.o: $(CRYPTO)/%.cpp | $(LF)
	$(CLANGXX) $(LF_FLAGS) -I$(CRYPTO) -c -o $@ $<

# Replay binaries
$(FB)/replay_%: $(RP)/fuzz_%.o $(RP)/replay_main.o $(HARNESS:%=$(RP)/%.o) $(RP_FW) $(RP_LIB)
	$(CXX) $(RP_FLAGS) -o $@ $^

$(RP)/fw/%.o: $(FW)/%.cpp $(HEADERS) | $(RP)/fw
	$(CXX) $(RP_FLAGS) $(RP_WARN) $(INCS) -include fuzz/sections.h -c -o $@ $<

$(RP)/%.o: fuzz/%.cpp $(HEADERS) | $(RP)
	$(CXX) $(RP_FLAGS) $(RP_WARN) $(INCS) -c -o $@ $<

$(RP)/crypto_%.o: $(CRYPTO)/%.cpp | $(RP)
	$(CXX) $(RP_FLAGS) -I$(CRYPTO) -c -o $@ $<

# Coverage binaries
$(CV)/replay_%: $(CV)/fuzz_%.o $(CV)/replay_main.o $(HARNESS:%=$(CV)/%.o) $(CV_FW) $(CV_LIB)
	$(CLANGXX) $(CV_FLAGS) -o $@ $^

$(CV)/fw/%.o: $(FW)/%.cpp $(HEADERS) | $(CV)/fw
	$(CLANGXX) $(CV_FLAGS) $(WARN) $(INCS) -c -o $@ $<

$(CV)/%.o: fuzz/%.cpp $(HEADERS) | $(CV)
	$(CLANGXX) $(CV_FLAGS) $(WARN) $(INCS) -c -o $@ $<

$(CV)/crypto_%.o: $(CRYPTO)/%.cpp | $(CV)
	$(CLANGXX) $(CV_FLAGS) -I$(CRYPTO) -c -o $@ $<

$(LF) $(LF)/fw $(RP) $(RP)/fw $(CV) $(CV)/fw:
	mkdir -p $@

clean-fuzz:
	rm -rf $(FB)
