#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build the native packages of this container's distribution from HEAD,
# with the pinned inputs already fetched into $INPUTS. Run from the
# repository root.
set -eu
: "${INPUTS:?set INPUTS to the directory make inputs fills}"
INPUTS=$(realpath "$INPUTS")
git config --global --add safe.directory "$PWD"
. /etc/os-release
case "$ID" in
debian|ubuntu)
	apt-get build-dep -qy .
	make deb INPUTS="$INPUTS"
	ls build/deb/*.deb ;;
fedora)
	dnf builddep -qy packaging/rpm/gpu-unlock.spec
	make rpm INPUTS="$INPUTS"
	ls build/rpm/RPMS/noarch/*.rpm ;;
opensuse*)
	zypper -n install $(rpmspec -q --buildrequires packaging/rpm/gpu-unlock.spec)
	make rpm INPUTS="$INPUTS"
	ls build/rpm/RPMS/noarch/*.rpm ;;
arch)
	# makepkg refuses to run as root.
	useradd -m builder
	chown -R builder "$PWD" "$INPUTS"
	su builder -c "git config --global --add safe.directory '$PWD' &&
		cd packaging/arch && SRCDEST='$INPUTS' makepkg -f --noconfirm"
	ls packaging/arch/*.pkg.tar.zst ;;
*)
	echo "unknown distribution: $ID" >&2
	exit 1 ;;
esac
