# GPU unlock 1.0.0 packaging candidate

This Linux kernel module combines the CMP unlocker's V67/PLM technique with the
pre-driver timing of the UEFI approach. It runs the unlock, writes
the speed selectors (SS0 `0x88888888`, which selects the full BF16 rate, and
the 4-bit SS1 `0x8`, whose effect is untested) and performs FLR before the unmodified
NVIDIA driver binds the card.
It is neither a UEFI application nor a patched NVIDIA driver.

The observed result is GPU1, an RTX 3090 at `0000:21:00.0`, on the main
CachyOS `7.2.8-1-cachyos` kernel with NVIDIA `615.71.09`. GPU0 at
`0000:01:00.0` was an unmodified control. Other RTX 3090 boards and
environments have not been verified. GeForce and CMP GA102/GA104 PCI device
IDs are accepted; professional RTX A-series and data-centre parts are not.
No GA104 card and no GA102 card other than the RTX 3090 has been tested; the
firmware and V67 image are the GA102 ones. Other chips are rejected.

## Local build

Every build downloads the pinned NVIDIA/CachyOS inputs over HTTPS, checks
their SHA-256, and generates firmware only in the build directory and the
binary package. All three formats install through the same `make install-*`
targets. Packages are built from the committed `HEAD`.

Build, install build dependencies, and install the packages in one step
(asks for your sudo password):

```sh
make install-arch   # makepkg -sif
make install-deb    # apt-get build-dep, dpkg-buildpackage, apt-get install
make install-rpm    # dnf builddep, rpmbuild, dnf install
```

`make arch`, `make deb` and `make rpm` only build. Debian and RPM packages go
to `build/deb` and `build/rpm`. The NVIDIA inputs are cached in
`~/.cache/gpu-unlock/inputs` (`make inputs`; override with `INPUTS=`).
`dpkg-buildpackage` itself never uses the network, so a clean-chroot build (sbuild or
pbuilder) needs `GPU_UNLOCK_INPUTS` pointing at that cache.
The `make dist` archive, the Debian source package and the `.nosrc.rpm`
contain no NVIDIA archives or generated firmware.

## Boot flow and opt-in

The packages put `gpu_unlock` and its firmware in the initramfs and load it
there, before `nvidia` can bind a card:

- **mkinitcpio:** `/etc/mkinitcpio.conf.d/zz-gpu-unlock.conf` inserts
  `gpu_unlock` right before `nvidia` in `MODULES`, so a display card is blank
  for as short a time as possible. Without `nvidia` in the image, it goes
  last and still runs before an `nvidia` loaded from the root filesystem.
  Images are rebuilt on install. mkinitcpio ignores drop-ins for an image
  built with `-c <config>` or a preset `_config`; put `gpu_unlock` right
  before `nvidia` in that config's `MODULES` yourself.
- **initramfs-tools:** `/usr/share/initramfs-tools/modules.d/gpu-unlock.conf`.
- **dracut:** `/usr/lib/dracut/dracut.conf.d/gpu-unlock.conf`. The packages
  rebuild the running kernel's initramfs on install and removal; new
  kernels pick the drop-in up automatically when they are installed.

There is no `softdep nvidia pre:` rule: kmod honours only the first
`softdep nvidia` line it reads, so other packages' rules (`nvidia-utils`,
`gddr6_hwmon`) would silently override it, or it would override theirs.

The module does nothing unless the kernel command line opts in:

```
gpu_unlock.targets=all                          # every unbound GA102/GA104 card
gpu_unlock.targets=0000:21:00.0                 # only these cards
gpu_unlock.targets=0000:21:00.0,0000:22:00.0    # comma-separated
```

Addresses are full PCI names as `lspci -D` prints them (hex digits in either
case), independent of GPU numbering and display role. An entry that is not
a GA102/GA104 card, including `all` mixed with addresses, is logged and ignored.
The cards are unlocked concurrently, about a second in all however many
there are, and each on its own: a card that is already bound or powered
down is skipped. Bus mastering that the board firmware left on (Supermicro
X11 does this whatever its option ROM and bus master settings) is cleared
first; a card some driver already initialised is still refused. Then
`nvidia` loads and binds every card as usual.

The display card works too. Its firmware framebuffer (the boot console and
Plymouth) keeps running while the unlock's firmware steps run, and is
removed just before the card's reset, as `nvidia-drm` would do a moment
later anyway. Its screen is blank from that reset until `nvidia-drm` takes
over.

The unlock needs a cold card: power the machine off rather than rebooting.
Warm reboots are not supported; with the option set, the unlock is expected
to fail and leave the card as it was. Loading the module by hand after boot
does nothing to a card that `nvidia` has bound. A card that a driver already
used this power cycle is refused before any firmware runs (`card already
initialised since power on`).

Check the result with:

```sh
journalctl -k -b | grep gpu_unlock
lspci -nnk -s 0000:21:00.0
```

To stop unlocking, remove `gpu_unlock.targets=` from the kernel command line;
for a one-off boot, delete it in the bootloader's edit screen. Removing the
packages also removes the module and the initramfs drop-ins.

If the firmware on a card never stops, the module logs it and keeps the DMA
buffers that firmware may still use until the next power off. Power off
before trying again.

The kernel module and local packaging are GPL-2.0-only. The generated payload
derives from the pinned CMP reference; NVIDIA firmware remains under its own
terms. See `firmware/NOTICE.md` and `licenses/` for attribution and notices.


# Support me

Help me buy more GPUs <3

https://buymeacoffee.com/zagitta
ETH: 0xf4eee937937f92038D70B16AbD7736D8a53a1c5c
BTC: bc1qwpkwp70fn76z3s4kg2l9y3j4kst95twcznuugf
SOL: 8Rih6HY7V4LcdjYHz4iSffT4fTniqw5CACaBigSVxNYu
