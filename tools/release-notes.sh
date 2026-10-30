#!/bin/sh
# The notes of a GitHub release: the version's section of CHANGELOG.md (its
# relative links pointed at the tag), how to install, and the packages'
# SHA-256 sums. Run from the repository root.
#
#   tools/release-notes.sh VERSION [SHA256SUMS]
set -eu
version=${1:?version}
sums=${2:-}
repo=https://github.com/theinvisible/qbucket

section=$(awk -v v="$version" '
    index($0, "## [" v "]") == 1 { on = 1; next }
    on && /^## \[/ { exit }
    on { print }' CHANGELOG.md)
if [ -z "$(printf '%s' "$section" | tr -d '[:space:]')" ]; then
    echo "release-notes: CHANGELOG.md has no section for $version" >&2
    exit 1
fi
# Without the leading blank lines; links like (docs/devlog/) to the tag.
printf '%s\n' "$section" | sed -e '/./,$!d' \
    -e "s#](\([^):/\#][^):]*\))#]($repo/blob/v$version/\1)#g"

cat <<EOF

## Install

| Platform | Package |
|---|---|
| Windows 10 / 11 (x64) | \`qbucket-$version-windows-x64-setup.exe\` (per user, or for all users) or the ZIP |
| Ubuntu 26.04 | \`sudo apt install ./qbucket_$version-1_amd64.deb\` |
| Any Linux with Flatpak | \`flatpak install --user qbucket-$version-x86_64.flatpak\` (the KDE runtime comes from Flathub) |

\`qbucket --self-test\` checks an installation without opening a window. The
Windows setup and executable are not signed yet: SmartScreen asks first
("More info" → "Run anyway").
EOF

if [ -n "$sums" ]; then
    printf '\n## SHA-256\n\n```\n'
    cat "$sums"
    printf '```\n'
fi
