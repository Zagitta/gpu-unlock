#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Commit the working tree, uncommitted and untracked files included, on top
# of HEAD and print its id. The checkout, index and branches are untouched;
# refs/gpu-unlock/snapshot keeps the commit reachable for makepkg's clone.
# A clean tree prints HEAD. The same tree and HEAD give the same id.
set -eu
cd "$(dirname "$0")/.."
index=$(mktemp)
trap 'rm -f "$index"' EXIT
cp "$(git rev-parse --git-path index)" "$index"
# .claude holds Claude Code session state and worktrees, not source.
GIT_INDEX_FILE=$index git add -A -- . ':!.claude'
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
