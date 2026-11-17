#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
root=$(git -C "$(dirname "$0")/../.." rev-parse --show-toplevel)
temp=$(mktemp -d)
trap 'rm -rf "$temp"' EXIT
git clone -q --no-hardlinks "$root" "$temp/repo"
cd "$temp/repo"
# Test the working packaging changes as well as a committed checkout.
while IFS= read -r path; do
    mkdir -p "$(dirname "$path")"
    cp "$root/$path" "$path"
done < "$root/packaging/source-files"
mkdir -p firmware/fwsec tests/integration emulator
echo private-probe > kernel/gpu_unlock/private.c
echo private-firmware > firmware/fwsec/private.dis
echo private-model > tests/integration/private.c
echo private-rom > emulator/private.rom
git add kernel/gpu_unlock/private.c tests/integration/private.c
before=$(git hash-object "$(git rev-parse --git-path index)")
head=$(git rev-parse HEAD)
commit=$(packaging/snapshot.sh)
test "$before" = "$(git hash-object "$(git rev-parse --git-path index)")"
test "$head" = "$(git rev-parse HEAD)"
git ls-tree -r --name-only "$commit" > "$temp/actual"
cmp packaging/source-files "$temp/actual"
make -s DESTDIR="$temp/install" install-dkms
test -f "$temp/install/usr/src/gpu-unlock-$(cat VERSION)/main.c"
test ! -e "$temp/install/usr/src/gpu-unlock-$(cat VERSION)/private.c"
git archive "$commit" | tar -tf - > "$temp/archive"
if grep -Eq '(^|/)(emulator|integration|private[.]|fwsec/)' "$temp/archive"; then
    echo "Private material entered the release archive" >&2
    exit 1
fi
echo "Release snapshots and DKMS sources exclude staged and untracked experiments"
