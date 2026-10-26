# Fedora Atomic and Secure Boot

`make install` does not work on Fedora Atomic desktops (Silverblue, Kinoite,
Bazzite and the other rpm-ostree images): `/usr` is read-only, so DKMS cannot
build into it and `dnf install` is not available. With Secure Boot on, the
kernel also refuses an unsigned module. This page builds the module and its
firmware once, signs the module with your own key, and layers both as one
local RPM.

Tested on Bazzite 44 (`bazzite-dx-nvidia`, kernel `7.2.9-ogc3.1.fc44`, open
NVIDIA driver 615.78) with an RTX 3090 and Secure Boot on. Other rpm-ostree
images should work the same way but have not been tried.

## 1. Build the module and the firmware

You need `kernel-devel` for the kernel you boot (`ls /usr/src/kernels/$(uname -r)`).
Bazzite ships it. On Silverblue, either layer `kernel-devel-$(uname -r)` or do
this step in a toolbox that has it.

```sh
git clone https://github.com/Zagitta/gpu-unlock
cd gpu-unlock
make -C kernel/gpu_unlock KDIR=/usr/src/kernels/$(uname -r)
make assets        # downloads and checks the pinned NVIDIA inputs, writes ~/.cache/gpu-unlock/assets
```

## 2. Sign the module

Create a signing key once and keep the private key private (mode 600, not in
any repository):

```sh
mkdir -p ~/.local/share/gpu-unlock-mok && cd ~/.local/share/gpu-unlock-mok
openssl req -new -x509 -newkey rsa:2048 -nodes -days 3650 -sha256 \
    -subj "/CN=gpu-unlock module signing $(hostname)/" \
    -keyout mok.key -outform DER -out mok.der
chmod 600 mok.key
cd -
/usr/src/kernels/$(uname -r)/scripts/sign-file sha256 \
    ~/.local/share/gpu-unlock-mok/mok.key ~/.local/share/gpu-unlock-mok/mok.der \
    kernel/gpu_unlock/gpu_unlock.ko
modinfo kernel/gpu_unlock/gpu_unlock.ko | grep -E '^(signer|sig_key)'
```

Enroll the certificate. `mokutil` asks for a one-time password; on the next
boot the blue MOK Manager screen asks for it again (Enroll MOK → Continue →
Yes):

```sh
sudo mokutil --import ~/.local/share/gpu-unlock-mok/mok.der
```

After that boot, `mokutil --test-key ~/.local/share/gpu-unlock-mok/mok.der`
should say the key is already enrolled. Later rebuilds signed with the same
key need no new enrollment.

## 3. Package it as a local RPM

Save this as `gpu-unlock-local.spec` in the checkout. The two `%global` lines
matter: by default `rpmbuild` strips `.ko` files, and stripping removes the
appended signature, so the packaged module would be refused under Secure Boot
(`modinfo` then shows no `signer`).

```spec
# rpmbuild --define "kver <uname -r>" --define "kcore <kernel-core version-release>"
%global debug_package %{nil}
%global __os_install_post %{nil}

Name:     gpu-unlock-local
Version:  1.0.0
Release:  1%{?dist}
Summary:  Signed gpu_unlock and its firmware for kernel %{kver}
License:  GPL-2.0-only
BuildArch: x86_64
# The module only fits this kernel: an image update to a new kernel stops at
# this dependency until you rebuild, instead of booting without the unlock.
Requires: kernel-core = %{kcore}

%description
gpu_unlock built and signed locally for %{kver}, with the generated firmware.

%install
install -Dm644 %{_sourcedir}/gpu_unlock.ko %{buildroot}/usr/lib/modules/%{kver}/extra/gpu_unlock.ko
for f in booter_load_ga102.bin gsp_ga10x.bin gsp_boot_ga102.bin gsp_boot_desc_ga102.bin v67-ga102-610.43.03.bin; do
    install -Dm644 %{_sourcedir}/assets/$f %{buildroot}/usr/lib/firmware/gpu-unlock/610.43.03/$f
done
install -Dm644 %{_sourcedir}/dracut.conf %{buildroot}/usr/lib/dracut/dracut.conf.d/gpu-unlock.conf

%files
/usr/lib/modules/%{kver}/extra/gpu_unlock.ko
/usr/lib/firmware/gpu-unlock
/usr/lib/dracut/dracut.conf.d/gpu-unlock.conf
```

Build it in a Fedora container of the same release, so the host needs no
build tools (`fedora:44` here; use the release your image is based on):

```sh
mkdir -p rpm-src/assets
cp kernel/gpu_unlock/gpu_unlock.ko packaging/boot/dracut.conf rpm-src/
cp ~/.cache/gpu-unlock/assets/*.bin rpm-src/assets/
podman run --rm --security-opt label=disable -v "$PWD:/w" -w /w \
    -e KVER="$(uname -r)" -e KCORE="$(rpm -q --qf '%{VERSION}-%{RELEASE}' kernel-core)" \
    registry.fedoraproject.org/fedora:44 bash -c '
        dnf install -y -q rpm-build &&
        rpmbuild -bb --define "kver $KVER" --define "kcore $KCORE" \
            --define "_sourcedir /w/rpm-src" --define "_rpmdir /w/rpm-out" gpu-unlock-local.spec'
```

Check that the module inside the package is still signed:

```sh
rpm2cpio rpm-out/x86_64/gpu-unlock-local-*.rpm | cpio -i --quiet --to-stdout '*gpu_unlock.ko' > /tmp/check.ko
modinfo /tmp/check.ko | grep -E '^(signer|sig_key)'
```

## 4. Layer it and opt in

```sh
sudo rpm-ostree install ./rpm-out/x86_64/gpu-unlock-local-*.rpm
sudo rpm-ostree initramfs --enable          # rebuild the initramfs locally, so the dracut drop-in applies
sudo rpm-ostree kargs --append=gpu_unlock.targets=all
systemctl reboot
```

Most Atomic images boot a prebuilt initramfs. Without `rpm-ostree initramfs
--enable` the module is not in it, so it loads only after `nvidia` has bound
the card, and then does nothing. All three changes go into one new
deployment, so the previous deployment in the boot menu is the way back.

## 5. Check

```sh
journalctl -k -b | grep -E 'gpu_unlock|selector'
```

A working boot prints `selector write: SS0=0x88888888`, `selector write:
SS1=0x8` and `gpu_unlock: unlocked`, and `nvidia-smi` then shows the card as
usual. `Key was rejected by service` means the certificate is not enrolled
or the module lost its signature.

## Kernel updates and removal

The module is built for one kernel. When an image update brings a new
kernel, the `kernel-core` dependency stops the update. Then
`rpm-ostree uninstall gpu-unlock-local`, rebuild steps 1 to 3 for the new
kernel, and reinstall it in the same transaction. Do this from the updated
deployment, or with the new kernel's `kernel-devel` in a toolbox.

To stop unlocking, run `sudo rpm-ostree kargs --delete=gpu_unlock.targets=all`.
To remove the module as well, run `sudo rpm-ostree uninstall gpu-unlock-local`.
To also drop the local initramfs, run `sudo rpm-ostree initramfs --disable`.
