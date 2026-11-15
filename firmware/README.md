# Pinned 610.43.03 firmware

`make inputs` downloads the two NVIDIA inputs into the cache.
`make firmware` runs `extract_gsp.py` for the two GSP ELFs and the NVIDIA
license, and `extract_boot_assets.py` for the signed SEC2 Booter bundles,
generic Falcon loader, and per-family GSP boot images/descriptors.
Extracted NVIDIA bytes are unchanged.

The payload is ordinary source code: the V67 call table in
`kernel/gpu_unlock/recipes.c`, built per write by `payload.c`. There is no
generator, no external `dmem.bin` override, and no reference image on disk.

The SEC2 Booter executes on Falcon and loads the separate GSP boot image.
GA100 uses `gsp_tu10x.bin` with `.fwsignature_ga100`; GA10x uses
`gsp_ga10x.bin` with `.fwsignature_ga10x`. The selected card determines its
GSP assets once, separately from register tables. All new hardware contexts
remain experimental; see [Stage 1 evidence](../docs/STAGE1.md).

Python 3, zstd, and the pinned inputs are sufficient for extraction.
See [NOTICE.md](NOTICE.md) for provenance and licensing.
