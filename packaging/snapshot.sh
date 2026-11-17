#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Snapshot only the explicit release source list. The checkout, index and
# branches stay untouched; the snapshot ref keeps the build commit reachable.
set -eu
cd "$(dirname "$0")/.."
index_dir=$(mktemp -d)
trap 'rm -rf "$index_dir"' EXIT
index=$index_dir/index
while IFS= read -r path; do
    case "$path" in
        ''|/*|..|../*|*/../*|*/..|.git|.git/*)
            echo "Invalid release source path: $path" >&2; exit 1 ;;
    esac
    if [ ! -f "$path" ] || [ -L "$path" ]; then
        echo "Release source must be a regular file: $path" >&2
        exit 1
    fi
done < packaging/source-files
GIT_INDEX_FILE=$index git read-tree --empty
GIT_INDEX_FILE=$index git --literal-pathspecs add --pathspec-from-file=packaging/source-files
tree=$(GIT_INDEX_FILE=$index git write-tree)
head=$(git rev-parse HEAD)
if [ "$tree" = "$(git rev-parse "HEAD^{tree}")" ]; then
	commit=$head
else
	date=$(git log -1 --format=%cd --date=raw HEAD)
	commit=$(GIT_AUTHOR_NAME=gpu-unlock GIT_AUTHOR_EMAIL=snapshot@localhost \
		GIT_COMMITTER_NAME=gpu-unlock GIT_COMMITTER_EMAIL=snapshot@localhost \
		GIT_AUTHOR_DATE=$date GIT_COMMITTER_DATE=$date \
		git commit-tree --no-gpg-sign "$tree" -p "$head" -m "Working tree snapshot")
fi
git update-ref refs/gpu-unlock/snapshot "$commit"
echo "$commit"
