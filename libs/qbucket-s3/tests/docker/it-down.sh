#!/bin/sh
# Stops the local S3 test backends and deletes their data volumes.
set -eu
cd "$(dirname "$0")"
docker compose down -v
