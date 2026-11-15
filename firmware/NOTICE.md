# Firmware provenance and distribution scope

`gsp_ga10x.bin` and `gsp_tu10x.bin` are extracted byte-for-byte from the NVIDIA
610.43.03 package by `extract_gsp.py`. The Booter and GSP boot images
are decoded unchanged from the pinned 610.43.03 source archive. The extraction
also preserves that package's complete `usr/share/licenses/nvidia-utils/LICENSE`
as `NVIDIA-LICENSE`. It is installed with the firmware package.

The source archive's open-module licensing does **not** license NVIDIA firmware.
The binary package contains the NVIDIA Driver License Agreement. Its section
1.1(d) addresses distribution for open source operating-system kernels with
unchanged binary files and inclusion of the agreement. Additional limitations
apply, including NVIDIA-platform use. Personal local packaging is the initial
delivery target. Review the full pinned agreement and the separately generated
payload before any public distribution; redistribution approval is not claimed.

The GA102 re-entrant payload is derived from the live 610.43.03 Booter
behavior (its OS-block validation, PLM pair writes and exit path), not from
the cmpunlocker template. The Stage 1 reference checkout,
amoghmunikote/cmpunlocker at commit
`2d10f891a58824f0591c3332c4eccf37ff36b086`, is the recorded origin of the
initial payload constants and PLM group. Its repository license is GPL version
2, preserved in `licenses/GPL-2.0.txt`. The payload is source code in this
repository, separate from the unchanged NVIDIA firmware.

NVIDIA source files inspected during the loader audit carry their own MIT
copyright notices. The minimum helper's metadata layout and Falcon operations
follow those source contracts; `licenses/NVIDIA-MIT.txt` preserves the notice.
The inspected EFI reference's notice is in `licenses/CMP40HX-MIT.txt`.
The payload reference targets GA100 CMP 170HX. It does not establish GA10x
firmware acceptance, standalone multiwrite, final FLR retention or stock-driver
handover. Those require the Stage 1 hardware experiments.

The Linux loading geometry, register sequence and success gates are ported from
[elias-gaeros/gpu-unlock's RTX 4090 work](https://github.com/elias-gaeros/gpu-unlock/blob/rtx4090/rtx4090-register-report.md).
That report records one successful RTX 4090 production-booter transaction.
A hardware repeat of the refactored path is still needed. AD102 uses the same per-entry signature image as GA10x. Its
privileged stores are WPR_CFG, WPR and FEAT.
