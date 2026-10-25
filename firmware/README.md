# Pinned 610.43.03 firmware

`make inputs` runs `fetch_artifacts.py`, which downloads the NVIDIA open kernel
module source archive and the exact CachyOS `nvidia-utils` package pinned in
`artifacts-610.43.03.json`. Downloads use HTTPS and are checked against their
SHA-256 before they enter the cache; cached files are rechecked on every run.
Nothing downloaded is executed or installed.

`make firmware` then builds the nine files the kernel module loads from
`gpu-unlock/610.43.03/`:

- `generate_v67.py` extracts shared `gsp_ga10x.bin` and the NVIDIA license
  from the package and generates the 64,000-byte `v67-ga102-610.43.03.bin`
  and `v67-ad102-610.43.03-four-plm.bin` payloads.
- `extract_boot_assets.py` builds `booter_load_ga102.bin`,
  `gsp_boot_ga102.bin` and `gsp_boot_desc_ga102.bin`, plus the corresponding
  `ad102` Booter, boot image and descriptor, from the source archive.

Supported Ada cards, including the explicitly gated experimental AD107M,
share the pinned AD102 asset profile. AD107 adds no firmware input or binary
payload and does not change the existing hashes, licenses or attribution.

Both scripts check every input and output against its pinned SHA-256 before
writing anything. Python 3.11+ and the `zstd` executable are required for the
input verification and `.zst` package extraction. See [NOTICE.md](NOTICE.md)
for provenance and licensing.
