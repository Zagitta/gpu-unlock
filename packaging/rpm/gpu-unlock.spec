%global nv 610.43.03

Name: gpu-unlock
Version: 1.0.0
Release: 1%{?dist}
Summary: RTX 3090 BF16 unlock before the NVIDIA driver loads
License: GPL-2.0-only
BuildArch: noarch
Source0: gpu-unlock-%{version}.tar.xz
Source1: https://download.nvidia.com/XFree86/NVIDIA-kernel-module-source/NVIDIA-kernel-module-source-%{nv}.tar.xz
Source2: https://archive.cachyos.org/archive/cachyos-v3/nvidia-utils-%{nv}-1-x86_64_v3.pkg.tar.zst
# NVIDIA inputs stay out of the source RPM (it becomes a .nosrc.rpm).
NoSource: 1
NoSource: 2
BuildRequires: make
BuildRequires: gcc
BuildRequires: python3
BuildRequires: zstd

%description
Kernel module that unlocks RTX 3090 BF16 throughput before the unmodified
NVIDIA driver binds, opted in on the kernel command line.

%package dkms
Summary: RTX 3090 BF16 unlock kernel source
Requires: dkms
Requires: dracut
Requires: gpu-unlock-firmware = %{version}-%{release}

%description dkms
DKMS source for gpu_unlock, loaded ahead of nvidia on every boot. It does
nothing unless gpu_unlock.targets= is on the kernel command line.

%package firmware
Summary: Locally generated pinned NVIDIA firmware for GPU unlock
License: LicenseRef-NVIDIA AND GPL-2.0-only

%description firmware
Firmware assets generated locally from verified upstream inputs.

%prep
sha256sum -c - <<EOF
7e118923c7a23edc36114d63273a46e3e04e9af98695a42203e7ac2dfe9fc1dc  %{SOURCE1}
e17e7c92eeffa178b0fe1f40eb17a47095f8fc3ce4a77bf115cf5bd5a4df86e9  %{SOURCE2}
EOF
%autosetup
test "$(cat VERSION)" = "%{version}"

%build
%make_build INPUTS=%{_sourcedir} ASSETS="$PWD/assets" firmware

%check
make test ASSETS="$PWD/assets"

%install
make DESTDIR=%{buildroot} ASSETS="$PWD/assets" \
    install-dkms install-firmware install-dracut

%files dkms
%license licenses/GPL-2.0.txt licenses/CMP40HX-MIT.txt licenses/NVIDIA-MIT.txt
%doc %{_docdir}/gpu-unlock
%{_usrsrc}/gpu-unlock-%{version}
%{_prefix}/lib/dracut/dracut.conf.d/gpu-unlock.conf

%files firmware
%license %{_defaultlicensedir}/gpu-unlock-firmware
%{_prefix}/lib/firmware/gpu-unlock

%post dkms
dkms add -m gpu-unlock -v %{version} --rpm_safe_upgrade || :
# dkms add only registers the source; build and install the module for the
# running kernel, then rebuild its initramfs so the module and its firmware
# are in the image before nvidia can load. New kernels pick the drop-in up
# automatically when they are installed (dkms autoinstalls for them).
dkms build -m gpu-unlock -v %{version} --force || :
dkms install -m gpu-unlock -v %{version} --force || :
if [ -x /usr/sbin/dracut ]; then dracut --force || :; fi

%preun dkms
if [ "$1" -eq 0 ]; then
    # dkms remove can leave weak-updates symlinks for other kernels
    # behind; drop them and refresh the module maps of every kernel
    # before dkms remove: dkms only runs depmod for the kernel the
    # module was built for, and a later dracut run (dkms itself can
    # have a post_transaction hook) would fail on dangling entries.
    rm -f /lib/modules/*/weak-updates/gpu_unlock.ko*
    # depmod -a covers only the running kernel on kmod >= 31.
    for k in /lib/modules/*; do [ -f "$k/config" ] && depmod "${k##*/}" || :; done
    if [ -d /var/lib/dkms/gpu-unlock/%{version} ]; then
        dkms remove -m gpu-unlock -v %{version} --all --rpm_safe_upgrade || :
    fi
fi

%postun dkms
# Drop gpu_unlock from the initramfs now that the module and the dracut
# drop-in are gone.
if [ "$1" -eq 0 ] && [ -x /usr/sbin/dracut ]; then dracut --force || :; fi

%postun firmware
# The firmware is gone from disk; refresh the initramfs after the dkms
# package's postremove has already dropped the module.
if [ "$1" -eq 0 ] && [ -x /usr/sbin/dracut ]; then dracut --force || :; fi

%changelog
* Mon Sep 28 2026 Zagitta <zetlon+github@gmail.com> - 1.0.0-1
- Initial release.
