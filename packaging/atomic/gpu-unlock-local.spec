# Local package for rpm-ostree systems, built by packaging/atomic/install.sh.
# rpmbuild --define "kver <uname -r>" --define "kcore <kernel-core version-release>"
#          --define "ver <VERSION>" --define "rel <release>" --define "nv <NVIDIA version>"
#
# By default rpmbuild strips .ko files, which removes an appended module
# signature; both lines below keep the module as built.
%global debug_package %{nil}
%global __os_install_post %{nil}

Name:      gpu-unlock-local
Version:   %{ver}
Release:   %{rel}%{?dist}
Summary:   gpu_unlock and its firmware for kernel %{kver}
License:   GPL-2.0-only AND LicenseRef-NVIDIA
BuildArch: x86_64
# The module only fits this kernel: an image update to a new kernel stops at
# this dependency until you rebuild, instead of booting without the unlock.
Requires:  kernel-core = %{kcore}

%description
gpu_unlock built locally for %{kver}, with the generated firmware and the
dracut drop-in that puts both in the initramfs.

%install
install -Dm644 %{_sourcedir}/gpu_unlock.ko %{buildroot}/usr/lib/modules/%{kver}/extra/gpu_unlock.ko
for f in booter_load_ga102.bin gsp_ga10x.bin gsp_boot_ga102.bin gsp_boot_desc_ga102.bin v67-ga102-%{nv}.bin; do
    install -Dm644 %{_sourcedir}/assets/$f %{buildroot}/usr/lib/firmware/gpu-unlock/%{nv}/$f
done
# dracut reads drop-ins in name order and loads force_drivers in that order.
# Images such as Bazzite force nvidia from 99-nvidia.conf; a later name would
# load gpu_unlock after nvidia has bound the card (unlock failed -16).
install -Dm644 %{_sourcedir}/dracut.conf %{buildroot}/usr/lib/dracut/dracut.conf.d/00-gpu-unlock.conf
install -Dm644 -t %{buildroot}/usr/share/licenses/%{name} %{_sourcedir}/licenses/*

# rpm-ostree only runs depmod when the kernel itself changes. Without
# gpu_unlock in modules.dep, dracut skips force_drivers and the module
# stays out of the initramfs.
%post
/usr/sbin/depmod -a %{kver} || :

%postun
/usr/sbin/depmod -a %{kver} || :

%files
/usr/lib/modules/%{kver}/extra/gpu_unlock.ko
/usr/lib/firmware/gpu-unlock
/usr/lib/dracut/dracut.conf.d/00-gpu-unlock.conf
%license /usr/share/licenses/%{name}
