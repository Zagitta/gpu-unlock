#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Print as a JSON list the stable Unraid releases with no package of this
# VERSION yet, from 7.3.0, the first to ship its kernel config and patches.
# The built.txt asset of the unraid release lists "<Unraid> <VERSION>" per
# package built. Needs curl, jq and gh. Run from the repository root.
set -eu
built=$(gh release download unraid -p built.txt -O - 2>/dev/null || :)
curl -sSf https://releases.unraid.net/json | jq -c --arg built "$built" --arg v "$(cat VERSION)" '
	($built | split("\n")) as $done
	| [.[] | select((.version | split(".") | map(tonumber? // 0)) >= [7, 3, 0])
	       | select("\(.version) \($v)" | IN($done[]) | not)
	       | {version, url: (.url | split("?")[0]), sha256}]'
