// SPDX-License-Identifier: GPL-2.0-only
#include <string.h>

#include "services.h"

/* ---- CSEC BAR0 bridge ------------------------------------------------- */

void gd_csec_init(struct gd_csec *csec,
		  bool (*bar_read)(void *, uint32_t, uint32_t *),
		  bool (*bar_write)(void *, uint32_t, uint32_t), void *ctx)
{
	memset(csec, 0, sizeof(*csec));
	csec->bar_read = bar_read;
	csec->bar_write = bar_write;
	csec->ctx = ctx;
	csec->csr = 0x00000400u; /* XACT_FINISHED, STATUS=IDLE */
}

uint32_t gd_csec_read(struct gd_csec *csec, uint32_t offset)
{
	switch (offset) {
	case 0x1c000:
		/* CMD, byte enables and the completed/idle status. */
		return (csec->csr & 0x800070f3u) | 0x00000400u;
	case 0x1c100:
		return csec->addr;
	case 0x1c200:
		return csec->data;
	default:
		return 0;
	}
}

void gd_csec_write(struct gd_csec *csec, uint32_t offset, uint32_t value)
{
	uint32_t address, word, out, i;

	switch (offset) {
	case 0x1c100:
		csec->addr = value;
		return;
	case 0x1c200:
		csec->data = value;
		return;
	case 0x1c000:
		break;
	default:
		return;
	}

	csec->csr = value & ~0x80000000u;
	if (!(value & 0x80000000u))
		return;

	csec->cmd = value & 3u;
	csec->byte_enable = (value >> 4) & 0xfu;
	address = csec->addr;
	word = 0;
	if (!csec->byte_enable)
		csec->byte_enable = 0xfu;

	if (csec->cmd == 1u) { /* read */
		if (csec->bar_read && csec->bar_read(csec->ctx, address & ~3u, &word)) {
			out = 0;
			for (i = 0; i < 4; i++)
				if (csec->byte_enable & (1u << i))
					out |= ((word >> (8 * ((address + i) & 3))) & 0xffu) << (8 * i);
			csec->data = out;
		}
	} else if (csec->cmd == 2u || csec->cmd == 3u) { /* write */
		if (csec->bar_write && csec->bar_read &&
		    csec->bar_read(csec->ctx, address & ~3u, &word)) {
			for (i = 0; i < 4; i++)
				if (csec->byte_enable & (1u << i))
					word = (word & ~(0xffu << (8 * ((address + i) & 3)))) |
					       (((csec->data >> (8 * i)) & 0xffu) <<
						(8 * ((address + i) & 3)));
			csec->bar_write(csec->ctx, address & ~3u, word);
		}
	}
	csec->finished = true;
	csec->csr = 0x00000400u;
}

/* ---- DOC / DIC output and input queues -------------------------------- */

void gd_doc_reset(struct gd_doc *doc)
{
	memset(doc, 0, sizeof(*doc));
	/*
	 * The debug FWSEC/Booter expect the pre-existing DIO idle count
	 * (0x1c0004).  CSEC's hardware reset is 0x001c0000; COUNT is a queue
	 * property and is firmware-visible, so keep the observed value here.
	 */
	doc->ctrl = 0x001c0004u;
}

void gd_doc_init(struct gd_doc *doc,
		 uint32_t (*dio_read)(void *, uint32_t, int),
		 void (*dio_write)(void *, uint32_t, uint32_t, int), void *ctx)
{
	gd_doc_reset(doc);
	doc->dio_read = dio_read;
	doc->dio_write = dio_write;
	doc->ctx = ctx;
}

uint32_t gd_doc_read(struct gd_doc *doc, uint32_t offset, int priv)
{
	(void)priv;
	switch (offset) {
	case 0xd000:
		return doc->ctrl;
	case 0xd100:
		return doc->addr;
	case 0xd200:
	case 0xd300:
		return doc->d[(offset - 0xd200) / 0x100];
	case 0xd400:
		return (doc->dic_ctrl & 0xffu) |
		       ((doc->dic_ctrl & (1u << 19)) ? (1u << 19) : 0);
	case 0xd500:
	case 0xd600:
	case 0xd700:
		return doc->dic_d[(offset - 0xd500) / 0x100];
	default:
		return 0;
	}
}

void gd_doc_write(struct gd_doc *doc, uint32_t offset, uint32_t value, int priv)
{
	switch (offset) {
	case 0xd000:
		/* The debug firmware treats DOC_CTRL writes as error clears.  The
		 * queue status is read-only from its point of view. */
		return;
	case 0xd100:
		doc->addr = value & 0xffffu;
		if (value & 0x10000u) {
			if (doc->dio_read)
				doc->dic_d[0] = doc->dio_read(doc->ctx, doc->addr, priv);
			doc->dic_ctrl = (doc->dic_ctrl & ~0xffu) | 1u;
			doc->dic_ctrl |= 1u << 19;
		} else if (doc->dio_write) {
			doc->dio_write(doc->ctx, doc->addr, doc->d[0], priv);
		}
		return;
	case 0xd200:
	case 0xd300:
		doc->d[(offset - 0xd200) / 0x100] = value;
		return;
	case 0xd400:
		doc->dic_ctrl = value;
		return;
	case 0xd500:
	case 0xd600:
	case 0xd700:
		doc->dic_d[(offset - 0xd500) / 0x100] = value;
		return;
	default:
		return;
	}
}

/* ---- SE TRNG ---------------------------------------------------------- */

void gd_trng_init(struct gd_trng *trng, uint32_t *regs, size_t reg_count)
{
	memset(trng, 0, sizeof(*trng));
	trng->regs = regs;
	trng->reg_count = reg_count;
}

uint32_t gd_trng_read(struct gd_trng *trng, uint32_t address)
{
	if (!trng->regs || address / 4 >= trng->reg_count)
		return 0;
	/* The debug image only requires the post-reseed ready state.  Full
	 * entropy generation remains in emulator/falcon_emu/se_trng.py. */
	if (address == 0x3f04)
		return 0x300u;
	if (address == 0x104)
		return trng->regs[0x104 / 4] | 1u;
	return trng->regs[address / 4];
}

void gd_trng_write(struct gd_trng *trng, uint32_t address, uint32_t value)
{
	if (!trng->regs || address / 4 >= trng->reg_count)
		return;
	trng->regs[address / 4] = value;
}

/* ---- Source-defined identification registers -------------------------- */

uint32_t gd_falcon_engid(void)
{
	/* NV_CSEC_FALCON_ENGID: FAMILYID_SEC (1), instance 0. */
	return 0x00000100u;
}

uint32_t gd_csberrstat(bool enable, bool valid, uint32_t pc)
{
	return (valid ? 0x80000000u : 0) |
	       (enable ? 0x40000000u : 0) |
	       (pc & 0x00ffffffu);
}

uint32_t gd_pbus_emulation_rev(unsigned index)
{
	(void)index;
	/* NV_PBUS_EMULATION_REV0/1_VALUE_HARDWARE. */
	return 0;
}
