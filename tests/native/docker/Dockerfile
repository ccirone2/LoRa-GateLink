# Linux toolchain for the host tests on a machine without one (Windows, macOS): g++ (tests, systests), clang with
# libFuzzer (tests/native/fuzz), cppcheck and clang-tidy (static analysis), gcovr (coverage). Matches CI's
# ubuntu-latest closely enough. Build: docker build -t gatelink-dev tests/native/docker
FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    g++ make clang llvm lld libclang-rt-18-dev cppcheck clang-tidy gcovr python3 python3-pip ca-certificates git \
    util-linux \
    && rm -rf /var/lib/apt/lists/*
