#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Publish already validated files. Never execute PKGBUILD with AUR credentials.
set -euo pipefail

: "${AUR_GIT_NAME:?Set the accepting maintainer's Git name}"
: "${AUR_GIT_EMAIL:?Set the accepting maintainer's Git email}"
: "${EXPECTED_AUR_COMMIT:?Set the reviewed AUR commit, or empty for first publication}"
if [[ $EXPECTED_AUR_COMMIT != empty && ! $EXPECTED_AUR_COMMIT =~ ^[0-9a-f]{40}$ ]]; then
    echo 'EXPECTED_AUR_COMMIT must be a full commit SHA or empty' >&2
    exit 1
fi

package_dir=$(realpath "${1:-packaging/aur}")
files=(PKGBUILD .SRCINFO gpu-unlock.install .gitignore)
for file in "${files[@]}"; do
    if [[ ! -f $package_dir/$file || -L $package_dir/$file ]]; then
        printf 'Missing regular packaging file: %s\n' "$file" >&2
        exit 1
    fi
done
grep -qx 'pkgbase = gpu-unlock' "$package_dir/.SRCINFO" || {
    echo 'Refusing to publish a different package base' >&2
    exit 1
}
grep -Eq '^# Maintainer: [^[:space:]].*<[^<>]+>' "$package_dir/PKGBUILD" || {
    echo 'An accepting AUR maintainer must add their name and contact before publication' >&2
    exit 1
}

work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
remote=ssh://aur@aur.archlinux.org/gpu-unlock.git
git clone --quiet --no-checkout -- "$remote" "$work/aur"

if tip=$(git -C "$work/aur" rev-parse --verify refs/remotes/origin/master 2>/dev/null); then
    [[ $tip == "$EXPECTED_AUR_COMMIT" ]] || {
        echo 'AUR master differs from the reviewed commit; review it before publishing' >&2
        exit 1
    }
    git -C "$work/aur" checkout --quiet --detach origin/master
else
    [[ $EXPECTED_AUR_COMMIT == empty ]] || {
        echo 'AUR master does not exist, but an existing commit was expected' >&2
        exit 1
    }
    [[ -z $(git -C "$work/aur" for-each-ref refs/remotes/origin) ]] || {
        echo 'AUR repository has unexpected branches; refusing initial publication' >&2
        exit 1
    }
    git -C "$work/aur" checkout --quiet --orphan master
fi

# Replace only the explicit packaging files; preserve unrelated remote files.
for file in "${files[@]}"; do
    [[ ! -d $work/aur/$file ]] || {
        printf 'Unexpected remote directory: %s\n' "$file" >&2
        exit 1
    }
    rm -f -- "$work/aur/$file"
    install -m644 -- "$package_dir/$file" "$work/aur/$file"
done
git -C "$work/aur" add -- "${files[@]}"
if git -C "$work/aur" diff --cached --quiet; then
    echo 'AUR packaging is already up to date'
    exit 0
fi

git -C "$work/aur" -c user.name="$AUR_GIT_NAME" -c user.email="$AUR_GIT_EMAIL" \
    commit --quiet -m 'Update reviewed gpu-unlock packaging'
# A concurrent remote update is rejected by this ordinary, non-force push.
git -C "$work/aur" push origin HEAD:refs/heads/master
