# Host-compiled tests (see README.md). Needs g++ and the libraries the firmware builds with:
#   make LIBS=<Arduino libraries directory>     (or CRYPTO=<Crypto/src> JSON=<ArduinoJson/src>)
# tests:    link.cpp and config.cpp on their own (test_link.cpp, test_config.cpp)
# systests: both boards' whole firmware in the simulated site (world.h; test_system*.cpp)
# fuzz:     the fuzz corpus replayed as a regression test (g++; fuzzing itself needs clang, see fuzz/README.md)
FW := ../../firmware/GateLink
LIBS ?= $(HOME)/Arduino/libraries
CRYPTO ?= $(LIBS)/Crypto/src
JSON ?= $(LIBS)/ArduinoJson/src
CXX ?= g++
BUILD ?= build

SAN := -fsanitize=address,undefined -fno-sanitize-recover=undefined
# COVERAGE=1: gcov counters for gcovr (tests/native/README.md, "Coverage").
FLAGS := -std=gnu++17 -g -O1 -DHOST_BUILD $(SAN) $(if $(COVERAGE),--coverage)
# GCC 13 reports maybe-uninitialized inside ArduinoJson's inlined iterators (false positives, more so with ASan).
WNO := -Wno-maybe-uninitialized
# Third-party headers as system headers: their warnings (ArduinoJson trips GCC 13 maybe-uninitialized) aren't ours.
CXXFLAGS := $(FLAGS) -Wall -Wextra -Werror $(WNO) -Istubs -I. -I$(FW) -isystem $(CRYPTO) -isystem $(JSON)
CRYPTO_SRCS := Crypto Hash SHA256

# --- tests: link and config ------------------------------------------------------------------------------------
SRCS := testing.cpp sim.cpp link_house.cpp link_gate.cpp test_link.cpp test_config.cpp
OBJS := $(SRCS:%.cpp=$(BUILD)/%.o) $(BUILD)/config.o
LIB_OBJS := $(CRYPTO_SRCS:%=$(BUILD)/crypto_%.o)

# --- systests: the firmware as a shared object, loaded once per board --------------------------------------------
# Everything but the drivers (radio, flash, charger, console transport) and GateLink.ino, whose calls the test
# program answers. -Bsymbolic keeps each copy's references inside it; -fno-gnu-unique lets dlclose really unload it
# (a reset starts with fresh RAM).
NODE_FW := app link role_gate role_house io log history console config
NODE_OBJS := $(NODE_FW:%=$(BUILD)/node/%.o) $(BUILD)/node/api.o $(CRYPTO_SRCS:%=$(BUILD)/node/crypto_%.o)
NODE_FLAGS := -fPIC -fno-gnu-unique
SYS_SRCS := testing.cpp world.cpp $(wildcard test_system*.cpp)
SYS_OBJS := $(SYS_SRCS:%.cpp=$(BUILD)/%.o)

# ASan in GCC 12 can loop on DEADLYSIGNAL at startup under kernels with vm.mmap_rnd_bits = 32 (Docker Desktop's
# WSL2 kernel): run with address randomization off when setarch may do that.
# Parallel system test shards (make JOBS=1 for one process); T= a test name substring to run only those.
JOBS ?= $(shell nproc 2>/dev/null || echo 1)
NORAND := $(shell setarch $$(uname -m) -R true 2>/dev/null && echo setarch $$(uname -m) -R)
HEADERS := $(wildcard *.h stubs/*.h $(FW)/*.h)

.PHONY: run run-tests run-systests run-fuzz-replay coverage cppcheck tidy clean
run: run-tests run-systests run-fuzz-replay
run-tests: $(BUILD)/tests
	$(NORAND) $(BUILD)/tests
# The system tests run in JOBS processes, each a shard of them (--shard=I/N); their outputs are shown in turn.
run-systests: $(BUILD)/systests $(BUILD)/node.so
	@rm -f $(BUILD)/systests.*.out; rc=0; pids=; for i in $$(seq 0 $$(($(JOBS) - 1))); do \
	  $(NORAND) $(BUILD)/systests --shard=$$i/$(JOBS) $(T) > $(BUILD)/systests.$$i.out 2>&1 & pids="$$pids $$!"; \
	done; for p in $$pids; do wait $$p || rc=1; done; \
	cat $(BUILD)/systests.*.out; grep -h 'tests, ' $(BUILD)/systests.*.out | \
	  awk '{t += $$1; f += $$3} END {print "systests: " t " tests, " f " failed in " NR " shards"}'; exit $$rc
# Every input in fuzz/corpus must run clean, and every reproducer in fuzz/crashes must still crash.
run-fuzz-replay:
	$(MAKE) -f fuzz/Makefile CXX="$(CXX)" CRYPTO="$(CRYPTO)" JSON="$(JSON)" BUILD="$(BUILD)" replay

$(BUILD)/tests: $(OBJS) $(LIB_OBJS)
	$(CXX) $(FLAGS) -o $@ $^

$(BUILD)/systests: $(SYS_OBJS)
	$(CXX) $(FLAGS) -rdynamic -o $@ $^ -ldl

$(BUILD)/node.so: $(NODE_OBJS)
	$(CXX) $(FLAGS) -shared -Wl,-Bsymbolic -o $@ $^

$(BUILD)/%.o: %.cpp $(HEADERS) $(FW)/link.cpp link_node.inc | $(BUILD)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/config.o: $(FW)/config.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/node/%.o: $(FW)/%.cpp $(HEADERS) | $(BUILD)/node
	$(CXX) $(CXXFLAGS) $(NODE_FLAGS) -c -o $@ $<

$(BUILD)/node/api.o: node/api.cpp $(HEADERS) | $(BUILD)/node
	$(CXX) $(CXXFLAGS) $(NODE_FLAGS) -c -o $@ $<

# Third-party: no warning flags.
$(BUILD)/crypto_%.o: $(CRYPTO)/%.cpp | $(BUILD)
	$(CXX) $(FLAGS) -I$(CRYPTO) -c -o $@ $<

$(BUILD)/node/crypto_%.o: $(CRYPTO)/%.cpp | $(BUILD)/node
	$(CXX) $(FLAGS) $(NODE_FLAGS) -I$(CRYPTO) -c -o $@ $<

$(BUILD) $(BUILD)/node:
	mkdir -p $@

# --- static analysis and coverage (CI's static and coverage jobs) -----------------------------------------------
# The firmware files the host build compiles, with the same flags (radio.cpp, extflash.cpp, supply.cpp and
# GateLink.ino need the Arduino core: cppcheck reads them, clang-tidy doesn't).
TIDY ?= clang-tidy
TIDY_SRCS := $(NODE_FW:%=$(FW)/%.cpp)
cppcheck:
	cppcheck -q --error-exitcode=1 --enable=warning,performance,portability --inline-suppr --std=c++17 \
	  --language=c++ --suppress=missingIncludeSystem --suppress=missingInclude $(FW)/*.cpp $(FW)/*.ino
# Checks and exceptions: $(FW)/.clang-tidy.
tidy:
	$(TIDY) --quiet $(TIDY_SRCS) -- -std=gnu++17 -DHOST_BUILD -Istubs -I. -I$(FW) -isystem $(CRYPTO) -isystem $(JSON)
# COVERAGE=1 build in $(BUILD)-cov, the tests run, then gcovr on the firmware (MIN_LINES=n fails below n %).
coverage:
	$(MAKE) BUILD=$(BUILD)-cov COVERAGE=1 run-tests run-systests
	gcovr -r ../.. --gcov-object-directory $(CURDIR) --filter '$(abspath $(FW))/' -j $(JOBS) --print-summary \
	  --txt $(BUILD)-cov/coverage.txt --json-summary $(BUILD)-cov/coverage.json \
	  $(if $(MIN_LINES),--fail-under-line $(MIN_LINES)) $(BUILD)-cov

clean:
	rm -rf $(BUILD) $(BUILD)-cov
