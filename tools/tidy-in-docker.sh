#!/bin/sh
# Runs tools/run-clang-tidy.sh or tools/check-msvc-shadow.sh with clang-tidy
# 18 from a Docker image (tools/docker/tidy.Dockerfile) instead of a local
# install. The repository is mounted read-only at its own path, so the paths
# in the build directory's compile_commands.json stay valid; the host must
# use the image's Qt (Ubuntu 26.04 packages).
#
#   tools/tidy-in-docker.sh run-clang-tidy.sh BUILD_DIR [SHARD SHARDS]
#   tools/tidy-in-docker.sh check-msvc-shadow.sh BUILD_DIR FILE...
set -eu
repo=$(cd "$(dirname "$0")/.." && pwd)
image=qbucket-tidy:18.1.8
script=$1
shift
docker build -q -t "$image" -f "$repo/tools/docker/tidy.Dockerfile" "$repo/tools/docker" >/dev/null
exec docker run --rm -u "$(id -u):$(id -g)" -v "$repo:$repo:ro" -w "$repo" \
    -e CLANG_TIDY=clang-tidy "$image" sh "tools/$script" "$@"
