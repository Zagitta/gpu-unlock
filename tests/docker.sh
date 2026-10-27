#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Run a CI job of the working tree in a fresh container of each
# distribution: "module" builds the module against the distribution
# kernels, "package" builds, installs and removes the native packages,
# "local" installs and removes the tree with the packager-free path.
# Run from the repository root; the arguments after the job pick the
# images, all of them by default, Debian's for "local".
set -eu
job=${1:?usage: tests/docker.sh module|package|local [image...]}
shift
INPUTS=${INPUTS:-$HOME/.cache/gpu-unlock/inputs}
mounts=
case "$job" in
package|local)
	make inputs INPUTS="$INPUTS"
	mounts="-v $(realpath "$INPUTS"):/inputs:ro" ;;
esac
commit=$(packaging/snapshot.sh)
if [ $# -eq 0 ]; then
	if [ "$job" = local ]; then
		set -- debian:13
	else
		set -- debian:12 debian:13 ubuntu:22.04 ubuntu:24.04 \
			ubuntu:26.04 fedora:43 fedora:latest archlinux:latest \
			opensuse/tumbleweed
	fi
fi

# The same setup as the CI jobs. The job runs on a clone so that nothing
# in the checkout ends up owned by root.
script='
. /etc/os-release
case "$ID" in
debian|ubuntu)
	apt-get update -q
	apt-get install -qy --no-install-recommends ca-certificates git make xz-utils zstd ;;
fedora)
	dnf install -qy git make xz zstd rpm-build dnf-plugins-core ;;
opensuse*)
	zypper -n install git make xz zstd rpm-build ;;
arch)
	pacman -Syu --noconfirm --needed base-devel git zstd ;;
esac
git config --global --add safe.directory "*"
if [ "$2" = local ]; then
	# The packager-free test clones the checkout itself.
	cd /src
	SRC=/src INPUTS=/inputs sh tests/docker/test-local-install.sh "$1"
else
	git clone -q --shared /src /work
	cd /work
	git checkout -q "$1"
	case "$2" in
	module)
		sh .github/ci/module.sh ;;
	package)
		cp -r /inputs /tmp/inputs
		INPUTS=/tmp/inputs sh .github/ci/package.sh
		sh tests/packaging/install.sh ;;
	esac
fi
'

failed=
for image; do
	echo "==> $image"
	# shellcheck disable=SC2086
	docker run --rm -v "$PWD:/src:ro" $mounts "$image" \
		sh -euc "$script" sh "$commit" "$job" || failed="$failed $image"
done
[ -z "$failed" ] || { echo "failed:$failed" >&2; exit 1; }
echo "$job passed on all images"
