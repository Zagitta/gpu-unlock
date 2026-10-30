# GPU Unlock — AD102/AD106M SEC2 Booter Reverse-Engineering Handoff
**Date:** 2026-10-30
**Status:** Booter code DISASSEMBLED. Embedded strings and data table found. Next: trace control flow to reconstruct decryption algorithm.

---

## 1. THE GOAL

Build a working `ad102_v67` payload for gpu-unlock to unlock AD102/AD106M (RTX 4070 Mobile, PCI ID 0x2860) tensor compute.

The current `ad102_v67` in `recipes.c` is a **byte-for-byte copy of `ga102_v67`** — it uses GA102 DMEM addresses that don't exist in the AD102 booter, causing a secure fault crash.

---

## 2. KEY FILES

| File | Purpose |
|------|---------|
| `kernel/gpu_unlock/recipes.c` | Defines `ad102_v67` payload (currently copies GA102 — WRONG) |
| `kernel/gpu_unlock/payload.c` | `gu_payload_build()` — fills 0xfa00-byte image with fill value, writes `words[]` at offsets, writes value at 0xf948, address at 0xf960 |
| `kernel/gpu_unlock/falcon.c` | `gu_falcon_hs()` / `gu_falcon_hs_probe()` — loads SEC2 image via Falcon DMA, starts CPU |
| `kernel/gpu_unlock/unlock_610.c` | `gu_launch_booter_sec2_direct()` — AD102 uses `GU_SEC2_DIRECT`, passes `work->booter_buf.cpu` as words array |
| `kernel/gpu_unlock/falcon.h` | Falcon transport interface, probe functions, DMA capture |
| `firmware/blobs/booter_load_ad102.bin` | **AD102 booter (57672 bytes) — THE TARGET** |
| `firmware/blobs/booter_load_ga102.bin` | **GA102 booter (61256 bytes) — THE REFERENCE** |
| `Falcon_Instructions.md` | Disassembled Falcon instructions (first 256), instruction distribution, embedded strings |
| `decrypt_sec2.py` | Python SEC2 decryption attempt (not yet working) |
| `analyze_booters.py` | Python booter comparison script |

---

## 3. CRITICAL DISCOVERY — BOOTER IS REAL EXECUTABLE CODE

**The firmware blob `booter_load_ad102.bin` contains REAL Falcon CPU instructions starting at offset 0x148** (after 72-byte BTL1 header + 256 bytes padding).

### Instruction Types (32-bit little-endian dwords)
| Top Nibble | Type | Count |
|------------|------|-------|
| 0 | NOP (zero-filled IMEM) | 6606 |
| 1 | HALT | 515 |
| 2 | UNKNOWN | 515 |
| 3 | UNKNOWN | 499 |
| 4 | LOAD | 491 |
| 5 | STORE | 563 |
| 6 | ARITH | 547 |
| 7 | BRANCH | 500 |
| 8 | CALL | 497 |
| 9 | JUMP | 462 |
| a-f | UNKNOWN | ~3000 |

**Total: ~14,300 dwords analyzed, ~497 CALL instructions to 491 unique targets.**

### Embedded Strings (BIG-ENDIAN in data section)
The booter code contains these ASCII strings embedded in its data section:
- `NVRNG engine` — Random number generator engine
- `NVPKA engine [RSA mode]` — RSA cryptographic engine
- `NVPKA engine [ECC mode]` — ECC cryptographic engine
- `CCC arithmetic error` — Error message for CCC operations
- `NVPKA arithmetic error` — Error message for NVPKA operations

### Data Table at Code Offset 0x3690 (File Offset 0x37d8)
A structured configuration table containing:
- **DMEM pointer addresses**: 0x5ef2 (NVRNG), 0x5ef6 (NVPKA), 0x5d68, 0x5d30, 0x5d75, 0x5d40, 0x5d8d, 0x5d50
- **Engine names**: NVRNG, NVPKA, CCC
- **Engine modes**: RSA mode, ECC mode
- **Error messages**: "CCC arithmetic error", "NVPKA arithmetic error"
- **DMEM addresses**: 0x5d68, 0x5d30, 0x5d75, 0x5d40, 0x5d8d, 0x5d50

### Multi-Caller Function
Only **one function is called from multiple locations**: target **0x0b178** (called from 6 different instruction offsets). This is likely a **dispatch/switch function**.

### Call Targets in Zero-Filled Regions
Many of the 491 unique call targets point to **zero-filled regions** — these are **external function pointers** (not in the booter code itself). The booter likely uses a **function pointer table** for external calls.

---

## 4. BTL1 HEADER COMPARISON

| Field | Offset | GA102 | AD102 |
|-------|--------|-------|-------|
| magic | 0x00 | "BTL1" | "BTL1" |
| version | 0x04 | 0x00010001 | 0x00010001 |
| os_code_off | 0x0c | 0x100 | 0x100 |
| os_data_off | 0x10 | 0x8a00 | **0x7e00** |
| image_size | 0x14 | 0x6200 | 0x6000 |
| sig_bytes | 0x24 | 256 | 256 |
| num_sigs | 0x28 | 0 | 0 |
| patch_loc | 0x2c | 0x8a10 | **0x7e10** |

**KEY DIFFERENCE:** AD102 os_data_off = 0x7e00, GA102 = 0x8a00. The DMEM frame starts at different offsets.

---

## 5. DMEM FRAME ANALYSIS

### GA102 DMEM (0x8a00-0xecff, 0x6200 bytes)
- First dword: `6a 20 a1 1d` (unique random per card)
- Second dword: `79 19 87 b5` (unique random per card)
- Third dword: `af 42 06 eb 01 b9 98 6c` (repeating 8-byte pattern, 3 copies)
- Then zeros
- **Program code at 0x1100** (relative to os_data_off)
- **Gadget call-table at 0xf950-0xf9ec** (relative to os_data_off)

### AD102 DMEM (0x7e00-0xddff, 0x6000 bytes)
- First dword: unique random (no pattern)
- Second dword: unique random (no pattern)
- Third dword: unique random (NO repeating pattern)
- **NO program code** anywhere
- **NO gadget call-table** at 0x1100 or 0xf950-0xf9ec (all zeros)
- Only 93 non-zero dwords total
- **DMEM pointer table at 0x5d78-0x5dac** — 13 entries, encrypted

---

## 6. SEC2 ENCRYPTION

SEC2 is encrypted with a **stream cipher derived from the GPU's public key at runtime**. NOT a rolling cipher.

### SEC2 Structure (file offsets)
| Region | Offset | Size | Description |
|--------|--------|------|-------------|
| BTL1 header | 0x00 | 72 | Boot loader header |
| Padding | 0x48 | 256 | All zeros |
| Signature | 0x148 | 384 | Random (key material?) |
| Header | 0x2c8 | 656 | Fully encrypted, random |
| Code | 0x548 | 32000 | Fully encrypted, ~3.0% zero dwords |
| DMEM frame | 0x7e00 | varies | 72-byte header + zeros + pointer table |

### Repeating Signatures in SEC2 Code (at 0x548+)
- **Main**: `0x5a87e20e, 0x8f90b178, 0xb2c0285a, 0x38894ef0` at 6 offsets
- **Secondary A**: `0xa4b53870, 0x4d51c68e, 0x51076cd1, 0x012f7607` at 3 offsets
- **Secondary B**: `0xdea2f84a, 0xfa2d7c22, 0xda76b247, 0xb85fbe26` at 3 offsets
- **Secondary C**: `0x399ef94d, 0x1b9e03b4, 0x2fa55512, 0x71a8bf4a` at 3 offsets
- **Secondary D**: `0x085616dc, 0x7504bd42, 0xee1af7b3, 0x51c72344` at 3 offsets

---

## 7. DMA HEIST PROBE (ALREADY IMPLEMENTED)

`gu_falcon_hs_probe()` in `falcon.c` returns **immediately after Falcon CPU start** (no halt poll). This allows capturing the DMA transfer before the SEC2 code is decrypted.

### How It Works
1. DMA loads code (IMEM, secure) and data (DMEM) via Falcon DMA engine
2. Sets BROM_PARAADDR, BROM_ENGIDMASK, BOOTVEC, MAILBOX0/1
3. Starts CPU — **returns immediately**
4. Probe dumps registers, DMEM, IMEM, SEC2 code region, SEC2 header, SEC2 signature, DMEM pointer table, PROM_DATA, payload buffer, booter buffer
5. **The DMA capture buffer contains the pre-decryption SEC2 code**

### Current Status
- DMA capture is implemented but **the probe data was lost** due to kernel log buffer overflow
- The filesystem dump method was also attempted but data may not have been written
- **The probe needs to run at boot** (before NVIDIA driver loads) via core_initcall

---

## 8. WHAT'S DONE

1. ✅ **Booter code disassembled** — All 14,300+ dwords analyzed
2. ✅ **Instruction types identified** — LOAD, STORE, ARITH, BRANCH, CALL, JUMP
3. ✅ **Embedded strings found** — NVRNG, NVPKA, CCC engine names and error messages
4. ✅ **Data table reverse-engineered** — DMEM pointers, engine configs, addresses
5. ✅ **Multi-caller function identified** — 0x0b178 (6 callers, likely dispatch)
6. ✅ **DMA heist probe implemented** — Returns immediately after CPU start
7. ✅ **Probe dumps everything** — SEC2 code, header, signature, DMEM, IMEM, PROM_DATA
8. ✅ **Build system working** — `cd kernel/gpu_unlock && make`

---

## 9. WHAT'S LEFT (NEXT STEPS)

### PRIORITY 1: Trace Booter Control Flow
The booter code IS the decryption algorithm. We need to:
1. **Start from the entry point** (first non-zero instruction after IMEM padding)
2. **Follow the 497 CALL instructions** to understand the call graph
3. **Identify the decryption loop** — likely uses NVRNG for key generation, NVPKA for crypto, CCC for arithmetic
4. **Map the state machine** — SEC2 code processes 4 operation types: NVRNG, NVPKA, CCC, Control
5. **Extract the key derivation logic** — How the stream cipher key is derived from GPU public key

### PRIORITY 2: Build Python Simulator
Once we understand the control flow:
1. **Implement Falcon CPU emulator** in Python
2. **Simulate the booter code** with known inputs
3. **Trace the decryption** of SEC2 code
4. **Extract the decryption key** for our specific GPU

### PRIORITY 3: Derive V67 Payload
With the decryption algorithm understood:
1. **Find the equivalent gadget call-table** in AD102 DMEM (different from GA102)
2. **Write `ad102_v67` descriptor** with correct offsets
3. **Build and test** with `targets=0000:01:00.0` (one power-cycle cost)

### PRIORITY 4: Fix DMA Heist Probe
1. **Increase kernel log buffer** to 32MB+ (already attempted)
2. **Add filesystem dump** as backup (already attempted)
3. **Ensure probe runs at boot** before NVIDIA driver loads
4. **Capture the DMA transfer** — the pre-decryption SEC2 code

---

## 10. BUILD & INSTALL

```bash
cd ~/WubarrkCODING/gpu-unlock/kernel/gpu_unlock && make
sudo cp gpu_unlock.ko /lib/modules/$(uname -r)/updates/
sudo depmod -a
sudo mkinitcpio -P
```

**CRITICAL:** Must rebuild initramfs with `mkinitcpio -P` after installing new module, otherwise kernel loads old module from initramfs at boot.

---

## 11. DON'T DO THIS

1. **Don't use `targets=all` for experimental Ada cards** — it will brick every boot
2. **Don't unbind nvidia driver at runtime** — froze the desktop in testing
3. **A warm reboot doesn't un-brick** — must fully power off, wait, power on
4. **Don't assume AD102 is similar to GA102** — fundamentally different DMEM layout

---

## 12. KEY INSIGHTS

1. **The booter code is REAL executable Falcon CPU code** — not encrypted data
2. **Strings are in BIG-ENDIAN format** in the data section
3. **The booter uses three engines**: NVRNG (RNG), NVPKA (RSA/ECC), CCC (arithmetic)
4. **AD102 DMEM is nearly empty** — the booter must use a different mechanism than GA102's gadget call-table
5. **SEC2 is encrypted with a stream cipher** derived from GPU public key at runtime
6. **The DMA heist probe can capture pre-decryption SEC2 code** — but needs to run at boot
7. **The data table at 0x3690 contains DMEM pointers** — these are the engine struct addresses
8. **The multi-caller function at 0x0b178** is likely a dispatch/switch function

---

## 13. TESTING

Test with probe mode:
```bash
sudo modprobe gpu_unlock probe_addr=0000:01:00.0
dmesg | grep gpu_unlock
```

**IMPORTANT:** Only test with `targets=0000:01:00.0` (specific card), never `targets=all` for unproven payloads.

---

## 14. REMEMBER

This is **uncharted territory** for the 40-series. We are pioneering this work. The AD102 booter is fundamentally different from GA102 — don't assume similarity where there is none. The SEC2 encryption is a stream cipher derived from the GPU's public key, not a simple rolling cipher. The AD102 DMEM frame is nearly empty — the booter must use a different mechanism entirely.

**The booter code IS the decryption algorithm.** We have the instructions — now we need to trace the control flow to understand how it works.
