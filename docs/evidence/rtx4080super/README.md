# RTX 4080 SUPER (AD103) evidence

One card, one cold boot, driver 615.71.09 (open kernel module), firmware
inputs pinned to 610.43.03. Ubuntu 26.04, kernel 7.0.0-38-generic, module
signed with a locally enrolled MOK and loaded by dracut before `nvidia`.

- `boot-log.txt` — full transaction log of the successful boot
  (`gpu_unlock: unlocked`), the nvidia bind afterwards, and the
  enrolled-certificate load.
- `baseline.md` / `unlocked.md` — dense 8192² GEMM medians (7 samples,
  PyTorch 2.11.0+cu128) before and after, same card, same day.

Cold registers read by the module at entry (from the diagnostic run):
`FEAT_OVR_PLM = 0xffffff8f`, `SS0 = 0x25121025`, `SS1 = 0x7`, WPR2 down.

Hardware findings folded back into the port (each observed on this card):

1. A cold AD103 hands the GSP over with GFW **halted**; the pre-start
   HALT gate times out and a masked CPUCTL write does not clear it, so
   launches start over the halt (CPUCTL_START resumes it).
2. FWSEC sizes WPR2 to its own footprint — 0xE1 of the reserved 0x100
   FRTS pages — so the WPR2 high address is bounded, not pinned to
   `wpr_end`.
3. After FWSEC, SEC2 reads return a PRI-fault marker (0xbadf5620) until
   the launcher's engine reset, so nothing may probe SEC2 registers
   before the launch.
4. The board ROM's FWSEC DMEM length (0xd80) is not 256-byte aligned;
   Falcon DMA moves ceil(size/256) blocks.

The 4080 SUPER reaches 202.7 TFLOPS FP16 and 187.4 BF16 with FP32
accumulate (from 110.6/110.7 stock, power-limited sustained) and 92.5
TFLOPS TF32 (from 55.9); FP32 CUDA-core throughput is unchanged.
