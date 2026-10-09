# Host-compiled unit tests for link.cpp and config.cpp (see README.md). Needs g++ and the rweather Crypto library
# (the one the firmware builds with): make CRYPTO=<its src directory>
FW := ../../firmware/GateLink
CRYPTO ?= $(HOME)/Arduino/libraries/Crypto/src
CXX ?= g++
BUILD := build

FLAGS := -std=gnu++17 -g -O1 -DHOST_BUILD -fsanitize=address,undefined -fno-sanitize-recover=undefined
CXXFLAGS := $(FLAGS) -Wall -Wextra -Werror -Istubs -I. -I$(FW) -I$(CRYPTO)

SRCS := sim.cpp link_house.cpp link_gate.cpp test_link.cpp test_config.cpp
OBJS := $(SRCS:%.cpp=$(BUILD)/%.o) $(BUILD)/config.o
LIB_OBJS := $(addprefix $(BUILD)/crypto_,Crypto.o Hash.o SHA256.o)

# ASan in GCC 12 can loop on DEADLYSIGNAL at startup under kernels with vm.mmap_rnd_bits = 32 (Docker Desktop's
# WSL2 kernel): run with address randomization off when setarch may do that.
NORAND := $(shell setarch $$(uname -m) -R true 2>/dev/null && echo setarch $$(uname -m) -R)

.PHONY: run clean
run: $(BUILD)/tests
	$(NORAND) $(BUILD)/tests

$(BUILD)/tests: $(OBJS) $(LIB_OBJS)
	$(CXX) $(FLAGS) -o $@ $^

$(BUILD)/%.o: %.cpp sim.h $(wildcard stubs/*.h) $(wildcard $(FW)/*.h) $(FW)/link.cpp link_node.inc | $(BUILD)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/config.o: $(FW)/config.cpp $(wildcard stubs/*.h) $(wildcard $(FW)/*.h) | $(BUILD)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Third-party: no warning flags.
$(BUILD)/crypto_%.o: $(CRYPTO)/%.cpp | $(BUILD)
	$(CXX) $(FLAGS) -I$(CRYPTO) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
