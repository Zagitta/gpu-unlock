/* SPDX-License-Identifier: GPL-2.0-only */
/* Address-services shared by the Falcon engine models.
 *
 * The Falcon core only performs an MMIO access.  CSEC BAR0, DOC/DIC and the
 * SE TRNG are separate hardware blocks; keeping their state here avoids
 * teaching the instruction interpreter about individual addresses.
 */
#ifndef GD_SERVICES_H
#define GD_SERVICES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct gd_csec {
	bool (*bar_read)(void *ctx, uint32_t address, uint32_t *value);
	bool (*bar_write)(void *ctx, uint32_t address, uint32_t value);
	void *ctx;
	uint32_t csr, addr, data;
	uint32_t cmd, byte_enable;
	bool finished;
};

void gd_csec_init(struct gd_csec *csec,
		  bool (*bar_read)(void *, uint32_t, uint32_t *),
		  bool (*bar_write)(void *, uint32_t, uint32_t), void *ctx);
uint32_t gd_csec_read(struct gd_csec *csec, uint32_t offset);
void gd_csec_write(struct gd_csec *csec, uint32_t offset, uint32_t value);

struct gd_doc {
	uint32_t ctrl;
	uint32_t d[3];
	uint32_t dic_ctrl;
	uint32_t dic_d[3];
	uint32_t addr;
	uint32_t (*dio_read)(void *ctx, uint32_t addr, int priv);
	void (*dio_write)(void *ctx, uint32_t addr, uint32_t value, int priv);
	void *ctx;
};

void gd_doc_reset(struct gd_doc *doc);
void gd_doc_init(struct gd_doc *doc,
		 uint32_t (*dio_read)(void *, uint32_t, int),
		 void (*dio_write)(void *, uint32_t, uint32_t, int), void *ctx);
uint32_t gd_doc_read(struct gd_doc *doc, uint32_t offset, int priv);
void gd_doc_write(struct gd_doc *doc, uint32_t offset, uint32_t value, int priv);

struct gd_trng {
	uint32_t *regs;       /* SE register file, owned by the engine */
	size_t reg_count;
};

void gd_trng_init(struct gd_trng *trng, uint32_t *regs, size_t reg_count);
uint32_t gd_trng_read(struct gd_trng *trng, uint32_t address);
void gd_trng_write(struct gd_trng *trng, uint32_t address, uint32_t value);

/* Source-defined identity values. */
uint32_t gd_falcon_engid(void);
uint32_t gd_csberrstat(bool enable, bool valid, uint32_t pc);
uint32_t gd_pbus_emulation_rev(unsigned index);

#endif
