# clang-tidy 18 (the version CI used) next to the same Qt and compiler as an
# Ubuntu 26.04 host, so a host build's compile_commands.json works unchanged
# inside. Used by tools/tidy-in-docker.sh; nothing is installed on the host.
FROM ubuntu:26.04
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        g++ cmake ninja-build jq python3-pip \
        qt6-base-dev qt6-svg-dev qt6-tools-dev qtkeychain-qt6-dev \
 && rm -rf /var/lib/apt/lists/*
RUN pip install --no-cache-dir --break-system-packages clang-tidy==18.1.8
