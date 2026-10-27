// SPDX-License-Identifier: GPL-2.0-only
/* Falcon register transport. Firmware policy and ownership belong to the caller. */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/device.h>
#include <linux/string.h>
#include <linux/timekeeping.h>

#include "falcon.h"
#include "prep_610.h"
#include "registers.h"

static int falcon_read(const struct gu_falcon_io *io, u32 offset, u32 *value)
{
	return io->read(io->context, offset, value);
}
static void falcon_write(const struct gu_falcon_io *io, u32 offset, u32 value)
{
	io->write(io->context, offset, value);
}
static bool inaccessible(u32 value)
{
	return value == ~0u || (value & 0xffff0000u) == 0xbadf0000u;
}

int gu_falcon_poll(const struct gu_falcon_io *io, u32 offset, u32 mask, u32 expect,
		   unsigned int budget_us, bool timeout_is_failure, u32 *last)
{
	ktime_t deadline = ktime_add_us(ktime_get(), budget_us);
	u32 value = 0;

	for (;;) {
		int ret = falcon_read(io, offset, &value);

		if (last)
			*last = value;
		if (ret || value == ~0u ||
		    (value & 0xffff0000u) == 0xbadf0000u) {
			dev_err(io->dev, "poll inaccessible: reg=%#x last=%#x mask=%#x expected=%#x budget_us=%u\n",
				offset, value, mask, expect, budget_us);
			return -EIO;
		}
		if ((value & mask) == expect)
			return 0;
		if (ktime_compare(ktime_get(), deadline) >= 0) {
			/* Console loglevel hides info. A tolerated timeout must not
			 * look like the failure that stopped the boot. */
			if (!timeout_is_failure) {
				dev_dbg(io->dev, "poll timeout tolerated: reg=%#x last=%#x mask=%#x expected=%#x budget_us=%u\n",
					offset, value, mask, expect, budget_us);
				return -ETIMEDOUT;
			}
			dev_err(io->dev, "poll timeout: reg=%#x last=%#x mask=%#x expected=%#x budget_us=%u\n",
				offset, value, mask, expect, budget_us);
			return -ETIMEDOUT;
		}
		usleep_range(50, 100);
	}
}

int gu_falcon_engine_reset(const struct gu_falcon_io *io, u32 base,
			   const char *phase, u32 *hwcfg_out,
			   struct gu_falcon_evidence *evidence)
{
	unsigned int j;
	u32 engine, before, first = 0, hwcfg, last = 0;
	int ret;

	memset(evidence, 0, sizeof(*evidence));
	if (falcon_read(io, base + GU_FALCON_ENGINE_OFFSET, &engine) || inaccessible(engine))
		return -ENODEV;
	before = engine;
	dev_dbg(io->dev, "engine reset assert: phase=%s base=%#x before=%#x write=%#x\n",
		 phase, base, before, before | 1);
	falcon_write(io, base + GU_FALCON_ENGINE_OFFSET, engine | 1);
	/* 610 ksec2ResetHw_TU102 requires ten register cycles for signal
	 * propagation. A stale zero during those cycles is not evidence that
	 * the assertion failed. No CPU start and no reset retry.
	 */
	for (j = 0; j < 10; j++) {
		if (falcon_read(io, base + GU_FALCON_ENGINE_OFFSET, &engine) ||
		    inaccessible(engine)) {
			dev_err(io->dev, "engine reset inaccessible: phase=%s base=%#x cycle=%u value=%#x\n",
				phase, base, j + 1, engine);
			return -EIO;
		}
		if (!j)
			first = engine;
	}
	dev_dbg(io->dev, "engine reset propagation: phase=%s base=%#x first=%#x tenth=%#x\n",
		 phase, base, first, engine);
	ret = gu_falcon_poll(io, base + GU_FALCON_ENGINE_OFFSET, 1, 1, 1000, true, &last);
	dev_dbg(io->dev, "engine reset asserted: phase=%s base=%#x error=%d last=%#x\n",
		 phase, base, ret, last);
	if (ret)
		return ret;
	evidence->reset_asserted = true;
	if (falcon_read(io, base + GU_FALCON_ENGINE_OFFSET, &engine) || inaccessible(engine))
		return -EIO;
	dev_dbg(io->dev, "engine reset release: phase=%s base=%#x before=%#x write=%#x\n",
		 phase, base, engine, engine & ~1u);
	falcon_write(io, base + GU_FALCON_ENGINE_OFFSET, engine & ~1u);
	for (j = 0; j < 10; j++) {
		if (falcon_read(io, base + GU_FALCON_ENGINE_OFFSET, &engine) ||
		    inaccessible(engine)) {
			dev_err(io->dev, "engine reset release inaccessible: phase=%s base=%#x cycle=%u value=%#x\n",
				phase, base, j + 1, engine);
			return -EIO;
		}
	}
	ret = gu_falcon_poll(io, base + GU_FALCON_ENGINE_OFFSET, 1, 0, 1000, true, NULL);
	if (ret)
		return ret;
	evidence->reset_released = true;
	/* FALCON_HWCFG2 bit 12 clears when memory scrubbing finishes.
	 * A PRI error fails this wait instead of being handed to RM.
	 */
	ret = gu_falcon_poll(io, base + GU_FALCON_HWCFG2_OFFSET, 1u << 12, 0, 1000000, true, NULL);
	if (ret)
		return ret;
	if (falcon_read(io, base + GU_FALCON_HWCFG2_OFFSET, &hwcfg) || inaccessible(hwcfg))
		return -EIO;
	dev_dbg(io->dev, "engine reset released: phase=%s base=%#x engine=%#x hwcfg2=%#x\n",
		 phase, base, engine, hwcfg);
	if (hwcfg_out)
		*hwcfg_out = hwcfg;
	return 0;
}

int gu_falcon_reset(const struct gu_falcon_io *io, const struct gu_falcon_engine *eng,
		    struct gu_falcon_evidence *evidence)
{
	u32 hwcfg, bcr, chip;
	int ret;

	memset(evidence, 0, sizeof(*evidence));
	/* 610 kflcnPreResetWait_GA102 permits this timeout (HW bug 3419321). */
	dev_dbg(io->dev, "Falcon reset begin: base=%#x\n", eng->base);
	ret = gu_falcon_poll(io, eng->base + GU_FALCON_HWCFG2_OFFSET,
			     1u << 31, 1u << 31, 1000, false, NULL);
	if (ret && ret != -ETIMEDOUT)
		return ret;
	if (ret)
		dev_dbg(io->dev, "Falcon pre-reset timeout tolerated per HW bug 3419321\n");
	ret = gu_falcon_engine_reset(io, eng->base, "pre-launch", &hwcfg, evidence);
	if (ret)
		return ret;
	if (hwcfg & (1u << 10)) {
		if (falcon_read(io, eng->falcon2 + GU_RISCV_BCR_CTRL_OFFSET, &bcr) || inaccessible(bcr))
			return -ENODEV;
		if (bcr & (1u << 4)) {
			falcon_write(io, eng->falcon2 + GU_RISCV_BCR_CTRL_OFFSET, 0);
			ret = gu_falcon_poll(io, eng->falcon2 + GU_RISCV_BCR_CTRL_OFFSET,
					     0x11, 1, 1000000, true, NULL);
			if (ret)
				return ret;
		}
	}
	if (falcon_read(io, GU_PMC_BOOT_0_OFFSET, &chip) || inaccessible(chip))
		return -ENODEV;
	falcon_write(io, eng->base + GU_FALCON_RM_OFFSET, chip);
	dev_dbg(io->dev, "Falcon reset done: base=%#x hwcfg=%#x chip=%#x\n", eng->base, hwcfg, chip);
	return 0;
}

/* DMATRFMOFFS is 24 bits, FBOFFS is 32 bits, and BASE is 49 bits.
 * Reject truncation/wrap before submitting any part of a transfer. */
static bool falcon_dma_valid(u32 dest, u32 mem_off, dma_addr_t src, u32 size)
{
	u64 rounded = ((u64)size + 255) & ~255ull;
	return size && !((dest | mem_off | src) & 255) &&
		(u64)dest + rounded <= (1ull << 24) &&
		(u64)mem_off + rounded <= (1ull << 32) &&
		src < (1ull << 49) &&
		(u64)mem_off + rounded <= (1ull << 49) - src;
}

int gu_falcon_dma(const struct gu_falcon_io *io, u32 base, u32 dest, u32 mem_off,
		      dma_addr_t src, u32 size, u32 cmd)
{
	u32 done = 0;
	int ret;

	dev_dbg(io->dev, "Falcon DMA: base=%#x dest=%#x offset=%#x src=%#llx size=%#x cmd=%#x\n",
		base, dest, mem_off, (unsigned long long)src, size, cmd);
	if (!falcon_dma_valid(dest, mem_off, src, size))
		return -EINVAL;
	ret = gu_falcon_poll(io, base + GU_FALCON_DMATRFCMD_OFFSET, 1, 0, 1000000, true, NULL);
	if (ret)
		return ret;
	falcon_write(io, base + GU_FALCON_DMATRFBASE_OFFSET, lower_32_bits(src >> 8));
	falcon_write(io, base + GU_FALCON_DMATRFBASE1_OFFSET, upper_32_bits(src >> 8) & 0x1ff);
	while (done < size) {
		ret = gu_falcon_poll(io, base + GU_FALCON_DMATRFCMD_OFFSET, 1, 0, 1000000, true, NULL);
		if (ret) {
			dev_err(io->dev, "Falcon DMA progress: done=%u total=%u\n", done, size);
			return ret;
		}
		falcon_write(io, base + GU_FALCON_DMATRFMOFFS_OFFSET, dest);
		falcon_write(io, base + GU_FALCON_DMATRFFBOFFS_OFFSET, mem_off);
		falcon_write(io, base + GU_FALCON_DMATRFCMD_OFFSET, cmd);
		done += 256;
		dest += 256;
		mem_off += 256;
	}
	return gu_falcon_poll(io, base + GU_FALCON_DMATRFCMD_OFFSET, 2, 2, 1000000, true, NULL);
}

int gu_falcon_hs(const struct gu_falcon_io *io, const struct gu_falcon_engine *eng,
		 const struct gu_falcon_hs_image *program,
		 struct gu_falcon_evidence *evidence)
{
	u32 trans, cpuctl;
	dma_addr_t code_src;
	int ret;

	memset(evidence, 0, sizeof(*evidence));
	if (program->dma > ~0ull - program->code_off ||
	    program->dma + program->code_off < program->code_va ||
	    program->dma > ~0ull - program->data_off)
		return -ERANGE;
	code_src = program->dma + program->code_off - program->code_va;
	if (!falcon_dma_valid(program->code_pa, program->code_va, code_src, program->code_size) ||
	    (program->data_size && !falcon_dma_valid(program->data_pa, 0,
						  program->dma + program->data_off, program->data_size)))
		return -EINVAL;
	ret = gu_falcon_reset(io, eng, evidence);
	if (ret)
		return ret;
	ret = gu_falcon_poll(io, eng->base + GU_FALCON_CPUCTL_OFFSET, 16, 0, 1000,
			     !program->stale_halt_ok, NULL);
	if (ret && !(ret == -ETIMEDOUT && program->stale_halt_ok))
		return ret;
	if (falcon_read(io, eng->fbif + GU_FBIF_CTL_OFFSET, &trans) || inaccessible(trans))
		return -EIO;
	trans |= 1u << 7;
	falcon_write(io, eng->fbif + GU_FBIF_CTL_OFFSET, trans);
	falcon_write(io, eng->base + GU_FALCON_DMACTL_OFFSET, 0);
	if (falcon_read(io, eng->fbif + GU_FBIF_TRANSCFG(0), &trans) || inaccessible(trans))
		return -ENODEV;
	trans = (trans & ~7u) | 5u;
	falcon_write(io, eng->fbif + GU_FBIF_TRANSCFG(0), trans);
	dma_wmb();
	if (program->os_code_size) {
		ret = gu_falcon_dma(io, eng->base, 0, 0,
				    program->dma + program->os_code_off, program->os_code_size,
				    gpu_unlock_dma_cmd(true, false));
		if (ret)
			return ret;
	}
	ret = gu_falcon_dma(io, eng->base, program->code_pa, program->code_va, code_src,
			 program->code_size, gpu_unlock_dma_cmd(true, true));
	if (ret)
		return ret;
	if (program->data_size) {
		ret = gu_falcon_dma(io, eng->base, program->data_pa, 0,
				    program->dma + program->data_off, program->data_size,
				    gpu_unlock_dma_cmd(false, false));
		if (ret)
			return ret;
	}
	evidence->reloaded = true;
	falcon_write(io, eng->falcon2 + GU_FALCON2_BROM_PARAADDR_OFFSET, program->sig_dmem);
	falcon_write(io, eng->falcon2 + GU_FALCON2_BROM_ENGIDMASK_OFFSET, program->engine_mask);
	falcon_write(io, eng->falcon2 + GU_FALCON2_BROM_CURR_UCODE_ID_OFFSET, program->ucode_id & 0xff);
	falcon_write(io, eng->falcon2 + GU_FALCON2_MOD_SEL_OFFSET, 1);
	falcon_write(io, eng->base + GU_FALCON_BOOTVEC_OFFSET, program->code_va);
	falcon_write(io, eng->base + GU_FALCON_MAILBOX0_OFFSET, program->mailbox0);
	falcon_write(io, eng->base + GU_FALCON_MAILBOX1_OFFSET, program->mailbox1);
	if (falcon_read(io, eng->base + GU_FALCON_CPUCTL_OFFSET, &cpuctl) || inaccessible(cpuctl))
		return -EIO;
	dev_dbg(io->dev, "Falcon start: base=%#x cpuctl=%#x code_va=%#x sig_dmem=%#x engine_mask=%#x ucode=%u mailbox=%#x:%#x\n",
		eng->base, cpuctl, program->code_va, program->sig_dmem, program->engine_mask,
		program->ucode_id, program->mailbox1, program->mailbox0);
	evidence->started = true;
	if (cpuctl & (1u << 6))
		falcon_write(io, eng->base + GU_FALCON_CPUCTL_ALIAS_OFFSET, 1u << 1);
	else
		falcon_write(io, eng->base + GU_FALCON_CPUCTL_OFFSET, 1u << 1);

	ret = gu_falcon_poll(io, eng->base + GU_FALCON_CPUCTL_OFFSET,
			     1u << 4, 1u << 4, 5000000, true, NULL);
	if (ret)
		return ret;
	evidence->halted = true;
	return 0;
}

/* TU102/GA100 generic loader for board FWSEC v2. Descriptor offsets are
 * RM_FLCN_BL_DMEM_DESC (84 bytes); loader identity/geometry is pinned by the
 * extractor. The loader uses DMA index 4, coherent physical system memory.
 */
int gu_falcon_loader(const struct gu_falcon_io *io, const struct gu_falcon_engine *eng,
		     const struct gu_falcon_loader_image *program,
		     struct gu_falcon_evidence *evidence)
{
	u32 desc[21] = { 0 }, hwcfg, trans, ctl, i, dst;
	const u32 *code;
	int ret;

	memset(evidence, 0, sizeof(*evidence));
	if (!program->loader || program->loader_size != 768 ||
	    (program->dma & 255) || program->dma > ~0ull - program->data_off ||
	    (program->data_off & 255))
		return -EINVAL;
	code = program->loader;
	ret = gu_falcon_reset(io, eng, evidence);
	if (ret)
		return ret;
	if (falcon_read(io, eng->base + GU_FALCON_CPUCTL_OFFSET, &ctl) || inaccessible(ctl) || (ctl & 16) ||
	    falcon_read(io, eng->base + GU_FALCON_HWCFG_OFFSET, &hwcfg) || inaccessible(hwcfg) ||
	    falcon_read(io, eng->base + GU_FALCON_DMACTL_OFFSET, &trans) || inaccessible(trans))
		return -ENODEV;
	if ((hwcfg & 0x1ff) < 2)
		return -ERANGE;
	dst = ((hwcfg & 0x1ff) - 2) << 8;
	falcon_write(io, eng->base + GU_FALCON_DMACTL_OFFSET, trans & ~1u);
	if (falcon_read(io, eng->fbif + GU_FBIF_TRANSCFG(4), &trans) || inaccessible(trans))
		return -ENODEV;
	falcon_write(io, eng->fbif + GU_FBIF_TRANSCFG(4), (trans & ~7u) | 5u);
	desc[8] = 4;
	desc[9] = lower_32_bits(program->dma);
	desc[10] = upper_32_bits(program->dma);
	desc[11] = program->imem_pa;
	desc[12] = program->imem_ns_size;
	desc[13] = program->imem_sec_pa;
	desc[14] = program->imem_sec_size;
	desc[16] = lower_32_bits(program->dma + program->data_off);
	desc[17] = upper_32_bits(program->dma + program->data_off);
	desc[18] = program->dmem_size;
	falcon_write(io, eng->base + GU_FALCON_DMEMC_OFFSET, 1u << 24);
	for (i = 0; i < 21; i++)
		falcon_write(io, eng->base + GU_FALCON_DMEMD_OFFSET, desc[i]);
	falcon_write(io, eng->base + GU_FALCON_IMEMC_OFFSET, dst | (1u << 24));
	for (i = 0; i < 512; i += 4) {
		if (!(i & 255))
			falcon_write(io, eng->base + GU_FALCON_IMEMT_OFFSET, 0xfd + (i >> 8));
		falcon_write(io, eng->base + GU_FALCON_IMEMD_OFFSET, code[i / 4]);
	}
	evidence->reloaded = true;
	falcon_write(io, eng->base + GU_FALCON_BOOTVEC_OFFSET, 0xfd00);
	falcon_write(io, eng->base + GU_FALCON_MAILBOX0_OFFSET, 0);
	falcon_write(io, eng->base + GU_FALCON_MAILBOX1_OFFSET, 0);
	dma_wmb();
	evidence->started = true;
	falcon_write(io, eng->base + ((ctl & 64) ? GU_FALCON_CPUCTL_ALIAS_OFFSET :
						GU_FALCON_CPUCTL_OFFSET), 2);
	ret = gu_falcon_poll(io, eng->base + GU_FALCON_CPUCTL_OFFSET, 16, 16, 5000000, true, NULL);
	evidence->halted = !ret;
	return ret;
}

/* GA100's direct loader: nonsecure OS stub, tagged secure app, DMEM.
 * GA10x uses the HS DMA/BROM loader with the same signed GA100 image. Its
 * cross-family acceptance is an explicit hardware gate, not an assumption.
 */
static void falcon_pio_imem(const struct gu_falcon_io *io, u32 base, const u32 *image,
			    u32 offset, u32 size, bool secure)
{
	u32 i;

	falcon_write(io, base + GU_FALCON_IMEMC_OFFSET, offset | (1u << 24) |
		  (secure ? 1u << 28 : 0));
	for (i = 0; i < size; i += 4) {
		if (!(i & 255))
			falcon_write(io, base + GU_FALCON_IMEMT_OFFSET, (offset + i) >> 8);
		falcon_write(io, base + GU_FALCON_IMEMD_OFFSET, image[(offset + i) / 4]);
	}
}

int gu_falcon_direct(const struct gu_falcon_io *io, const struct gu_falcon_engine *eng,
		     const struct gu_falcon_direct_image *program,
		     struct gu_falcon_evidence *evidence)
{
	const u32 *image = program->words;
	u32 i, ctl;
	int ret;

	memset(evidence, 0, sizeof(*evidence));
	ret = gu_falcon_reset(io, eng, evidence);
	if (ret)
		return ret;
	ret = gu_falcon_poll(io, eng->base + GU_FALCON_CPUCTL_OFFSET, 16, 0, 1000, true, NULL);
	if (ret)
		return ret;
	/* REQUIRE_CTX=false, matching kflcnDisableCtxReq. */
	if (falcon_read(io, eng->base + GU_FALCON_DMACTL_OFFSET, &ctl) || inaccessible(ctl))
		return -ENODEV;
	falcon_write(io, eng->base + GU_FALCON_DMACTL_OFFSET, ctl & ~1u);
	falcon_pio_imem(io, eng->base, image, program->os_code_off, program->os_code_size, false);
	falcon_pio_imem(io, eng->base, image, program->app_code_off, program->app_code_size, true);
	falcon_write(io, eng->base + GU_FALCON_DMEMC_OFFSET, 1u << 24);
	for (i = 0; i < program->os_data_size; i += 4)
		falcon_write(io, eng->base + GU_FALCON_DMEMD_OFFSET, image[(program->os_data_off + i) / 4]);
	evidence->reloaded = true;
	falcon_write(io, eng->base + GU_FALCON_BOOTVEC_OFFSET, 0);
	falcon_write(io, eng->base + GU_FALCON_MAILBOX0_OFFSET, program->mailbox0);
	falcon_write(io, eng->base + GU_FALCON_MAILBOX1_OFFSET, program->mailbox1);
	if (falcon_read(io, eng->base + GU_FALCON_CPUCTL_OFFSET, &ctl) || inaccessible(ctl))
		return -ENODEV;
	dma_wmb();
	evidence->started = true;
	falcon_write(io, eng->base + ((ctl & 64) ? GU_FALCON_CPUCTL_ALIAS_OFFSET :
						GU_FALCON_CPUCTL_OFFSET), 2);
	ret = gu_falcon_poll(io, eng->base + GU_FALCON_CPUCTL_OFFSET, 16, 16, 5000000, true, NULL);
	evidence->halted = !ret;
	return ret;
}
