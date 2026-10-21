#!/bin/sh
# clang-tidy over the project's own sources (src/ and tests/ of the app and
# the library), in parallel, optionally only one shard of them — CI splits
# the files across several runners. The build directory needs
# compile_commands.json and the moc output; building the `autogen` target
# (configure with -DCMAKE_GLOBAL_AUTOGEN_TARGET=ON
# -DCMAKE_AUTOGEN_ORIGIN_DEPENDS=OFF, seconds) is enough.
#
#   tools/run-clang-tidy.sh BUILD_DIR [SHARD SHARDS]
#
# CLANG_TIDY picks the binary (default: clang-tidy). Prints the findings of
# failing files only; exits non-zero if any file fails or none was found.
set -eu
build=$1
shard=${2:-0}
shards=${3:-1}
tidy=${CLANG_TIDY:-clang-tidy}

list=$(mktemp)
trap 'rm -f "$list"' EXIT
jq -r '.[].file' "$build/compile_commands.json" \
    | grep -E '/(src|tests)/.*\.cpp$' | grep -v '_autogen' | sort -u \
    | awk -v k="$shard" -v n="$shards" '(NR - 1) % n == k' > "$list"
count=$(wc -l < "$list")
if [ "$count" -eq 0 ]; then
    echo "clang-tidy: no files in shard $shard of $shards"
    exit 1
fi
echo "clang-tidy: $count files (shard $shard of $shards), $(nproc) at a time"

# One clang-tidy per file; its output is printed in one piece, only when it
# fails (WarningsAsErrors '*' makes every finding a failure).
export tidy build
if xargs -P "$(nproc)" -I{} sh -c '
        out=$("$tidy" -p "$build" --quiet "$1" 2>&1) && exit 0
        printf "%s\n\n" "$out"
        exit 1' sh {} < "$list"; then
    echo "clang-tidy: ok"
else
    echo "clang-tidy: findings above"
    exit 1
fi
