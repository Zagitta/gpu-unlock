# Driver integration harness

Native runner for the `kernel/gpu_unlock` transaction against the GA102/AD102
device model. The driver, the kernel shim and the device library are compiled
into one shared object; Python only calls `gd_run_transaction` once per run and
decodes the trace afterwards.

## Run

```sh
python3 -m tests.integration.run GA10x --threads
GPU_UNLOCK_DEBUG_KEY=<32 hex chars> python3 -m tests.integration.test_integration
make integration-test
```

`GPU_UNLOCK_DEBUG_KEY` is required for the real path. The test executes the
real FWSEC and SEC2 Booter with the real V67 payload; the final clean-halt,
target-write and unlock checks currently fail and are reported as failures.

The runner reads firmware from `$GPU_UNLOCK_ASSETS` (default
`~/.cache/gpu-unlock/assets`) and prepares the committed VBIOS containers under
`emulator/vbios/`. `GPU_UNLOCK_DEBUG_KEY` (32 hex chars) supplies the external
AES-128 debug key; it is never written to the run directory.

The Booter bundle copied from the cache has HS-encrypted application code. For
keyed execution the harness replaces only that code range with the external
encrypted debug image (`$GPU_UNLOCK_BOOTERS`, default `~/Projects/booters`:
`ga100_secure.enc`, `ga102_secure.enc`, `ad102_secure.enc`). Header, DMEM data
and signature area remain the production bundle; the driver still loads one
ordinary driver-format bundle and performs the same secure load. The emulator
decrypts at the secure DMA/PIO boundary, and the Python code-table builder
performs the same AES-ECB transform. The debug image is not committed.

Outputs per run: `rom.bin`, `assets/`, `manifest.json`, `result.json`,
`trace.bin` and the decoded `trace.jsonl`.

## Stages

| Stage | State |
| --- | --- |
| 1. Host shim + end-to-end driver | Done. The driver parses the prepared ROM, patches FRTS, publishes the V67 signature and runs the real FWSEC/Booter transaction. |
| 2. Free-running engines | Done. SEC2/GSP pthreads, per-engine locks, a BAR lock, DMA queue draining and the FLR quiesce barrier. The real-path test runs with free-running engines; traces are not compared for equality. |
| 3. Execute FWSEC | Partial. Debug application selection (`GPU_UNLOCK_FWSEC_APP=0x45`), AES-128-ECB IMEM loading, a table-driven C interpreter, full-VBIOS `NVGI`/IFR scanning, the HS_UCODE_DMEM op4 SCP KDF, and the GSP SHA-256 operation/mutex path are implemented. With the supplied key the device executes the real FWSEC through the VBIOS `NVGI` check and SHA digest comparisons, then reaches and completes the DIO/SE PKA RSA path; the driver now reaches the real Booter. FWSEC and Booter each have their own pre-decoded instruction table, so the Booter no longer runs against FWSEC records. Neither engine reaches a clean halt yet (`fwsec_executed=false`, `booter_executed=false`). The secure-bus window, GSP identity values, AES/SHA registers, SE PKA modexp and the WPR2-visible register path are modeled. |
| 4. Hooks and tracing | Done for the actual `booter.toml` inventory (storage, the two compiled CSB operations) with binary per-run trace rings and Python decoding. |
| 5. Behavioral gates | Partial. GA10x real-path execution is asserted; the clean-halt/target/unlock gates fail with the current payload. |

## Verdicts

`result.json` now separates:

- `driver_result` — the driver's return value.
- `fwsec_executed`, `booter_executed` — real instruction execution only.
- `plm_opened`, `target_written` — latched before FLR, not post-reset state.
- `unlock_verified` — true only when the real FWSEC and Booter ran, the PLM
  opened, the target mask was written, both selectors read back, and SEC2 left
  `RESET_PLM=0xffffffff`.

PLM enforcement is active: a host write to a protected target is denied while
its mask still has the matching write level closed. The real firmware performs
the privileged writes and then must pass the same host-side gate.

The real keyed run asserts `unlock_verified`. It currently reaches the Booter,
but the V67 chain faults at `pc=1` before the clean halt and target write, so
the integration test reports failures rather than a synthesized success.

## Deliberate limits

- Production firmware remains unexecutable. The real path requires the external
  debug FWSEC/Booter assets and a complete Falcon core; neither is in this repository.
- The current V67 payload does not reach a clean Booter halt; the final gates fail.
- GA100 stays blocked by the v2 descriptor gap described in
  `emulator/PLAN_GSP_FWSEC.md`.
- Traces are not compared for equality. Acceptance uses driver return values,
  execution evidence, halt/fault PCs, WPR2 state and unknown-register evidence.
