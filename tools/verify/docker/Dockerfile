# The model checkers for tools/verify on a machine without them (Windows, macOS), as CI installs them
# (.github/workflows/ci.yml, job verify): ESBMC (its release build, checked against GitHub's digest of it), TLC
# (tla2tools.jar, checked against the hash recorded when it was first downloaded: the release has no digest) and Java.
# Versions: keep in step with ci.yml and docs/development.md. Build: docker build -t gatelink-verify tools/verify/docker
FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    default-jre-headless python3 make curl ca-certificates unzip libc6-dev \
    && rm -rf /var/lib/apt/lists/*
ARG ESBMC_VERSION=8.5
ARG ESBMC_SHA256=d8da304dd0dfce6c9f488379f03a72e768bce8598c22478c980e1704247352f1
ARG TLA_VERSION=1.7.4
ARG TLA_SHA256=936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88
RUN curl -fsSL -o /tmp/esbmc.zip "https://github.com/esbmc/esbmc/releases/download/v${ESBMC_VERSION}/esbmc-linux.zip" \
    && echo "${ESBMC_SHA256}  /tmp/esbmc.zip" | sha256sum -c - \
    && mkdir -p /opt/esbmc && unzip -q /tmp/esbmc.zip -d /opt/esbmc && rm /tmp/esbmc.zip \
    && ln -s /opt/esbmc/release/bin/esbmc /usr/local/bin/esbmc \
    && curl -fsSL -o /opt/tla2tools.jar "https://github.com/tlaplus/tlaplus/releases/download/v${TLA_VERSION}/tla2tools.jar" \
    && echo "${TLA_SHA256}  /opt/tla2tools.jar" | sha256sum -c -
