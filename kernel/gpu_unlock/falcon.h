/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_FALCON_H
#define GPU_UNLOCK_FALCON_H

#include <linux/types.h>

struct device;

/* Borrowed hardware access only: no PCI, allocation or transaction ownership. */
struct gu_falcon_io {
	struct device *dev;
	void *context;
	int (*read)(void *context, u32 offset, u32 *value);
	void (*write)(void *context, u32 offset, u32 value);
};
struct gu_falcon_engine { u32 base, fbif, falcon2; };

/* Reset for every operation, including validation failures. Available on error.
 * started records a submitted start; halted records a successful halt wait.
 * Neither proves firmware-specific success, cleanup or safe DMA disposal. */
struct gu_falcon_evidence {
	bool reset_asserted, reset_released, reloaded, started, halted;
};

/* Image spans/firmware geometry are validated by preparation before calling.
 * Transport additionally rejects unrepresentable HS/DMA addresses before reset. */
struct gu_falcon_hs_image {
	dma_addr_t dma;
	u32 code_off, code_size, code_va, code_pa;
	/* Non-secure OS stub loaded at IMEM 0, used by the Booter bundle. */
	u32 os_code_off, os_code_size;
	u32 data_off, data_size, data_pa;
	u32 sig_dmem, engine_mask, ucode_id;
	u32 mailbox0, mailbox1;
	/* The caller verifies the result independently, so HALTED left set
	 * across reset is tolerated, as RM does. GA102 GSP and SEC2 keep it. */
	bool stale_halt_ok;
};
struct gu_falcon_direct_image {
	const u32 *words;
	u32 os_code_off, os_code_size, app_code_off, app_code_size;
	u32 os_data_off, os_data_size;
	u32 mailbox0, mailbox1;
};
struct gu_falcon_loader_image {
	const u32 *loader;
	size_t loader_size;
	dma_addr_t dma;
	u32 data_off, imem_pa, imem_ns_size, imem_sec_pa, imem_sec_size, dmem_size;
};

int gu_falcon_poll(const struct gu_falcon_io *io, u32 offset, u32 mask, u32 expect,
		   unsigned int budget_us, bool timeout_is_failure, u32 *last);
int gu_falcon_dma(const struct gu_falcon_io *io, u32 base, u32 dest, u32 mem_off,
		  dma_addr_t src, u32 size, u32 cmd);
int gu_falcon_engine_reset(const struct gu_falcon_io *io, u32 base, const char *phase,
			   u32 *hwcfg, struct gu_falcon_evidence *evidence);
int gu_falcon_reset(const struct gu_falcon_io *io, const struct gu_falcon_engine *eng,
		    struct gu_falcon_evidence *evidence);
int gu_falcon_hs(const struct gu_falcon_io *io, const struct gu_falcon_engine *eng,
		 const struct gu_falcon_hs_image *image, struct gu_falcon_evidence *evidence);
int gu_falcon_direct(const struct gu_falcon_io *io, const struct gu_falcon_engine *eng,
		     const struct gu_falcon_direct_image *image, struct gu_falcon_evidence *evidence);
int gu_falcon_loader(const struct gu_falcon_io *io, const struct gu_falcon_engine *eng,
		     const struct gu_falcon_loader_image *image, struct gu_falcon_evidence *evidence);

#endif
