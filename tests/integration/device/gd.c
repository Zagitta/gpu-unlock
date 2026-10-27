// SPDX-License-Identifier: GPL-2.0-only
/* Native GA102 device model. Stage 1 models the host-visible register and
 * DMA behavior; execution is always by the Falcon core. */
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gd.h"
#include "aes.h"
#include "booter_regs.h"
#include "wpr2.h"
#include "services.h"

#define SEC2_BASE       0x00840000u
#define GSP_BASE        0x00110000u
#define ENGINE_WINDOW   0x00004000u
#define GD_IO_SIZE      0x02000000u
/* The layout places every reservation below the top 256 MiB of usable
 * framebuffer; model that whole window so WPR2, the GSP image, the BL and
 * FRTS all have backing.  The base is derived from the FB-size register. */
#define GD_FB_SIZE       (256u << 20)
#define GD_DEVMAP_BASE  0x01000000u
#define GD_DEVMAP_SIZE  0x00001000u
#define PROM_OFFSET     0x00300000u
#define PROM_SIZE       0x00100000u

#define OFF_MAILBOX0    0x040u
#define OFF_MAILBOX1    0x044u
#define OFF_HWCFG2      0x0f4u
#define OFF_CPUCTL      0x100u
#define OFF_BOOTVEC     0x104u
#define OFF_HWCFG       0x108u
#define OFF_DMACTL      0x10cu
#define OFF_DMABASE     0x110u
#define OFF_DMAMOFFS    0x114u
#define OFF_DMACMD      0x118u
#define OFF_DMAFBOFFS   0x11cu
#define OFF_DMABASE1    0x128u
#define OFF_CPUCTL_ALT  0x130u
#define OFF_IMEMC       0x180u
#define OFF_IMEMD       0x184u
#define OFF_IMEMT       0x188u
#define OFF_DMEMC       0x1c0u
#define OFF_DMEMD       0x1c4u
#define OFF_ENGINE      0x3c0u
#define OFF_RESET_PLM   0x3c4u

#define GD_DIAG(...) do { } while (0)

#define CPUCTL_START    0x2u
#define CPUCTL_HALTED   0x10u
#define HWCFG2_SCRUB    (1u << 12)
#define HWCFG2_RESET    (1u << 31)

struct gd_code_rec {
	uint32_t pc, target;
	int32_t off, a, b, d, base, idx, scale;
	uint8_t len, op, sz, sized, bimm, space;
};

struct gd_engine {
	struct gd_device *dev;
	int index;
	uint32_t base;
	bool enabled;
	bool started, halted, faulted;
	bool scrub_pending;
	uint64_t scrub_done_us;
	uint32_t cpuctl, hwcfg2, bootvec, hwcfg, dmactl;
	uint32_t dma_base, dma_base1, dma_moffs, dma_fboffs, dma_cmd, dma_stat;
	uint32_t mailbox[2], rm, scratch[2], reset_plm, engine_reg;
	uint32_t imemc, dmemc, pio_imem, pio_dmem;
	uint32_t pio_secure_lo, pio_secure_hi;
	bool csb_enable, csb_valid;
	uint32_t csb_pc;
	uint32_t imem_tag;
	uint8_t imem[0x10000];
	uint8_t dmem[0x10000];
	uint8_t devmap[GD_DEVMAP_SIZE];
	uint32_t io[GD_IO_SIZE / 4];
	uint8_t io_valid[GD_IO_SIZE / 4];
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t wake;
	bool thread_up, stop, busy, pending_kick;
	pthread_cond_t idle;
	uint32_t dma_q[64];
	unsigned qhead, qtail;
	uint64_t instructions;
	uint32_t r[16], pc, sp, flags;
	bool core_fault;
	uint32_t sr[16];
	uint32_t call_depth;
	uint32_t call_pc[64];
	uint32_t pc_ring[64], pc_ring_at;
	uint32_t br_pc[64], br_tgt[64], br_sp[64], br_at;
	uint32_t ret_pc[64], ret_val[64], ret_sp[64], ret_at;
	uint32_t sh_ret[128], sh_sp[128], sh_at, sh_bad;
	uint32_t halt_pc, halt_reason;
	uint32_t token, owner, aux_owner, response, response_ready;
	uint8_t c[8][16];
	bool xfer_trap, xfer_shortcut;
	uint32_t seq_pending;
	bool crypto_recording;
	uint8_t crypto_record_slot, crypto_record_left;
	uint8_t crypto_script_op[2][64], crypto_script_d[2][64], crypto_script_a[2][64], crypto_script_imm[2][64];
	uint8_t crypto_script_count[2];
	uint8_t crypto_stream_in[64][16], crypto_stream_out[64][16];
	uint8_t fwsec_kdf_out[16], fwsec_expect[32], fwsec_digest[16];
	bool fwsec_kdf_seen, fwsec_digest_seen;
	uint8_t crypto_stream_in_count, crypto_stream_out_count;
	uint64_t crypto_rng;
	uint8_t tlb_flags[256], tlb_virt[256];
	uint32_t keyreg;
	uint32_t xfer_count;
	uint32_t sha_op, sha_status;
	uint32_t sha_mutex;
	uint32_t sha_h[8], sha_buf_len, sha_update_bits;
	uint8_t sha_buf[64];
	uint64_t sha_total_bits;
	bool sha_started;
	bool sha_stream;
	uint32_t sha_in_addr, sha_in_hi, sha_msg_len;
	uint32_t sha_result[8];
	uint32_t doc_addr, doc_data[3];
	uint32_t dic_count, dic_valid, dic_data[3];
	uint32_t se_regs[0x8000];
	struct gd_csec csec;
	struct gd_doc doc;
	struct gd_trng trng;
	struct gd_code_rec *code;
	uint32_t code_count;
	int32_t *code_index;
	bool c_hidden[8];
};

struct gd_csb {
	/* NV_SSE_SE_COMMON_MUTEX: 3 groups x 0x50 bytes.  ID is an
	 * 8-bit hardware-assigned token, MUTEX has 16 slots, and
	 * ID_RELEASE returns the token. */
	uint32_t plm[3];
	uint8_t id[3];
	uint8_t mutex[3][16];
	bool released[3];
};

struct gd_device {
	const struct gd_board *board;
	struct gd_run_config config;
	uint32_t *bar;
	uint8_t *bar_valid;
	uint8_t *prom;
	size_t prom_size;
	struct gd_engine engine[2];
	pthread_mutex_t lock;
	pthread_mutex_t bar_lock;
	pthread_mutex_t dma_lock;
	struct gd_dma_span dma[GD_MAX_DMA];
	uint32_t dma_count;
	struct gd_csb csb;
	uint8_t *fb;
	size_t fb_size;
	uint64_t fb_base;
	uint32_t pci_command;
	uint32_t pci_saved_command;
	uint32_t devices;
	uint64_t time_us;
	int device_error;
	uint32_t fault_pc, fault_opcode;
	uint32_t denied_writes;
	uint32_t plm_violations;
	bool plm_opened_seen, target_write_seen;
	uint32_t plm_feat_latched, plm_wpr_cfg_latched;
	uint8_t secrets[256][16];
	bool secret_set[256];
	uint32_t unknown_count;
	struct gd_unknown unknown[GD_MAX_UNKNOWN];
	uint64_t host_writes, host_reads, engine_writes, engine_reads;
	uint32_t launches[2];
	uint32_t resets;
	bool flr_seen;
	bool fwsec_launch_reached, booter_launch_reached;
	bool fwsec_kdf_ok;
	bool fwsec_mac_ok;
	bool wpr2_normalized;
	uint32_t feats[8];
	uint32_t feat_count;
	uint32_t wpr2_lo, wpr2_hi;
	uint32_t ss0, ss1;
	struct gd_trace_record trace[GD_MAX_TRACE];
	uint32_t trace_head, trace_count;
	pthread_mutex_t trace_lock;
};

static uint64_t now_us(void);

static void trace_add(struct gd_device *dev, enum gd_trace_kind kind, int source,
		      uint32_t address, uint32_t value, uint32_t pc)
{
	uint32_t slot;
	pthread_mutex_lock(&dev->trace_lock);
	slot = dev->trace_head++ % GD_MAX_TRACE;
	if (dev->trace_count < GD_MAX_TRACE)
		dev->trace_count++;
	dev->trace[slot].time_us = now_us();
	dev->trace[slot].kind = (uint16_t)kind;
	dev->trace[slot].source = (uint16_t)source;
	dev->trace[slot].address = address;
	dev->trace[slot].value = value;
	dev->trace[slot].pc = pc;
	dev->trace[slot].size = 4;
	pthread_mutex_unlock(&dev->trace_lock);
}

static uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + ts.tv_nsec / 1000u;
}

void gd_log(struct gd_device *dev, enum gd_log_level level, const char *format, ...)
{
	va_list args;
	(void)level;
	if (!dev || !dev->config.verbose)
		return;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
}

static void device_error(struct gd_device *dev, const char *what, uint32_t address, uint32_t value)
{
	if (!dev->device_error)
		dev->device_error = -ENODEV;
	gd_log(dev, GD_LOG_ERROR, "device: %s at %#x value=%#x\n", what, address, value);
}

static void record_unknown(struct gd_device *dev, bool write, uint32_t addr, uint32_t value, uint32_t pc)
{
	if (dev->unknown_count < GD_MAX_UNKNOWN) {
		struct gd_unknown *u = &dev->unknown[dev->unknown_count++];
		u->write = write;
		u->address = addr;
		u->value = value;
		u->pc = pc;
	}
	device_error(dev, write ? "unknown write" : "unknown read", addr, value);
}

static struct gd_engine *engine_for(struct gd_device *dev, uint32_t address, uint32_t *offset)
{
	uint32_t off;

	(void)off;
	if (address >= SEC2_BASE && address < SEC2_BASE + ENGINE_WINDOW) {
		*offset = address - SEC2_BASE;
		return &dev->engine[0];
	}
	if (address >= GSP_BASE && address < GSP_BASE + ENGINE_WINDOW) {
		*offset = address - GSP_BASE;
		return &dev->engine[1];
	}
	return NULL;
}

static struct gd_dma_span *dma_find(struct gd_device *dev, uint64_t address, size_t size)
{
	uint32_t i;
	for (i = 0; i < dev->dma_count; i++) {
		struct gd_dma_span *s = &dev->dma[i];
		if (s->live && address >= s->address && address + size <= s->address + s->size)
			return s;
	}
	return NULL;
}

static struct gd_dma_span *dma_find_size(struct gd_device *dev, size_t size)
{
	uint32_t i;
	for (i = 0; i < dev->dma_count; i++)
		if (dev->dma[i].live && dev->dma[i].size == size)
			return &dev->dma[i];
	return NULL;
}

static void plm_latch_write(struct gd_device *dev, uint32_t address, uint32_t value);

static void set_bar_locked(struct gd_device *dev, uint32_t address, uint32_t value)
{
	if (address + 4 > GD_BAR_SIZE) {
		record_unknown(dev, true, address, value, 0);
		return;
	}
	dev->bar[address / 4] = value;
	dev->bar_valid[address / 4] = 1;
	plm_latch_write(dev, address, value);
}

static void settle_scrub(struct gd_device *dev, struct gd_engine *eng)
{
	if (!eng->scrub_pending)
		return;
	if (dev->time_us < eng->scrub_done_us)
		return;
	eng->scrub_pending = false;
	eng->hwcfg2 &= ~HWCFG2_SCRUB;
	memset(eng->imem, 0, sizeof(eng->imem));
	memset(eng->dmem, 0, sizeof(eng->dmem));
	gd_log(dev, GD_LOG_DEBUG, "device: engine %d scrub complete\n", eng->index);
}

static void engine_reset_state(struct gd_device *dev, struct gd_engine *eng)
{
	eng->started = eng->faulted = false;
	/* A CPU reset leaves the core halted and not started; the next
	 * CPUCTL.STARTCPU write clears HALTED. */
	eng->halted = true;
	eng->cpuctl = CPUCTL_HALTED;
	eng->bootvec = 0;
	eng->dmactl = 0;
	eng->dma_stat = 2;
	eng->rm = 0;
	eng->reset_plm = 0xffffffffu; /* all levels enabled, per RESET_PLM init */
	/* The CPU reset restores the per-register masks to all-levels; the
	 * Booter locks them again before it starts. */
	eng->io[0x288 / 4] = 0xffffffffu;
	eng->io[0x194 / 4] = 0xffffffffu;
	eng->io[0x28c / 4] = 0xffffffffu;
	eng->io[0x190 / 4] = 0xffffffffu;
	eng->io[0x18c / 4] = 0xffffffffu;
	eng->io[0x2b0 / 4] = 0xffffffffu;
	eng->io[0x3c4 / 4] = eng->reset_plm;
	eng->engine_reg = 0;
	eng->pio_imem = eng->pio_dmem = 0;
	eng->pio_secure_lo = eng->pio_secure_hi = 0;
	eng->csb_enable = true;
	eng->csb_valid = false;
	eng->csb_pc = 0;
	eng->sha_started = false;
	eng->sha_buf_len = 0;
	eng->sha_update_bits = 0;
	eng->sha_total_bits = 0;
	eng->imemc = eng->dmemc = 0;
	eng->imem_tag = 0;
	eng->hwcfg2 = HWCFG2_RESET;
	eng->scrub_pending = false;
	eng->scrub_done_us = 0;
	memset(eng->imem, 0, sizeof(eng->imem));
	memset(eng->dmem, 0, sizeof(eng->dmem));
	memset(eng->se_regs, 0, sizeof(eng->se_regs));
	eng->se_regs[0x144 / 4] = 0xffffffffu; /* SE mutex timeout default */
	eng->se_regs[0x114 / 4] = 1; /* PKA mutex free/available */
	eng->se_regs[0x4020 / 4] = 1u << 30;   /* PKA status IRQ */
}

static uint32_t mask_of(int sz) { return sz == 32 ? 0xffffffffu : (1u << sz) - 1; }

static void set_r(struct gd_engine *eng, int d, uint32_t value, int sz)
{
	uint32_t mask = mask_of(sz);
	if (sz == 32)
		eng->r[d] = value;
	else
		eng->r[d] = (eng->r[d] & ~mask) | (value & mask);
}

static void set_flags(struct gd_engine *eng, uint32_t result, int sz, int overflow, int carry)
{
	uint32_t f = eng->flags & ~0xe00u;
	if (carry >= 0)
		f = (f & ~0x100u) | (uint32_t)carry << 8;
	eng->flags = f | (uint32_t)overflow << 9 |
		((result >> (sz - 1)) & 1) << 10 | (result == 0) << 11;
}

static int condition(uint32_t f, int cc)
{
	int c, o, s, z;
	if (cc < 8)
		return (f >> cc) & 1;
	if (cc >= 0x10 && cc < 0x18)
		return !((f >> (cc - 0x10)) & 1);
	c = (f >> 8) & 1; o = (f >> 9) & 1; s = (f >> 10) & 1; z = (f >> 11) & 1;
	switch (cc) {
	case 8: return c; case 9: return o; case 10: return s; case 11: return z;
	case 12: return !c && !z; case 13: return c || z; case 14: return 1;
	case 24: return !c; case 25: return !o; case 26: return !s; case 27: return !z;
	case 28: return !(o ^ s) && !z; case 29: return (o ^ s) || z;
	case 30: return o ^ s; case 31: return !(o ^ s); default: return -1;
	}
}


/* ---- crypto coprocessor and code TLB ---------------------------------- */

static void crypto_gfmul2(const uint8_t in[16], uint8_t out[16])
{
	uint8_t v[16];
	int carry = in[0] >> 7;
	for (int i = 0; i < 15; i++)
		v[i] = (uint8_t)((in[i] << 1) | (in[i + 1] >> 7));
	v[15] = (uint8_t)(in[15] << 1);
	if (carry)
		v[15] ^= 0x87;
	memcpy(out, v, 16);
}

static void crypto_record(struct gd_engine *eng, unsigned op, unsigned d,
			  unsigned a, unsigned imm)
{
	unsigned slot = eng->crypto_record_slot;
	unsigned n;
	if (!eng->crypto_recording || slot > 1)
		return;
	n = eng->crypto_script_count[slot];
	if (n >= 64)
		return;
	eng->crypto_script_op[slot][n] = (uint8_t)op;
	eng->crypto_script_d[slot][n] = (uint8_t)d;
	eng->crypto_script_a[slot][n] = (uint8_t)a;
	eng->crypto_script_imm[slot][n] = (uint8_t)imm;
	eng->crypto_script_count[slot] = (uint8_t)(n + 1);
	if (eng->crypto_record_left)
		eng->crypto_record_left--;
	if (!eng->crypto_record_left)
		eng->crypto_recording = false;
}

static void crypto_exec_script(struct gd_engine *eng, unsigned slot, unsigned count);

static void crypto_apply(struct gd_engine *eng, unsigned op, unsigned d,
			 unsigned a, unsigned imm)
{
	uint8_t tmp[16], key[16], round[176];
	uint64_t lo;
	bool cs_op = op == 86 || op == 87 || op == 88 || op == 104 ||
		     op == 105 || op == 106 || op == 107;

	if (!cs_op)
		crypto_record(eng, op, d, a, imm);
	if (eng->crypto_recording && !cs_op)
		return;

	switch (op) {
	case 70: /* csecret */
		memset(eng->c[d], 0, 16);
		if (imm < 256 && eng->dev->secret_set[imm])
			memcpy(eng->c[d], eng->dev->secrets[imm], 16);
		eng->c_hidden[d] = true;
		break;
	case 71: /* cenc */
		gd_aes128_encrypt_block(eng->c[eng->keyreg & 7], eng->c[a], eng->c[d]);
		eng->c_hidden[d] = eng->c_hidden[eng->keyreg & 7] || eng->c_hidden[a];
		break;
	case 72: /* cxor */
		for (int i = 0; i < 16; i++)
			eng->c[d][i] ^= eng->c[a][i];
		eng->c_hidden[d] = eng->c_hidden[d] || eng->c_hidden[a];
		break;
	case 73: /* crnd */
		eng->crypto_rng = eng->crypto_rng * 6364136223846793005ULL + 1;
		for (int i = 0; i < 16; i++) {
			eng->crypto_rng = eng->crypto_rng * 6364136223846793005ULL + 1;
			eng->c[d][i] = (uint8_t)(eng->crypto_rng >> 56);
		}
		eng->c_hidden[d] = false;
		break;
	case 74: /* ckeyreg */
		eng->keyreg = d;
		break;
	case 75: /* cmov */
		memcpy(eng->c[d], eng->c[a], 16);
		eng->c_hidden[d] = eng->c_hidden[a];
		break;
	case 81: /* cxsin */
		if (eng->crypto_stream_in_count) {
			eng->crypto_stream_in_count--;
			memcpy(eng->c[d], eng->crypto_stream_in[eng->crypto_stream_in_count], 16);
		} else {
			memset(eng->c[d], 0, 16);
		}
		eng->c_hidden[d] = false;
		break;
	case 82: /* cxsout */
		if (eng->crypto_stream_out_count < 8) {
			memcpy(eng->crypto_stream_out[eng->crypto_stream_out_count], eng->c[d], 16);
			eng->crypto_stream_out_count++;
		}
		break;
	case 86: /* cs0begin */
		eng->crypto_script_count[0] = 0;
		eng->crypto_stream_in_count = 0;
		eng->crypto_stream_out_count = 0;
		eng->seq_pending = 0;
		eng->crypto_recording = imm != 0;
		eng->crypto_record_slot = 0;
		eng->crypto_record_left = (uint8_t)imm;
		break;
	case 87: /* cs0exec: arm the sequencer; data flow drives it */
		eng->seq_pending = imm;
		break;
	case 88: /* csigclr */
		break;
	case 96: /* cadd: 64-bit big-endian counter in bytes 8..15 */
		lo = 0;
		for (int i = 8; i < 16; i++)
			lo = (lo << 8) | eng->c[d][i];
		lo += imm;
		for (int i = 15; i >= 8; i--) {
			eng->c[d][i] = (uint8_t)lo;
			lo >>= 8;
		}
		break;
	case 97: /* cand */
		for (int i = 0; i < 16; i++)
			eng->c[d][i] &= eng->c[a][i];
		eng->c_hidden[d] = eng->c_hidden[d] || eng->c_hidden[a];
		break;
	case 98: /* crev */
		for (int i = 0; i < 16; i++)
			tmp[i] = eng->c[a][15 - i];
		memcpy(eng->c[d], tmp, 16);
		eng->c_hidden[d] = eng->c_hidden[a];
		break;
	case 99: /* cgfmul */
		crypto_gfmul2(eng->c[a], eng->c[d]);
		eng->c_hidden[d] = eng->c_hidden[a];
		break;
	case 100: /* cchmod */
		break;
	case 101: /* ckexp: last round key */
		gd_aes128_expand_key(eng->c[a], round);
		memcpy(eng->c[d], round + 160, 16);
		eng->c_hidden[d] = eng->c_hidden[a];
		break;
	case 102: /* ckrexp */
		gd_aes128_reverse_key(eng->c[a], key);
		memcpy(eng->c[d], key, 16);
		eng->c_hidden[d] = eng->c_hidden[a];
		break;
	case 103: /* cdec */
		gd_aes128_reverse_key(eng->c[eng->keyreg & 7], key);
		gd_aes128_decrypt_block(key, eng->c[a], eng->c[d]);
		eng->c_hidden[d] = eng->c_hidden[eng->keyreg & 7] || eng->c_hidden[a];
		break;
	case 104: /* csigcmp: no signature model */
		break;
	case 105: /* csigenc: no code signature, zeros */
		memset(eng->c[d], 0, 16);
		eng->c_hidden[d] = true;
		break;
	case 106: /* cs1begin */
		eng->crypto_script_count[1] = 0;
		eng->crypto_recording = imm != 0;
		eng->crypto_record_slot = 1;
		eng->crypto_record_left = (uint8_t)imm;
		break;
	case 107: /* cs1exec */
		crypto_exec_script(eng, 1, imm);
		break;
	case 120: case 121: case 122: case 123: case 124: case 125:
	case 126: case 127: case 128: case 129: case 130: case 131:
	case 132: case 133: case 134: case 135: case 136: case 137:
	case 138: case 139: case 140: case 141: case 142: {
		static const uint8_t map[] = {
			75, 81, 82, 73, 86, 87, 106, 107, 100, 72, 96, 97,
			98, 99, 70, 74, 101, 102, 71, 103, 104, 105, 88
		};
		uint32_t v = eng->r[a];
		crypto_apply(eng, map[op - 120], (v >> 8) & 7, v & 7, v & 0x3f);
		break;
	}
	default:
		break;
	}
	(void)tmp;
}

static void crypto_exec_script(struct gd_engine *eng, unsigned slot, unsigned count)
{
	for (unsigned n = 0; n < count; n++)
		for (unsigned i = 0; i < eng->crypto_script_count[slot]; i++)
			crypto_apply(eng, eng->crypto_script_op[slot][i],
				     eng->crypto_script_d[slot][i],
				     eng->crypto_script_a[slot][i],
				     eng->crypto_script_imm[slot][i]);
}

static void tlb_set(struct gd_engine *eng, unsigned phys, unsigned virt, unsigned flags)
{
	if (phys >= 256)
		return;
	eng->tlb_virt[phys] = (uint8_t)(virt & 0xff);
	eng->tlb_flags[phys] = (uint8_t)(flags & 7);
}

static void tlb_itlb(struct gd_engine *eng, unsigned phys)
{
	if (phys < 256 && !(eng->tlb_flags[phys] & 4))
		tlb_set(eng, phys, 0, 0);
}

static uint32_t tlb_ptlb(struct gd_engine *eng, unsigned phys)
{
	if (phys >= 256)
		return 0;
	return (uint32_t)eng->tlb_flags[phys] << 24 | (uint32_t)eng->tlb_virt[phys] << 8;
}

static uint32_t tlb_vtlb(struct gd_engine *eng, unsigned virt)
{
	unsigned page = (virt >> 8) & 0xff;
	unsigned flags = 0, phys = 0, count = 0;
	for (unsigned i = 0; i < 256; i++)
		if (eng->tlb_flags[i] && eng->tlb_virt[i] == page) {
			flags |= eng->tlb_flags[i];
			phys = i;
			count++;
		}
	uint32_t res = phys | (flags << 24);
	if (!count)
		res |= 0x80000000u;
	if (count > 1)
		res |= 0x40000000u;
	return res;
}

static uint32_t dmem_read(struct gd_engine *eng, uint32_t address, int sz)
{
	uint32_t value = 0;
	int width = sz / 8, i;
	if (address >= GD_DEVMAP_BASE && address < GD_DEVMAP_BASE + GD_DEVMAP_SIZE) {
		uint32_t at = address - GD_DEVMAP_BASE;
		for (i = 0; i < width && at + i < GD_DEVMAP_SIZE; i++)
			value |= (uint32_t)eng->devmap[at + i] << (8 * i);
		return value;
	}
	address &= 0xffffu;
	address &= ~(uint32_t)(width - 1);
	for (i = 0; i < width; i++)
		value |= (uint32_t)eng->dmem[(address + i) & 0xffffu] << (8 * i);
	return value;
}

static void dmem_write(struct gd_engine *eng, uint32_t address, int sz, uint32_t value)
{
	int width = sz / 8, i;
	if (eng->index == 0 && address == 0xff50)
		GD_DIAG( "diag: w-ff50 val=%#x sz=%d pc=%#x\n", value, sz, eng->pc);
	if (eng->index == 0 && address >= 0xff40 && address <= 0xffe0 && value == 0xd44)
		GD_DIAG( "diag: write-d44 [%#x] pc=%#x sized=%d sz=%d\n", address, eng->pc, 0, sz);
	if (eng->index == 0 && address >= 0xff40 && address <= 0xffe0 && value == 0x1fce)
		GD_DIAG( "diag: push1fce [%#x] pc=%#x\n", address, eng->pc);
	if (eng->index == 0 && address >= 0xffa0 && address <= 0xffc8)
		GD_DIAG( "diag: dmem-write [%#x]=%#x sz=%d pc=%#x\n", address, value, sz, eng->pc);
	if (eng->index == 0 && (address == 0x1650 || address == 0x1654 ||
				address == 0x1658 || address == 0x165c))
		GD_DIAG( "diag: dmem1650[%#x] = %#x pc=%#x\n", address, value, eng->pc);
	if (address >= GD_DEVMAP_BASE && address < GD_DEVMAP_BASE + GD_DEVMAP_SIZE) {
		uint32_t at = address - GD_DEVMAP_BASE;
		for (i = 0; i < width && at + i < GD_DEVMAP_SIZE; i++)
			eng->devmap[at + i] = value >> (8 * i);
		return;
	}
	address &= 0xffffu;
	address &= ~(uint32_t)(width - 1);
	for (i = 0; i < width; i++)
		eng->dmem[(address + i) & 0xffffu] = value >> (8 * i);
}

static uint32_t rotr32(uint32_t x, int n)
{
	return (x >> n) | (x << (32 - n));
}

static void sha256_block(uint32_t h[8], const uint8_t block[64])
{
	static const uint32_t k[64] = {
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
		0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
		0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
		0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
		0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
		0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
		0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
		0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
		0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
		0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
		0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
		0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
		0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
		0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
		0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
		0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
	};
	uint32_t w[64], a, b, c, d, e, f, g, hh;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = (uint32_t)block[i * 4] << 24 |
		       (uint32_t)block[i * 4 + 1] << 16 |
		       (uint32_t)block[i * 4 + 2] << 8 |
		       block[i * 4 + 3];
	for (i = 16; i < 64; i++)
		w[i] = w[i - 16] + (rotr32(w[i - 15], 7) ^
		       rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3)) +
		       w[i - 7] + (rotr32(w[i - 2], 17) ^
		       rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10));
	a = h[0]; b = h[1]; c = h[2]; d = h[3];
	e = h[4]; f = h[5]; g = h[6]; hh = h[7];
	for (i = 0; i < 64; i++) {
		uint32_t t1 = hh + (rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25)) +
			      ((e & f) ^ (~e & g)) + k[i] + w[i];
		uint32_t t2 = (rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22)) +
			      ((a & b) ^ (a & c) ^ (b & c));
		hh = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}
	h[0] += a; h[1] += b; h[2] += c; h[3] += d;
	h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

static void sha256_init(uint32_t h[8])
{
	h[0] = 0x6a09e667; h[1] = 0xbb67ae85;
	h[2] = 0x3c6ef372; h[3] = 0xa54ff53a;
	h[4] = 0x510e527f; h[5] = 0x9b05688c;
	h[6] = 0x1f83d9ab; h[7] = 0x5be0cd19;
}

static void sha256_update(struct gd_engine *eng, const uint8_t *data, size_t length)
{
	size_t at = 0;
	while (at < length) {
		size_t n = 64 - eng->sha_buf_len;
		if (n > length - at)
			n = length - at;
		memcpy(eng->sha_buf + eng->sha_buf_len, data + at, n);
		eng->sha_buf_len += n;
		at += n;
		if (eng->sha_buf_len == 64) {
			sha256_block(eng->sha_h, eng->sha_buf);
			eng->sha_buf_len = 0;
		}
	}
}

static void sha256_final(struct gd_engine *eng, uint8_t out[32])
{
	uint8_t tail[128] = { 0 };
	size_t rem = eng->sha_buf_len;
	uint64_t bits = eng->sha_total_bits;

	memcpy(tail, eng->sha_buf, rem);
	tail[rem] = 0x80;
	if (rem >= 56) {
		for (int i = 0; i < 8; i++)
			tail[127 - i] = (uint8_t)(bits >> (8 * i));
		sha256_block(eng->sha_h, tail);
		sha256_block(eng->sha_h, tail + 64);
	} else {
		for (int i = 0; i < 8; i++)
			tail[63 - i] = (uint8_t)(bits >> (8 * i));
		sha256_block(eng->sha_h, tail);
	}
	for (int i = 0; i < 8; i++) {
		out[i * 4] = (uint8_t)(eng->sha_h[i] >> 24);
		out[i * 4 + 1] = (uint8_t)(eng->sha_h[i] >> 16);
		out[i * 4 + 2] = (uint8_t)(eng->sha_h[i] >> 8);
		out[i * 4 + 3] = (uint8_t)eng->sha_h[i];
	}
}

static void sha_feed(struct gd_engine *eng)
{
	size_t length = eng->sha_in_hi & 0xffffffu;

	if (!eng->sha_update_bits)
		return;
	if (eng->sha_update_bits && length > eng->sha_update_bits / 8)
		length = eng->sha_update_bits / 8;
	if (!eng->sha_started) {
		sha256_init(eng->sha_h);
		eng->sha_buf_len = 0;
		eng->sha_started = true;
	}
	if (eng->dev->config.verbose)
		gd_log(eng->dev, GD_LOG_DEBUG, "device: sha-feed pc=%#x addr=%#x hi=%#x len=%#x total=%#x\n",
		       eng->pc, eng->sha_in_addr, eng->sha_in_hi, eng->sha_update_bits, eng->sha_msg_len);
	if (length && eng->sha_in_addr + length <= sizeof(eng->dmem)) {
		if (eng->dev->config.verbose)
			gd_log(eng->dev, GD_LOG_DEBUG, "device: sha-feed-data addr=%#x len=%#zx bytes=%02x%02x%02x%02x %02x%02x%02x%02x\n",
			       eng->sha_in_addr, length, eng->dmem[eng->sha_in_addr],
			       eng->dmem[eng->sha_in_addr+1], eng->dmem[eng->sha_in_addr+2], eng->dmem[eng->sha_in_addr+3],
			       eng->dmem[eng->sha_in_addr+4], eng->dmem[eng->sha_in_addr+5], eng->dmem[eng->sha_in_addr+6], eng->dmem[eng->sha_in_addr+7]);
		sha256_update(eng, eng->dmem + eng->sha_in_addr, length);
	}
	else if (length) {
		gd_log(eng->dev, GD_LOG_ERROR,
		       "device: sha update outside DMEM addr=%#x len=%#x\n",
		       eng->sha_in_addr, eng->sha_update_bits);
		eng->core_fault = true;
	}
}

static void sha_publish(struct gd_engine *eng, bool final)
{
	uint8_t digest[32];
	uint32_t hsave[8];
	uint32_t blen = eng->sha_buf_len;
	memcpy(hsave, eng->sha_h, sizeof(hsave));
	sha256_final(eng, digest);
	if (!final) {
		memcpy(eng->sha_h, hsave, sizeof(hsave));
		eng->sha_buf_len = blen;
	}
	if (eng->dev->config.verbose) {
		gd_log(eng->dev, GD_LOG_DEBUG,
		       "device: sha-final pc=%#x total=%#llx stream=%u words=%08x %08x %08x %08x %08x %08x %08x %08x\n",
		       eng->pc, (unsigned long long)eng->sha_total_bits, eng->sha_stream,
		       eng->sha_h[0], eng->sha_h[1], eng->sha_h[2], eng->sha_h[3],
		       eng->sha_h[4], eng->sha_h[5], eng->sha_h[6], eng->sha_h[7]);
	}
	for (int i = 0; i < 8; i++)
		eng->sha_result[i] = (uint32_t)digest[i * 4] << 24 |
			(uint32_t)digest[i * 4 + 1] << 16 |
			(uint32_t)digest[i * 4 + 2] << 8 |
			(uint32_t)digest[i * 4 + 3];
	eng->sha_status = 0;
	eng->sha_mutex = 0;
	if (final) {
		eng->sha_started = false;
		eng->sha_buf_len = 0;
	}
}

static void sha_run(struct gd_engine *eng)
{
	uint32_t op = eng->sha_op;

	if ((op & 0x40u) && !(op & 0x01u)) {
		if (!eng->sha_started) {
			sha256_init(eng->sha_h);
			eng->sha_buf_len = 0;
			eng->sha_total_bits = eng->sha_msg_len;
		}
		sha_publish(eng, true);
	} else if (op & 0x01u) {
		if (!eng->sha_started) {
			sha256_init(eng->sha_h);
			eng->sha_buf_len = 0;
		}
		{
			uint64_t chunk_bits = (uint64_t)(eng->sha_in_hi & 0xffffffu) * 8u;
			sha_publish(eng, eng->sha_update_bits <= chunk_bits);
		}
	}
	eng->sha_op = op & ~(0x20u | 0x40u | 0x01u);
}

static uint32_t se_read(struct gd_engine *eng, uint32_t address)
{
	return gd_trng_read(&eng->trng, address);
}

static void pka_modexp(struct gd_engine *eng, uint32_t mod_addr, uint32_t exp_addr,
		       uint32_t base_addr, uint32_t out_addr);

static void se_write(struct gd_engine *eng, uint32_t address, uint32_t value)
{
	if (address >= sizeof(eng->se_regs))
		return;
	eng->se_regs[address / 4] = value;
	if (address == 0x4000 && (value & 0x80000000u)) {
		if (eng->se_regs[0x4004 / 4] == 0x14)
			pka_modexp(eng, 0x5000, 0x5400, 0x4400, 0x4400);
		eng->se_regs[0x4008 / 4] = 0;
		eng->se_regs[0x4020 / 4] = 0x40000000u;
	}
}


#define PKA_LIMBS 96

static int pka_cmp(const uint32_t *a, const uint32_t *b)
{
	for (int i = PKA_LIMBS - 1; i >= 0; i--) {
		if (a[i] != b[i])
			return a[i] > b[i] ? 1 : -1;
	}
	return 0;
}

static void pka_mulmod(const uint32_t a[PKA_LIMBS], const uint32_t b[PKA_LIMBS],
		       const uint32_t mod[PKA_LIMBS], uint32_t out[PKA_LIMBS])
{
	uint32_t prod[PKA_LIMBS * 2] = { 0 };
	uint32_t rem[PKA_LIMBS + 1] = { 0 };

	for (int i = 0; i < PKA_LIMBS; i++) {
		uint64_t carry = 0;
		for (int j = 0; j < PKA_LIMBS; j++) {
			uint64_t cur = (uint64_t)a[i] * b[j] + prod[i + j] + carry;
			prod[i + j] = (uint32_t)cur;
			carry = cur >> 32;
		}
		int k = i + PKA_LIMBS;
		while (carry && k < PKA_LIMBS * 2) {
			uint64_t cur = (uint64_t)prod[k] + carry;
			prod[k] = (uint32_t)cur;
			carry = cur >> 32;
			k++;
		}
	}

	for (int bit = PKA_LIMBS * 64 - 1; bit >= 0; bit--) {
		uint32_t carry = 0;
		for (int i = 0; i <= PKA_LIMBS; i++) {
			uint64_t v = ((uint64_t)rem[i] << 1) | carry;
			rem[i] = (uint32_t)v;
			carry = (uint32_t)(v >> 32);
		}
		rem[0] |= (prod[bit >> 5] >> (bit & 31)) & 1u;
		if (rem[PKA_LIMBS] || pka_cmp(rem, mod) >= 0) {
			uint32_t borrow = 0;
			for (int i = 0; i < PKA_LIMBS; i++) {
				uint64_t sub = (uint64_t)mod[i] + borrow;
				uint32_t old = rem[i];
				rem[i] = old - (uint32_t)sub;
				borrow = (uint64_t)old < sub;
			}
			rem[PKA_LIMBS] -= borrow;
		}
	}
	memcpy(out, rem, PKA_LIMBS * sizeof(uint32_t));
}

static void pka_modexp(struct gd_engine *eng, uint32_t mod_addr, uint32_t exp_addr,
		       uint32_t base_addr, uint32_t out_addr)
{
	if (eng->dev->config.verbose)
		gd_log(eng->dev, GD_LOG_DEBUG,
		       "device: pka-modexp pc=%#x mod=%#x exp=%#x base=%#x out=%#x\n",
		       eng->pc, mod_addr, exp_addr, base_addr, out_addr);
	uint32_t mod[PKA_LIMBS], exp[PKA_LIMBS], base[PKA_LIMBS], result[PKA_LIMBS] = { 0 };
	int high = -1;

	for (int i = 0; i < PKA_LIMBS; i++) {
		mod[i] = eng->se_regs[(mod_addr / 4) + i];
		exp[i] = eng->se_regs[(exp_addr / 4) + i];
		base[i] = eng->se_regs[(base_addr / 4) + i];
	}
	for (int i = PKA_LIMBS - 1; i >= 0 && high < 0; i--)
		for (int bit = 31; bit >= 0; bit--)
			if (exp[i] & (1u << bit)) {
				high = i * 32 + bit;
				break;
			}
	result[0] = 1;
	for (int bit = high; bit >= 0; bit--) {
		pka_mulmod(result, result, mod, result);
		if (exp[bit >> 5] & (1u << (bit & 31)))
			pka_mulmod(result, base, mod, result);
	}
	for (int i = 0; i < PKA_LIMBS; i++)
		eng->se_regs[(out_addr / 4) + i] = result[i];
	if (eng->dev->config.verbose)
		gd_log(eng->dev, GD_LOG_DEBUG, "device: pka-result pc=%#x first=%08x %08x %08x %08x\n",
		       eng->pc, result[0], result[1], result[2], result[3]);
}

static bool plm_mask_register(uint32_t address)
{
	switch (address) {
	case 0x823804: case 0x1fa7c4: case 0x1fa7cc: case 0x1fa7c8:
	case 0x9a0148: case 0x88ff4: case 0x88ab4: case 0x88ff8:
	case 0x8200fc: case 0x8200d8: case 0xc840: case 0xc848:
		return true;
	default:
		return false;
	}
}

static uint32_t plm_mask_for(uint32_t address)
{
	switch (address) {
	case 0x823818: case 0x82381c: case 0x823820:
		return 0x823804;
	/* NV_PFB_PRI_MMU_WPR2_ADDR_LO/HI__PRIV_LEVEL_MASK */
	case 0x1fa824: case 0x1fa828:
		return 0x1fa7cc;
	/* WPR1 and WPR_ALLOW are also gated by WPR_CFG_PLM. */
	case 0x1fa814: case 0x1fa818: case 0x1fa81c: case 0x1fa820:
		return 0x1fa7cc;
	/* NV_PFB_PRI_MMU_LOCK_ADDR_LO/HI__PRIV_LEVEL_MASK */
	case 0x1fa82c: case 0x1fa830:
		return 0x1fa7c8;
	/* NV_PFB_PRI_MMU_LOCAL_MEMORY_RANGE__PRIV_LEVEL_MASK */
	case 0x100ce0:
		return 0x1fa7c4;
	default:
		return 0;
	}
}

static void plm_latch_write(struct gd_device *dev, uint32_t address, uint32_t value)
{
	if (address == 0x823804) {
		dev->plm_feat_latched = value;
		if (value == 0xffffffffu)
			dev->target_write_seen = true;
		if ((value & 0x11u) == 0x11u)
			dev->plm_opened_seen = true;
	} else if (address == 0x1fa7cc) {
		dev->plm_wpr_cfg_latched = value;
		if ((value & 0xfffff0ffu) == 0xfffff0ffu)
			dev->target_write_seen = true;
		if (value & 0x10u)
			dev->plm_opened_seen = true;
	}
}

static bool plm_allowed(struct gd_device *dev, uint32_t address, int privilege, bool write)
{
	uint32_t mask_address, mask;
	int bit;

	if (plm_mask_register(address))
		return !write || privilege >= 3;
	mask_address = plm_mask_for(address);
	if (!mask_address)
		return true;
	mask = __atomic_load_n(&dev->bar[mask_address / 4], __ATOMIC_RELAXED);
	/* SOURCE_ENABLE[31:12] must name at least one source.  The individual
	 * source bits are not documented in the available GA102 headers. */
	if (!(mask >> 12))
		return false;
	bit = privilege + (write ? 4 : 0);
	return (mask >> bit) & 1;
}

/* Writable mask from the generated GA102 fixture.  Registers with a
 * partial mask (e.g. SS1's low nibble) must not store the reserved bits. */
static uint32_t bar_write_mask(uint32_t address)
{
	size_t i;
	for (i = 0; i < sizeof(gd_bar0_defaults) / sizeof(gd_bar0_defaults[0]); i++)
		if (gd_bar0_defaults[i].addr == address)
			return gd_bar0_defaults[i].mask;
	return 0xffffffffu;
}

static uint32_t bar0_raw_read(struct gd_device *dev, uint32_t address)
{
	uint32_t value;
	if (address >= PROM_OFFSET && address < PROM_OFFSET + PROM_SIZE) {
		const uint8_t *p = dev->prom + (address - PROM_OFFSET);
		return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
		       (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
	}
	pthread_mutex_lock(&dev->bar_lock);
	value = (address + 4 <= GD_BAR_SIZE && dev->bar_valid[address / 4]) ?
		dev->bar[address / 4] : 0;
	pthread_mutex_unlock(&dev->bar_lock);
	if (!(address + 4 <= GD_BAR_SIZE) || !dev->bar_valid[address / 4])
		gd_log(dev, GD_LOG_DEBUG, "device: unmodelled secure-bus read %#x -> 0\n", address);
	return value;
}

static void bar0_raw_write(struct gd_device *dev, uint32_t address, uint32_t value)
{
	value &= bar_write_mask(address);
	pthread_mutex_lock(&dev->bar_lock);
	if (address + 4 <= GD_BAR_SIZE) {
		dev->bar[address / 4] = value;
		dev->bar_valid[address / 4] = 1;
	}
	pthread_mutex_unlock(&dev->bar_lock);
}

static void csb_init(struct gd_device *dev)
{
	unsigned g;
	for (g = 0; g < 3; g++) {
		dev->csb.plm[g] = g == 2 ? 4u : 0u;
		/* Hardware-assigned 8-bit token; deliberately not 1. */
		dev->csb.id[g] = (uint8_t)(0x5a + g);
		memset(dev->csb.mutex[g], 0, sizeof(dev->csb.mutex[g]));
		dev->csb.released[g] = false;
	}
}

/* Clear every held SE common mutex.  Called by the Booter's release
 * primitive when it sets BAR0 0x1180f8 bit 24. */
static void csb_release_all(struct gd_device *dev)
{
	unsigned g, j;
	for (g = 0; g < 3; g++) {
		for (j = 0; j < 16; j++)
			dev->csb.mutex[g][j] = 0;
		dev->csb.released[g] = true;
	}
}

static bool csb_mutex_slot(uint32_t addr, unsigned *group, unsigned *slot)
{
	uint32_t rem;
	if (addr < 0x8e00 || addr >= 0x8f00)
		return false;
	rem = addr - 0x8e00;
	*group = rem / 0x50;
	rem %= 0x50;
	if (rem < 0x0c || rem >= 0x0c + 16 * 4 || ((rem - 0x0c) & 3))
		return false;
	*slot = (rem - 0x0c) / 4;
	return *group < 3;
}

static bool csb_allowed(struct gd_device *dev, unsigned g, int priv, bool write)
{
	uint32_t plm = dev->csb.plm[g];
	if (priv >= 3)
		return true;
	return (plm >> ((priv & 3) + (write ? 4 : 0))) & 1;
}

static uint32_t csb_read(struct gd_device *dev, uint32_t addr, int priv)
{
	unsigned g, slot;
	uint32_t rem;
	if (addr >= 0x8e00 && addr < 0x8f00) {
		rem = (addr - 0x8e00) % 0x50;
		g = (addr - 0x8e00) / 0x50;
		if (!csb_allowed(dev, g, priv, false)) {
			dev->denied_writes++;
			return 0xbadf0000u;
		}
		if (rem == 0x00)
			return dev->csb.plm[g];
		if (rem == 0x04)
			return dev->csb.id[g];
		if (rem == 0x08)
			return 0;
		if (csb_mutex_slot(addr, &g, &slot))
			return dev->csb.mutex[g][slot];
		return 0;
	}
	return bar0_raw_read(dev, addr);
}

static void csb_write(struct gd_device *dev, uint32_t addr, uint32_t value, int priv)
{
	unsigned g, slot, j;
	uint32_t rem;
	if (addr >= 0x8e00 && addr < 0x8f00) {
		rem = (addr - 0x8e00) % 0x50;
		g = (addr - 0x8e00) / 0x50;
		if (!csb_allowed(dev, g, priv, true)) {
			dev->denied_writes++;
			dev->plm_violations++;
			return;
		}
		if (rem == 0x00) {
			dev->csb.plm[g] = value;
			return;
		}
		if (rem == 0x04)
			return; /* read-only ID */
		if (rem == 0x08) {
			/* ID_RELEASE: returning a token releases it and any
			 * slot still carrying it. */
			if ((value & 0xffu) == dev->csb.id[g]) {
				for (j = 0; j < 16; j++)
					if (dev->csb.mutex[g][j] == (value & 0xffu))
						dev->csb.mutex[g][j] = 0;
				dev->csb.released[g] = true;
			}
			return;
		}
		if (csb_mutex_slot(addr, &g, &slot)) {
			dev->csb.mutex[g][slot] = value & 0xffu;
			return;
		}
		return;
	}
	bar0_raw_write(dev, addr, value);
}

static uint8_t *fb_window(struct gd_device *dev, uint64_t addr, size_t size)
{
	if (!dev->fb_base || addr < dev->fb_base || addr - dev->fb_base > dev->fb_size ||
	    size > dev->fb_size - (addr - dev->fb_base))
		return NULL;
	return dev->fb + (addr - dev->fb_base);
}

static uint32_t dio_read(struct gd_device *dev, struct gd_engine *eng, uint32_t addr, int priv)
{
	addr &= 0xffffu;
	if (addr >= 0x8e00 && addr < 0x8f00)
		return csb_read(dev, addr, priv);
	return se_read(eng, addr);
}

static void dio_write(struct gd_device *dev, struct gd_engine *eng, uint32_t addr, uint32_t value, int priv)
{
	addr &= 0xffffu;
	if (addr >= 0x8e00 && addr < 0x8f00)
		csb_write(dev, addr, value, priv);
	else
		se_write(eng, addr, value);
}

/* Falcon per-register privilege masks (NV_PFALCON_FALCON_*__PRIV_LEVEL_MASK).
 * Offsets are identical for every Falcon engine; the mask register itself is
 * L3-write-only and its low bits gate the protected register. */
static uint32_t engine_plm_off(uint32_t off)
{
	switch (off) {
	case 0x3c0: return 0x3c4; /* ENGINE -> RESET_PRIV_LEVEL_MASK */
	case 0x100: case 0x130: return 0x288; /* CPUCTL/ALIAS */
	case 0x104: return 0x194; /* BOOTVEC */
	case 0x10c: return 0x28c; /* DMACTL */
	case 0x110: case 0x114: case 0x118: case 0x11c: case 0x128:
		return 0x190; /* DMATRF* */
	case 0x180: case 0x184: case 0x188: return 0x18c; /* PMB IMEM */
	case 0x1c0: case 0x1c4: case 0x1c8: case 0x1cc: return 0x2b0; /* PMB DMEM */
	default: return 0;
	}
}

static bool engine_plm_allowed(struct gd_engine *eng, uint32_t off, int priv, bool write)
{
	uint32_t mask_off = engine_plm_off(off);
	uint32_t mask;

	if (!mask_off)
		return true;
	mask = eng->io[mask_off / 4];
	if (off == mask_off) {
		/* A PLM register cannot be opened by the level it gates. */
		if (write)
			return priv >= 3;
		return (mask >> (priv & 3)) & 1;
	}
	if (!(mask >> 12))
		return false;
	return (mask >> ((priv & 3) + (write ? 4 : 0))) & 1;
}

static uint32_t engine_read_locked(struct gd_device *dev, struct gd_engine *eng, uint32_t off, int priv);
static bool engine_write_locked(struct gd_device *dev, struct gd_engine *eng, uint32_t off, uint32_t value, int priv);

/* The firmware reaches the same Falcon registers through the secure-bus
 * (CSEC CSB) view, whose offsets differ from the host PFALCON view.  Translate
 * the CSB aliases onto the one engine state so firmware writes and host reads
 * observe the same registers.  dev_sec_csb.h carries the offsets; the PFALCON
 * view is defined in dev_falcon_v4.h. */
static uint32_t csb_alias(uint32_t offset)
{
	switch (offset) {
	case 0x1000: return OFF_MAILBOX0;
	case 0x1100: return OFF_MAILBOX1;
	case 0x2100: return 0x084;          /* FALCON_RM */
	case 0x3d00: return OFF_HWCFG2;
	case 0x4000: return OFF_CPUCTL;
	case 0x4100: return OFF_BOOTVEC;
	case 0x4300: return OFF_DMACTL;
	case 0x4200: return OFF_HWCFG;
	case 0x4400: return OFF_DMABASE;
	case 0x4500: return OFF_DMAMOFFS;
	case 0x4600: return OFF_DMACMD;
	case 0x4700: return OFF_DMAFBOFFS;
	case 0x4a00: return OFF_DMABASE1;
	case 0xf000: return OFF_ENGINE;
	case 0xf100: return OFF_RESET_PLM;
	default: return offset;
	}
}

static uint32_t local_io(struct gd_device *dev, struct gd_engine *eng, uint32_t offset, uint32_t value, int write)
{
	uint32_t result;
	uint32_t alias = csb_alias(offset & 0xffffffu);
	if (dev->config.verbose)
		gd_log(dev, GD_LOG_DEBUG, "device: io %s pc=%#x off=%#x\n",
		       write ? "w" : "r", eng->pc, offset);
	pthread_mutex_lock(&eng->lock);
	if (write) {
		engine_write_locked(dev, eng, alias & (GD_IO_SIZE - 1), value, 3);
		result = value;
	} else {
		result = engine_read_locked(dev, eng, alias & (GD_IO_SIZE - 1), 3);
	}
	pthread_mutex_unlock(&eng->lock);
	return result;
}

/* Keep the last 64 executed PCs per engine so a halt or fault can be
 * reconstructed without a per-instruction trace.  The ring is written from
 * the executing thread only. */
static void engine_pc_history(struct gd_device *dev, struct gd_engine *eng,
			      uint32_t reason, uint32_t pc)
{
	if (eng->index == 1 && reason == 1) {
		uint32_t o;
		GD_DIAG( "diag: fwsec-final dmem:");
		for (o = 0; o < 0x20; o += 4) GD_DIAG( " %08x", *(uint32_t *)(eng->dmem+o));
		GD_DIAG( "\n hdr2:");
		for (o = 0x4e0; o < 0x520; o += 4) GD_DIAG( " %08x", *(uint32_t *)(eng->dmem+o));
		GD_DIAG( "\n digest:");
		for (o = 0x35f0; o < 0x3610; o += 4) GD_DIAG( " %08x", *(uint32_t *)(eng->dmem+o));
		GD_DIAG( "\n mac:");
		for (o = 0x540; o < 0x560; o += 4) GD_DIAG( " %08x", *(uint32_t *)(eng->dmem+o));
		GD_DIAG( "\n out:");
		for (o = 0x8df0; o < 0x8e10; o += 4) GD_DIAG( " %08x", *(uint32_t *)(eng->dmem+o));
		GD_DIAG( "\n seed:");
		for (o = 0xd710; o < 0xd720; o += 4) GD_DIAG( " %08x", *(uint32_t *)(eng->imem+o));
		GD_DIAG( " fifo in=%u out=%u seen=%d/%d kdf:", eng->crypto_stream_in_count,
			eng->crypto_stream_out_count, eng->fwsec_kdf_seen, eng->fwsec_digest_seen);
		for (o = 0; o < 16; o += 4) GD_DIAG( " %08x", *(uint32_t *)(eng->fwsec_kdf_out+o));
		GD_DIAG( " expect:");
		for (o = 0; o < 32; o += 4) GD_DIAG( " %08x", *(uint32_t *)(eng->fwsec_expect+o));
		GD_DIAG( " digest:");
		for (o = 0; o < 16; o += 4) GD_DIAG( " %08x", *(uint32_t *)(eng->fwsec_digest+o));
		GD_DIAG( "\n");
	}
	if (eng->index == 0 && reason == 3) {
		uint32_t i;
		GD_DIAG( "diag: retring:");
		for (i = 0; i < 32; i++) {
			uint32_t at = (eng->ret_at - 32 + i) & 63;
			(void)at;
			GD_DIAG( " [%#x->%#x sp=%#x]", eng->ret_pc[at], eng->ret_val[at], eng->ret_sp[at]);
		}
		GD_DIAG( "\n");
		GD_DIAG( "diag: brring:");
		for (i = 0; i < 32; i++) {
			uint32_t at = (eng->br_at - 32 + i) & 63;
			(void)at;
			GD_DIAG( " [%#x->%#x sp=%#x]", eng->br_pc[at], eng->br_tgt[at], eng->br_sp[at]);
		}
		GD_DIAG( "\n");
	}
	uint32_t n = eng->pc_ring_at < 64 ? eng->pc_ring_at : 64;
	uint32_t first = eng->pc_ring_at - n;
	uint32_t i;

	eng->halt_pc = pc;
	eng->halt_reason = reason;
	trace_add(dev, GD_TRACE_PC, eng->index + 1, 0x10000u | reason, pc, pc);
	for (i = 0; i < n; i++)
		trace_add(dev, GD_TRACE_PC, eng->index + 1, i,
			  eng->pc_ring[(first + i) & 63], 0);
}

static int engine_execute(struct gd_device *dev, struct gd_engine *eng, unsigned budget)
{
	unsigned step;
	uint32_t pc = eng->bootvec & 0xffffu;

	for (step = 0; step < budget; step++) {
		const struct gd_code_rec *rec;
		uint32_t d, a, b, result = 0, m, target, address;
		int sz, overflow = 0, carry = 0, taken = 0;
		int32_t index;

		eng->pc_ring[eng->pc_ring_at++ & 63] = pc;
		if (eng->index == 0 && (pc == 0x1fd4 || pc == 0x1fd7 || pc == 0x1fdb)) {
			uint32_t i;
			GD_DIAG( "diag: at1fd pc=%#x prev:", pc);
			for (i = 1; i <= 10; i++)
				GD_DIAG( " %#x", eng->pc_ring[(eng->pc_ring_at - 1 - i) & 63]);
			GD_DIAG( " sp=%#x depth=%u r15=%#x\n", eng->sp, eng->call_depth, eng->r[15]);
		}
		if (eng->index == 1 && (pc == 0x988 || pc == 0xa69 || pc == 0xa6c || pc == 0xa72)) {
			GD_DIAG( "diag: seq pc=%#x r10=%#x r11=%#x r14=%#x r9=%#x trap=%d pend=%u in=%u out=%u\n",
				pc, eng->r[10], eng->r[11], eng->r[14], eng->r[9],
				eng->xfer_trap, eng->seq_pending,
				eng->crypto_stream_in_count, eng->crypto_stream_out_count);
			if (pc == 0xa6c) {
				uint32_t off = eng->r[14] & 0xffff;
				(void)off;
				GD_DIAG( "diag: seq imem@%#x:", off);
				for (uint32_t k = 0; k < 16; k += 4) GD_DIAG( " %08x", *(uint32_t *)(eng->imem+off+k));
				GD_DIAG( "\n");
			}
		}
		if (eng->index == 0 && pc == 0x576b) {
			uint32_t i;
			GD_DIAG( "diag: at576b prev:");
			for (i = 1; i <= 8; i++)
				GD_DIAG( " %#x", eng->pc_ring[(eng->pc_ring_at - 1 - i) & 63]);
			GD_DIAG( " sp=%#x depth=%u\n", eng->sp, eng->call_depth);
		}
		if (eng->index == 0 && pc == 0x1fa3) {
			uint32_t i;
			GD_DIAG( "diag: at1fa3 prev:");
			for (i = 1; i <= 10; i++)
				GD_DIAG( " %#x", eng->pc_ring[(eng->pc_ring_at - 1 - i) & 63]);
			GD_DIAG( " sp=%#x depth=%u\n", eng->sp, eng->call_depth);
		}
		if (eng->index == 0 && (pc == 0x330f || pc == 0x576b || pc == 0x57b2))
			GD_DIAG( "diag: callframe pc=%#x sp=%#x w0=%#x w4=%#x w8=%#x depth=%u\n",
				pc, eng->sp, *(uint32_t *)(eng->dmem+((eng->sp)&0xffff)),
				*(uint32_t *)(eng->dmem+((eng->sp+4)&0xffff)),
				*(uint32_t *)(eng->dmem+((eng->sp+8)&0xffff)), eng->call_depth);
		if (eng->index == 0 && (pc == 0x15cd || pc == 0x193b || pc == 0x1940))
			GD_DIAG( "diag: retpath pc=%#x sp=%#x w0=%#x w4=%#x depth=%u\n",
				pc, eng->sp, *(uint32_t *)(eng->dmem+((eng->sp)&0xffff)),
				*(uint32_t *)(eng->dmem+((eng->sp+4)&0xffff)), eng->call_depth);
		if (eng->index == 0 && (pc == 0x29d3 || pc == 0x28b1 || pc == 0x28b8 || pc == 0x28aa)) {
			uint32_t i;
			GD_DIAG( "diag: errsite %#x r0=%#x r4=%#x r1=%#x r14=%#x r15=%#x r12=%#x r13=%#x stack:",
				pc, eng->r[0], eng->r[4], eng->r[1], eng->r[14], eng->r[15], eng->r[12], eng->r[13]);
			for (i = 0; i < eng->call_depth && i < 12; i++)
				GD_DIAG( " %#x", eng->call_pc[i]);
			GD_DIAG( " last:");
			for (i = 0; i < 64; i++)
				GD_DIAG( " %#x", eng->pc_ring[(eng->pc_ring_at - 64 + i) & 63]);
			GD_DIAG( "\n wpr2lo=%08x wpr2hi=%08x wprplm=%08x mmucfg=%08x g03=%08x g6a=%08x g6b=%08x\n",
				dev->bar[0x1fa824/4], dev->bar[0x1fa828/4], dev->bar[0x1fa7c4/4],
				dev->bar[0x1fa7c8/4], dev->bar[0x11821c/4], dev->bar[0x118244/4],
				dev->bar[0x118248/4]);
			GD_DIAG( " d400=%08x %08x d500=%08x %08x d504=%08x d1650=%08x %08x\n",
				*(uint32_t *)(eng->dmem+0x400), *(uint32_t *)(eng->dmem+0x404),
				*(uint32_t *)(eng->dmem+0x500), *(uint32_t *)(eng->dmem+0x504),
				*(uint32_t *)(eng->dmem+0x508),
				*(uint32_t *)(eng->dmem+0x1650), *(uint32_t *)(eng->dmem+0x1658));
		}
		if (eng->index == 0 && (pc == 0x18f9 || pc == 0x1900 || pc == 0x1907 ||
					pc == 0x190e || pc == 0x1915 || pc == 0x191c)) {
			uint32_t b4 = eng->r[4];
			(void)b4;
			GD_DIAG( "diag: errsite %#x r4=%#x d34=%#x d30=%#x d3c=%#x d38=%#x d44=%#x d40=%#x stack:",
				pc, b4,
				(b4+0x34 < 0x10000) ? *(uint32_t *)(eng->dmem+b4+0x34) : 0,
				(b4+0x30 < 0x10000) ? *(uint32_t *)(eng->dmem+b4+0x30) : 0,
				(b4+0x3c < 0x10000) ? *(uint32_t *)(eng->dmem+b4+0x3c) : 0,
				(b4+0x38 < 0x10000) ? *(uint32_t *)(eng->dmem+b4+0x38) : 0,
				(b4+0x44 < 0x10000) ? *(uint32_t *)(eng->dmem+b4+0x44) : 0,
				(b4+0x40 < 0x10000) ? *(uint32_t *)(eng->dmem+b4+0x40) : 0);
			for (uint32_t di = 0; di < eng->call_depth && di < 12; di++)
				GD_DIAG( " %#x", eng->call_pc[di]);
			GD_DIAG( "\n");
		}
		if (!eng->code_index || pc + 1 > 0x10000) {
			dev->fault_pc = pc;
			device_error(dev, "code fetch outside decoded table", pc, 0);
			eng->core_fault = true;
			engine_pc_history(dev, eng, 3, pc);
			return -EFAULT;
		}
		index = eng->code_index[pc];
		if (index < 0) {
			uint32_t opcode = (uint32_t)eng->imem[pc] | (uint32_t)eng->imem[(pc + 1) & 0xffff] << 8;
			dev->fault_pc = pc;
			dev->fault_opcode = opcode;
			device_error(dev, "no decoded instruction", pc, opcode);
			eng->core_fault = true;
			engine_pc_history(dev, eng, 3, pc);
			return -EINVAL;
		}
		rec = &eng->code[index];
		if (dev->config.verbose && pc == 0x8fc) {
			gd_log(dev, GD_LOG_DEBUG, "device: frts head %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
			       eng->dmem[0], eng->dmem[1], eng->dmem[2], eng->dmem[3], eng->dmem[4],
			       eng->dmem[5], eng->dmem[6], eng->dmem[7], eng->dmem[8], eng->dmem[9]);
		}
		if (dev->config.verbose && (pc == 0x1382 || pc == 0x13a5 || pc == 0x13c9 || pc == 0x22b || pc == 0x235 || pc == 0x23f || pc == 0x241 || pc == 0x271)) {
			gd_log(dev, GD_LOG_DEBUG, "device: status pc=%#x r10=%#x r0=%#x\n", pc, eng->r[10], eng->r[0]);
		}
		if (dev->config.verbose && (pc == 0x1c74 || pc == 0x1ed4)) {
			uint32_t v[8];
			for (int i = 0; i < 8; i++)
				memcpy(&v[i], eng->dmem + 0x3000 + i * 4, 4);
			gd_log(dev, GD_LOG_DEBUG,
			       "device: page-state pc=%#x r10=%#x r11=%#x r12=%#x r13=%#x d3000=%08x d3004=%08x d3010=%08x d3014=%08x d301c=%08x d3020=%08x d3024=%08x\n",
			       pc, eng->r[10], eng->r[11], eng->r[12], eng->r[13],
			       v[0], v[1], v[4], v[5], v[7], *(uint32_t *)(eng->dmem+0x3020), *(uint32_t *)(eng->dmem+0x3024));
			for (unsigned di = 0; di < dev->dma_count; di++)
				if (dev->dma[di].live)
					gd_log(dev, GD_LOG_DEBUG, "device: dma-span %u addr=%#llx size=%#zx\n",
					       di, (unsigned long long)dev->dma[di].address, dev->dma[di].size);
		}
		if (dev->config.verbose && (pc == 0xa8dd || pc == 0xa95b || pc == 0xa9d2 ||
					   pc == 0xac3b || pc == 0xa6d1)) {
			gd_log(dev, GD_LOG_DEBUG,
			       "device: sha-call pc=%#x r0=%#x r1=%#x r2=%#x r3=%#x r10=%#x r11=%#x r12=%#x r13=%#x r14=%#x in=%#x len=%#x\n",
			       pc, eng->r[0], eng->r[1], eng->r[2], eng->r[3], eng->r[10], eng->r[11],
			       eng->r[12], eng->r[13], eng->r[14], eng->sha_in_addr, eng->sha_msg_len);
		}
		if (dev->config.verbose && pc == 0xd0a1) {
			uint32_t a = 0, b = 0;
			if (0x8e04 + 8 < sizeof(eng->dmem)) {
				memcpy(&a, eng->dmem + 0x8e04, 4);
				memcpy(&b, eng->dmem + 0x8e08, 4);
			}
			gd_log(dev, GD_LOG_DEBUG, "device: stage-buffers d8e04=%08x d8e08=%08x d3600=%08x\n",
			       a, b, *(uint32_t *)(eng->dmem + 0x3600));
		}
		if (dev->config.verbose && (pc == 0xcfec || pc == 0xd003 || pc == 0xd08a)) {
			uint32_t desc = eng->r[pc == 0xcfec ? 10 : 6];
			uint32_t off = 0, len = 0;
			if (desc + 12 < sizeof(eng->dmem)) {
				memcpy(&off, eng->dmem + desc + 4, 4);
				memcpy(&len, eng->dmem + desc + 8, 4);
			}
			gd_log(dev, GD_LOG_DEBUG,
			       "device: cert-desc pc=%#x r10=%#x r11=%#x r6=%#x desc=%#x off=%#x len=%#x\n",
			       pc, eng->r[10], eng->r[11], eng->r[6], desc, off, len);
		}
		if (dev->config.verbose && (pc == 0xd08a || pc == 0xd0a1 || pc == 0x1b3c ||
					   pc == 0xa215 || pc == 0xd445 || pc == 0xd49a || pc == 0xd4f6 ||
					   pc == 0xd50f || pc == 0xd51a)) {
			uint32_t h = 0;
			if (eng->r[10] + 4 <= sizeof(eng->dmem))
				memcpy(&h, eng->dmem + eng->r[10], 4);
			gd_log(dev, GD_LOG_DEBUG,
			       "device: stage pc=%#x r0=%#x r1=%#x r2=%#x r3=%#x r4=%#x r5=%#x r10=%#x r11=%#x r12=%#x hash=%08x\n",
			       pc, eng->r[0], eng->r[1], eng->r[2], eng->r[3], eng->r[4], eng->r[5],
			       eng->r[10], eng->r[11], eng->r[12], h);
		}
		if (dev->config.verbose && (pc == 0xa09c || pc == 0xa0a0 || pc == 0xa0a4 || pc == 0xe33)) {
			unsigned i;
			gd_log(dev, GD_LOG_DEBUG, "device: sig-check pc=%#x r10=%#x r11=%#x r12=%#x\n",
			       pc, eng->r[10], eng->r[11], eng->r[12]);
			for (i = 0; i < 32 && i < eng->r[12] && eng->r[10] + i < sizeof(eng->dmem); i += 4) {
				uint32_t av = 0, bv = 0;
				memcpy(&av, eng->dmem + eng->r[10] + i, 4);
				if (eng->r[11] + i < sizeof(eng->dmem))
					memcpy(&bv, eng->dmem + eng->r[11] + i, 4);
				gd_log(dev, GD_LOG_DEBUG, "device: sig-data %02x %08x %08x\n", i, av, bv);
			}
		}
		if (dev->config.verbose && (pc == 0xb6ef || pc == 0xb75c || pc == 0xa9b2 ||
					   pc == 0xaa3b || pc == 0xaa4c)) {
			gd_log(dev, GD_LOG_DEBUG, "device: master-check pc=%#x r1=%#x r2=%#x d0=%#x d1=%#x d2=%#x d3=%#x\n",
			       pc, eng->r[1], eng->r[2],
			       eng->r[1] + 0 < sizeof(eng->dmem) ? *(uint32_t *)(eng->dmem + eng->r[1]) : 0,
			       eng->r[1] + 4 < sizeof(eng->dmem) ? *(uint32_t *)(eng->dmem + eng->r[1] + 4) : 0,
			       eng->r[1] + 8 < sizeof(eng->dmem) ? *(uint32_t *)(eng->dmem + eng->r[1] + 8) : 0,
			       eng->r[1] + 12 < sizeof(eng->dmem) ? *(uint32_t *)(eng->dmem + eng->r[1] + 12) : 0);
		}
		if (dev->config.verbose && (pc == 0xd15d || pc == 0xd168 || pc == 0xd16c)) {
			unsigned off;
			gd_log(dev, GD_LOG_DEBUG, "device: cert-check pc=%#x r0=%#x r2=%#x r10=%#x r11=%#x r12=%#x\n",
			       pc, eng->r[0], eng->r[2], eng->r[10], eng->r[11], eng->r[12]);
			for (off = 0; off < 0x20; off += 4) {
				uint32_t a = eng->r[0] + off, b = eng->r[2] + 0x14 + off;
				gd_log(dev, GD_LOG_DEBUG, "device: cert hash %02x %08x %08x\n", off,
				       a < sizeof(eng->dmem) ? *(uint32_t *)(eng->dmem + a) : 0,
				       b < sizeof(eng->dmem) ? *(uint32_t *)(eng->dmem + b) : 0);
			}
		}
		if (dev->config.verbose && (pc == 0x2e6 || pc == 0x2ea || pc == 0x2f4 ||
					   pc == 0x74c || pc == 0x2e2)) {
			gd_log(dev, GD_LOG_DEBUG,
			       "device: fwsec-check pc=%#x r0=%#x r10=%#x r11=%#x r15=%#x\n",
			       pc, eng->r[0], eng->r[10], eng->r[11], eng->r[15]);
		}
		if (dev->config.verbose && pc == 0x959) {
			int i;
			gd_log(dev, GD_LOG_DEBUG, "device: sigcmp r1=%#x r10=%#x r11=%#x r12=%#x\n",
			       eng->r[1], eng->r[10], eng->r[11], eng->r[12]);
			gd_log(dev, GD_LOG_DEBUG, "device: sig expected:");
			for (i = 0; i < 16; i++)
				gd_log(dev, GD_LOG_DEBUG, " %02x", eng->dmem[0x540 + i]);
			gd_log(dev, GD_LOG_DEBUG, "\n");
			gd_log(dev, GD_LOG_DEBUG, "device: sig computed:");
			for (i = 0; i < 16; i++)
				gd_log(dev, GD_LOG_DEBUG, " %02x", eng->dmem[0x8df0 + i]);
			gd_log(dev, GD_LOG_DEBUG, "\n");
		}
		sz = rec->sz ? rec->sz : 32;
		m = mask_of(sz);
		d = rec->d; a = rec->a; b = rec->b;
		target = rec->target;
		switch (rec->op) {
		case 1: /* mov */
			if (rec->sized) set_r(eng, d, eng->r[a], sz);
			else eng->r[d] = a;
			break;
		case 2: /* sethi */
			eng->r[d] = (eng->r[d] & 0xffffu) | (a << 16);
			break;
		case 3: case 4: case 5: case 6: { /* add adc sub sbb */
			uint32_t x = eng->r[a] & m;
			uint32_t y = (rec->bimm ? b : eng->r[b]) & m;
			uint32_t cin = (rec->op == 4 || rec->op == 6) ? (eng->flags >> 8) & 1 : 0;
			uint64_t wide;
			if (rec->op == 5 || rec->op == 6) {
				wide = (uint64_t)x - y - cin;
				carry = (uint64_t)x < (uint64_t)y + cin;
				result = (uint32_t)wide & m;
				overflow = (((x ^ y) & (x ^ result)) >> (sz - 1)) & 1;
			} else {
				wide = (uint64_t)x + y + cin;
				carry = (wide >> sz) & 1;
				result = (uint32_t)wide & m;
				overflow = (((x ^ result) & (y ^ result)) >> (sz - 1)) & 1;
			}
			set_r(eng, d, result, sz);
			set_flags(eng, result, sz, overflow, carry);
			break;
		}
		case 7: case 8: case 9: /* cmpu cmps cmp */
			a = eng->r[a] & m;
			b = (rec->bimm ? b : eng->r[b]) & m;
			result = (a - b) & m;
			if (rec->op == 8) {
				int64_t sa = (int64_t)a - ((uint64_t)(a >> (sz - 1)) << sz);
				int64_t sb = (int64_t)b - ((uint64_t)(b >> (sz - 1)) << sz);
				carry = sa < sb;
			} else carry = a < b;
			if (rec->op == 9) {
				overflow = (((a ^ b) & (a ^ result)) >> (sz - 1)) & 1;
				set_flags(eng, result, sz, overflow, carry);
			} else eng->flags = (eng->flags & ~0x900u) | (uint32_t)carry << 8 | (result == 0) << 11;
			break;
		case 10: case 11: case 12: case 13: case 14: { /* shifts */
			uint32_t x = eng->r[a] & m;
			uint32_t n = (rec->bimm ? b : eng->r[b]) & (sz - 1);
			if (rec->op == 10 || rec->op == 13) {
				result = (x << n) & m;
				if (rec->op == 13 && n)
					result |= ((eng->flags >> 8) & 1) << (n - 1);
				carry = n ? (x >> (sz - n)) & 1 : 0;
			} else {
				result = x >> n;
				if (rec->op == 14 && n)
					result |= ((eng->flags >> 8) & 1) << (sz - n);
				if (rec->op == 12 && (x >> (sz - 1)))
					result |= (uint32_t)((uint64_t)m << (sz - n)) & m;
				carry = n ? (x >> (n - 1)) & 1 : 0;
			}
			set_r(eng, d, result, sz);
			set_flags(eng, result, sz, 0, carry);
			break;
		}
		case 15: case 16: case 17: case 18: case 19:
			a = eng->r[a] & m;
			if (rec->op == 18) { set_r(eng, d, 0, sz); break; }
			if (rec->op == 15) result = ~a & m;
			else if (rec->op == 16) result = -a & m;
			else if (rec->op == 17) result = ((a >> (sz / 2)) | (a << (sz / 2))) & m;
			else result = a;
			if (rec->op != 19) set_r(eng, d, result, sz);
			set_flags(eng, result, sz, rec->op == 16 && result == (1u << (sz - 1)), -1);
			break;
		case 20: case 21: { /* ld st */
			uint32_t base = rec->base == 16 ? eng->sp : eng->r[rec->base];
			address = base + (rec->idx >= 0 ? eng->r[rec->idx] * (uint32_t)rec->scale : (uint32_t)rec->off);
			if (rec->space) {
				if (rec->op == 20) {
					result = local_io(dev, eng, address, 0, 0);
					set_r(eng, d, result, sz);
				} else {
					local_io(dev, eng, address, eng->r[a], 1);
				}
			} else if (rec->op == 20) {
				result = dmem_read(eng, address, sz);
				set_r(eng, d, result, sz);
			} else {
				dmem_write(eng, address, sz, eng->r[a]);
			}
			break;
		}
		case 22: case 23:
			a = eng->r[a]; b = rec->bimm ? b : eng->r[b];
			eng->r[d] = rec->op == 22 ? (a & 0xffffu) * (b & 0xffffu) :
				(uint32_t)((int32_t)(int16_t)a * (int32_t)(int16_t)b);
			break;
		case 24:
			a = eng->r[a]; b = (rec->bimm ? b : eng->r[b]) & 31;
			result = a & ((1u << b) - 1);
			if ((a >> b) & 1) result |= 0xffffffffu << b;
			eng->r[d] = result;
			eng->flags = (eng->flags & ~0xc00u) | (result >> 31) << 10 | (result == 0) << 11;
			break;
		case 25: case 26: {
			uint32_t low, sm1, bf, fill;
			a = eng->r[a]; b = rec->bimm ? b : eng->r[b];
			low = b & 31; sm1 = (b >> 5) & 31;
			bf = (a >> low) & ((2u << sm1) - 1);
			fill = rec->op == 26 ? (a >> ((low + sm1) & 31)) & 1 : 0;
			if (fill) bf |= (uint32_t)(-(2u << sm1));
			eng->r[d] = bf;
			eng->flags = (eng->flags & ~0xc00u) | fill << 10 | (bf == 0) << 11;
			break;
		}
		case 27: {
			uint32_t low, size, fm;
			b = rec->bimm ? b : eng->r[b];
			low = b & 31; size = ((b >> 5) & 31) + 1;
			if (low + size <= 32) {
				fm = ((1u << size) - 1) << low;
				eng->r[d] = (eng->r[d] & ~fm) | ((eng->r[a] << low) & fm);
			}
			break;
		}
		case 28: case 29: case 30:
			a = eng->r[a]; b = rec->bimm ? b : eng->r[b];
			result = rec->op == 28 ? a & b : rec->op == 29 ? a | b : a ^ b;
			eng->r[d] = result;
			set_flags(eng, result, 32, 0, 0);
			break;
		case 31: case 32:
			b = (rec->bimm ? b : eng->r[b]) & 31;
			result = ((rec->op == 31 ? eng->r[a] : eng->flags) >> b) & 1;
			eng->r[d] = result;
			eng->flags = (eng->flags & ~0xc00u) | (result == 0) << 11;
			break;
		case 33: case 34: case 35:
			b = 1u << ((rec->bimm ? b : eng->r[b]) & 31);
			if (rec->op == 33) eng->r[d] |= b;
			else if (rec->op == 34) eng->r[d] &= ~b;
			else eng->r[d] ^= b;
			break;
		case 36: case 37:
			a = eng->r[a]; b = rec->bimm ? b : eng->r[b];
			eng->r[d] = rec->op == 36 ? (b ? a / b : 0xffffffffu) : (b ? a % b : a);
			break;
		case 38: case 39: case 40:
			if (rec->op == 39)
				taken = ((eng->r[a] & m) == (uint32_t)b) != !!d;
			else if (rec->op == 38)
				taken = condition(eng->flags, d);
			else
				taken = 1;
			if (taken) {
				if (rec->op == 40) {
					eng->br_pc[eng->br_at & 63] = pc;
					eng->br_tgt[eng->br_at & 63] = rec->bimm ? (uint32_t)a : eng->r[a];
					eng->br_sp[eng->br_at & 63] = eng->sp;
					eng->br_at++;
					if (eng->index == 0 && (eng->br_tgt[(eng->br_at - 1) & 63] == 0x576b ||
								eng->br_tgt[(eng->br_at - 1) & 63] == 0x1fa3))
						GD_DIAG( "diag: enter-jmp pc=%#x tgt=%#x sp=%#x\n",
							pc, eng->br_tgt[(eng->br_at - 1) & 63], eng->sp);
					pc = (rec->bimm ? (uint32_t)a : eng->r[a]) & 0xffff;
				}
				else
					pc = target & 0xffff;
			} else {
				pc = (pc + rec->len) & 0xffff;
			}
			break;
		case 41:
			eng->sp = (eng->sp + (rec->bimm ? a : eng->r[a])) & ~3u;
			break;
		case 42: break;
		case 43: /* movsr */
			if (d == 4) eng->sp = eng->r[a] & ~3u;
			else if (d == 5) eng->pc = rec->pc;
			else eng->sr[d] = eng->r[a];
			break;
		case 44: /* movrs */
			if (a == 4) eng->r[d] = eng->sp;
			else if (a == 5) eng->r[d] = rec->pc;
			else if (a == 8) eng->r[d] = eng->flags;
			else eng->r[d] = eng->sr[a];
			break;
		case 45: case 65: case 66: /* call lbra lcall */
			eng->br_pc[eng->br_at & 63] = pc;
			eng->br_tgt[eng->br_at & 63] = (rec->bimm ? (uint32_t)a : eng->r[a]);
			eng->br_sp[eng->br_at & 63] = eng->sp;
			eng->br_at++;
			if (eng->index == 0 && eng->br_tgt[(eng->br_at - 1) & 63] >= 0x5760 &&
			    eng->br_tgt[(eng->br_at - 1) & 63] <= 0x57b5)
				GD_DIAG( "diag: call-into-57 pc=%#x tgt=%#x op=%d sp=%#x\n",
					pc, eng->br_tgt[(eng->br_at - 1) & 63], rec->op, eng->sp);
			if (eng->index == 0 && (eng->br_tgt[(eng->br_at - 1) & 63] == 0x576b ||
						eng->br_tgt[(eng->br_at - 1) & 63] == 0x1fa3))
				GD_DIAG( "diag: enter pc=%#x tgt=%#x op=%d sp=%#x bimm=%d a=%d\n",
					pc, eng->br_tgt[(eng->br_at - 1) & 63], rec->op, eng->sp,
					rec->bimm, rec->a);
			if (rec->op == 45 || rec->op == 66) {
				uint32_t ret = (pc + rec->len) & 0xffff;

				eng->sp -= 4;
				dmem_write(eng, eng->sp, 32, ret);
				if (eng->call_depth < 64)
					eng->call_pc[eng->call_depth] = pc;
				eng->call_depth++;
				if (eng->sh_at < 128) {
					eng->sh_ret[eng->sh_at] = ret;
					eng->sh_sp[eng->sh_at] = eng->sp;
					eng->sh_at++;
				}
			}
			pc = (rec->op == 45 ? (rec->bimm ? a : eng->r[a]) : target) & 0xffff;
			break;
		case 46: /* ret */
			{
				uint32_t rv = dmem_read(eng, eng->sp, 32) & 0xffff;
				if (eng->sh_at && eng->index == 0) {
					eng->sh_at--;
					if ((rv != eng->sh_ret[eng->sh_at] || eng->sp != eng->sh_sp[eng->sh_at]) &&
					    eng->sh_bad++ < 20)
						GD_DIAG( "diag: SHADOW ret pc=%#x rv=%#x exp=%#x sp=%#x expsp=%#x\n",
							pc, rv, eng->sh_ret[eng->sh_at], eng->sp, eng->sh_sp[eng->sh_at]);
				}
				eng->ret_pc[eng->ret_at & 63] = pc;
				eng->ret_val[eng->ret_at & 63] = rv;
				eng->ret_sp[eng->ret_at & 63] = eng->sp;
				eng->ret_at++;
				if (eng->index == 0 && pc == 0x1fdb) {
					uint32_t di;
					GD_DIAG( "diag: ret1fdb rv=%#x sp=%#x depth=%u calls:", rv, eng->sp, eng->call_depth);
					for (di = 0; di < eng->call_depth && di < 12; di++)
						GD_DIAG( " %#x", eng->call_pc[di]);
					GD_DIAG( "\n");
				}
				pc = rv;
			}
			eng->sp += 4;
			if (eng->call_depth) eng->call_depth--;
			break;
		case 47: case 48: /* push pop */
			if (rec->op == 47) {
				eng->sp -= 4;
				dmem_write(eng, eng->sp, 32, eng->r[a]);
			} else {
				eng->r[d] = dmem_read(eng, eng->sp, 32);
				eng->sp += 4;
			}
			break;
		case 49: case 50: case 51: case 52: case 53: {
			int i, count = a + 1;
			if (rec->op == 49) {
				for (i = 0; i < count; i++) {
					eng->sp -= 4;
					dmem_write(eng, eng->sp, 32, eng->r[i]);
				}
			} else {
				for (i = a; i >= 0; i--) {
					eng->r[i] = dmem_read(eng, eng->sp, 32);
					eng->sp += 4;
				}
				if (rec->op == 52 || rec->op == 53)
					eng->sp = (eng->sp + (uint32_t)b) & ~3u;
				if (rec->op == 51 || rec->op == 53) {
					uint32_t rv = dmem_read(eng, eng->sp, 32) & 0xffff;
					if (eng->sh_at && eng->index == 0) {
						eng->sh_at--;
						if ((rv != eng->sh_ret[eng->sh_at] || eng->sp != eng->sh_sp[eng->sh_at]) &&
						    eng->sh_bad++ < 20)
							GD_DIAG( "diag: SHADOW mpopret pc=%#x rv=%#x exp=%#x sp=%#x expsp=%#x\n",
								pc, rv, eng->sh_ret[eng->sh_at], eng->sp, eng->sh_sp[eng->sh_at]);
					}
					eng->ret_pc[eng->ret_at & 63] = pc;
					eng->ret_val[eng->ret_at & 63] = rv;
					eng->ret_sp[eng->ret_at & 63] = eng->sp;
					eng->ret_at++;
					if (eng->index == 0 && pc == 0x1fdb) {
						uint32_t di;
						GD_DIAG( "diag: ret1fdb mpop op=%d rv=%#x sp=%#x depth=%u calls:", rec->op, rv, eng->sp, eng->call_depth);
						for (di = 0; di < eng->call_depth && di < 12; di++)
							GD_DIAG( " %#x", eng->call_pc[di]);
						GD_DIAG( "\n");
					}
					pc = rv;
					eng->sp += 4;
					if (eng->call_depth) eng->call_depth--;
				}
			}
			break;
		}
		case 60: case 61: /* iord/iords */
			address = eng->r[rec->base] + (rec->idx >= 0 ? eng->r[rec->idx] * (uint32_t)rec->scale : (uint32_t)rec->off);
			result = local_io(dev, eng, address, 0, 0);
			set_r(eng, d, result, 32);
			break;
		case 62: /* iowr/iowrs */
			address = eng->r[rec->base] + (rec->idx >= 0 ? eng->r[rec->idx] * (uint32_t)rec->scale : (uint32_t)rec->off);
			if (eng->index == 0 && (address & 0xffffu) == 0x1000u) {
				uint32_t di, dn = eng->call_depth < 12 ? eng->call_depth : 12;
				GD_DIAG( "diag: status %#x pc=%#x stack:", eng->r[a], pc);
				for (di = 0; di < dn; di++)
					GD_DIAG( " %#x", eng->call_pc[di]);
				GD_DIAG( "\n");
			}
			local_io(dev, eng, address, eng->r[a], 1);
			break;
		case 63: /* exit */
			gd_log(dev, GD_LOG_DEBUG, "device: engine %d exit at %#x after %llu instructions\n",
			       eng->index, pc, (unsigned long long)eng->instructions);
			eng->pc = pc;
			eng->halted = true;
			eng->cpuctl = CPUCTL_HALTED;
			trace_add(dev, GD_TRACE_HALT, eng->index + 1, eng->base, pc, pc);
			engine_pc_history(dev, eng, 1, pc);
			__atomic_add_fetch(&eng->instructions, 1, __ATOMIC_RELAXED);
			return 0;
		case 64: /* trap */
			dev->fault_pc = pc;
			device_error(dev, "software trap", pc, a);
			eng->core_fault = true;
			engine_pc_history(dev, eng, 4, pc);
			return -EINVAL;
		case 67: /* sleep */
			if ((eng->flags >> a) & 1u) {
				eng->pc = pc;
				eng->halted = true;
				eng->cpuctl = CPUCTL_HALTED;
				trace_add(dev, GD_TRACE_HALT, eng->index + 1, eng->base, pc, pc);
				engine_pc_history(dev, eng, 2, pc);
				__atomic_add_fetch(&eng->instructions, 1, __ATOMIC_RELAXED);
				return 0;
			}
			break;
		case 70: case 71: case 72: case 73: case 74: case 75:
			crypto_apply(eng, rec->op, d, a, b);
			break;
		case 76: case 77: /* xdst / xdld */
			{
				uint32_t bv = eng->r[b];
				uint32_t av = eng->r[a];
				if (eng->xfer_trap) {
					uint32_t local = bv & 0xffffu;
					uint32_t size = 4u << ((bv >> 16) & 7);

					if (local + size > sizeof(eng->imem)) {
						device_error(dev, "SCP transfer overrun", local, size);
						eng->core_fault = true;
						return -EINVAL;
					}
					if (rec->op == 76) {
						uint32_t off;
						for (off = 0; off + 16 <= size && eng->crypto_stream_in_count < 64; off += 16) {
							memcpy(eng->crypto_stream_in[eng->crypto_stream_in_count],
							       eng->imem + local + off, 16);
							eng->crypto_stream_in_count++;
						}
						eng->crypto_stream_out_count = 0;
						while (eng->seq_pending && eng->crypto_stream_in_count) {
							crypto_exec_script(eng, 0, 1);
							eng->seq_pending--;
						}
					} else {
						uint32_t off;
						uint32_t at = 0;
						for (off = 0; off + 16 <= size && at < eng->crypto_stream_out_count; off += 16) {
							if (eng->index == 0 && ((local + off) & 0xffff) == 0xff50)
								GD_DIAG( "diag: trap-xdld-ff50 pc=%#x block=%08x\n", pc,
									*(uint32_t *)eng->crypto_stream_out[at]);
							memcpy(eng->dmem + ((local + off) & 0xffff),
							       eng->crypto_stream_out[at], 16);
							at++;
						}
						eng->crypto_stream_out_count = 0;
					}
					if (eng->xfer_count != UINT32_MAX && eng->xfer_count)
						eng->xfer_count--;
					break;
				}
				if (eng->xfer_count) {
					uint32_t creg = (bv >> 16) & 7;
					uint32_t local = bv & 0xffff;
					gd_log(dev, GD_LOG_DEBUG, "device: crypto %s pc=%#x b=%#x count=%u\n",
					       rec->op == 76 ? "xdst" : "xdld", pc, b, eng->xfer_count);
					if (rec->op == 76) {
						for (int i = 0; i < 16; i++)
							eng->c[creg][i] = eng->dmem[(local + i) & 0xffff];
						eng->c_hidden[creg] = false;
					} else {
						for (int i = 0; i < 16; i++)
							eng->dmem[(local + i) & 0xffff] = eng->c[creg][i];
						if (eng->index == 1 && local == 0x8df0) {
							memcpy(eng->fwsec_kdf_out, eng->c[creg], 16);
							memcpy(eng->fwsec_expect, eng->dmem + 0x540, 32);
							eng->fwsec_kdf_seen = true;
						}
						if (eng->index == 1 && local == 0x35f0) {
							memcpy(eng->fwsec_digest, eng->c[creg], 16);
							eng->fwsec_digest_seen = true;
						}
					}
					eng->xfer_count--;
				} else {
					uint64_t base = ((uint64_t)eng->sr[15] << 32) | eng->sr[7];
					uint64_t ext = (base << 8) + av;
					uint32_t port = (rec->op == 77) ? (eng->sr[11] >> 8) & 7 :
							(eng->sr[11] >> 12) & 7;
					uint32_t local = bv & 0xffff;
					uint32_t size = 4u << ((bv >> 16) & 7);
					uint32_t i;
					struct gd_dma_span *span = NULL;
					if (dev->config.verbose)
						gd_log(dev, GD_LOG_DEBUG,
						       "device: xfer %s pc=%#x port=%u ext=%#llx local=%#x size=%#x\n",
						       rec->op == 76 ? "xdst" : "xdld", pc, port,
						       (unsigned long long)ext, local, size);
					if (local + size > sizeof(eng->dmem)) {
						device_error(dev, "XDMA DMEM overrun", local, size);
						eng->core_fault = true;
						return -EINVAL;
					}
					uint8_t *fb = fb_window(dev, ext, size);
					if (fb) {
						if (eng->index == 0 && local <= 0xff50 && local + size > 0xff50)
							GD_DIAG( "diag: ext-xdld-fb-ff50 pc=%#x size=%#x\n", pc, size);
						if (rec->op == 77)
							memcpy(eng->dmem + local, fb, size);
						else
							memcpy(fb, eng->dmem + local, size);
					} else {
					pthread_mutex_lock(&dev->dma_lock);
					span = dma_find(dev, ext, size);
					if (span) {
						if (eng->index == 0 && local <= 0xff50 && local + size > 0xff50)
							GD_DIAG( "diag: ext-xdld-span-ff50 pc=%#x size=%#x\n", pc, size);
						if (rec->op == 77)
							memcpy(eng->dmem + local, span->cpu + (ext - span->address), size);
						else
							memcpy(span->cpu + (ext - span->address), eng->dmem + local, size);
					}
					pthread_mutex_unlock(&dev->dma_lock);
					if (!span) {
						for (i = 0; i < size; i += 4) {
							if (rec->op == 77) {
								uint32_t v = bar0_raw_read(dev, (uint32_t)ext + i);
								if (eng->index == 0 && local + i <= 0xff50 && local + i + 4 > 0xff50)
									GD_DIAG( "diag: ext-xdld-bar-ff50 pc=%#x v=%#x\n", pc, v);
								memcpy(eng->dmem + local + i, &v, 4);
							} else {
								uint32_t v;
								memcpy(&v, eng->dmem + local + i, 4);
								bar0_raw_write(dev, (uint32_t)ext + i, v);
							}
						}
					}
					}
				}
			}
			break;
		case 78: case 79: case 80:
			break; /* xdwait/xdfence/xcwait */
		case 81: case 82: case 86: case 87: case 88:
			crypto_apply(eng, rec->op, d, a, b);
			break;
		case 83: /* vtlb */
			eng->r[d] = tlb_vtlb(eng, eng->r[a] & 0xffffffu);
			break;
		case 84: /* itlb */
			tlb_itlb(eng, eng->r[a] & 0xffffffu);
			break;
		case 85: /* ptlb */
			eng->r[d] = tlb_ptlb(eng, eng->r[a] & 0xffffffu);
			break;
		case 94: { /* iret */
			eng->pc = pc = dmem_read(eng, eng->sp, 32) & 0xffffu;
			eng->sp += 4;
			uint32_t is = (eng->flags >> 20) & 7u;
			eng->flags = (eng->flags & ~(7u << 16)) | (is << 16);
			break;
		}
		case 95: { /* xcld: code-load XDMA */
			uint32_t av = eng->r[a], bv = eng->r[b];
			uint64_t base = ((uint64_t)eng->sr[14] << 32) | eng->sr[6];
			uint64_t ext = (base << 8) + av;
			uint32_t local = bv & 0xffffu;
			if (local + 0x100u > sizeof(eng->imem)) {
				device_error(dev, "XCLD IMEM overrun", local, 0x100);
				eng->core_fault = true;
				return -EINVAL;
			}
			for (uint32_t i = 0; i < 0x100; i += 4) {
				uint8_t *fbw = fb_window(dev, ext + i, 4);
				if (fbw) memcpy(eng->imem + local + i, fbw, 4);
				else {
					struct gd_dma_span *sp = dma_find(dev, ext + i, 4);
					if (sp) memcpy(eng->imem + local + i, sp->cpu + (ext + i - sp->address), 4);
					else {
						uint32_t v = bar0_raw_read(dev, (uint32_t)(ext + i));
						memcpy(eng->imem + local + i, &v, 4);
					}
				}
			}
			tlb_set(eng, local >> 8, local >> 8, (eng->sr[10] >> 16 & 1) ? 4 : 1);
			break;
		}
		case 96: case 97: case 98: case 99: case 100: case 101:
		case 102: case 103: case 104: case 105: case 106: case 107:
		case 120: case 121: case 122: case 123: case 124: case 125:
		case 126: case 127: case 128: case 129: case 130: case 131:
		case 132: case 133: case 134: case 135: case 136: case 137:
		case 138: case 139: case 140: case 141: case 142:
			crypto_apply(eng, rec->op, d, a, b);
			break;
		case 89: case 90: case 91: {
			uint32_t bit = 1u << ((rec->bimm ? b : eng->r[b]) & 31);
			if (rec->op == 89) eng->flags |= bit;
			else if (rec->op == 90) eng->flags &= ~bit;
			else eng->flags ^= bit;
			break;
		}
		case 93: /* cxset: bit7 trap, bit5 shortcut, low 5 bits count */
			{
				uint32_t count = a & 0x1f;

				eng->xfer_trap = !!(a & 0x20);      /* 0xa1 and 0x22 */
				eng->xfer_shortcut = !!(a & 0x80);  /* 0xa1 */
				if (count == 0x1f)
					eng->xfer_count = UINT32_MAX;
				else if (!count)
					eng->xfer_count = 0;
				else
					eng->xfer_count = count; /* hardware counts transfers */
			}
			break;
		case 92: {
			uint32_t bit = (rec->bimm ? b : eng->r[b]) & 31;
			eng->flags = (eng->flags & ~(1u << bit)) | ((eng->r[a] & 1) << bit);
			break;
		}
		default:
			dev->fault_pc = pc;
			device_error(dev, "unimplemented opcode", pc, rec->op);
			eng->core_fault = true;
			return -EINVAL;
		}
		if (rec->op != 38 && rec->op != 39 && rec->op != 40 &&
		    rec->op != 45 && rec->op != 46 && rec->op != 51 && rec->op != 53 &&
		    rec->op != 65 && rec->op != 66)
			pc = (pc + rec->len) & 0xffff;
		eng->pc = pc;
		__atomic_add_fetch(&eng->instructions, 1, __ATOMIC_RELAXED);
	}
	dev->fault_pc = pc;
	device_error(dev, "Falcon instruction budget exhausted", pc, budget);
	eng->core_fault = true;
	engine_pc_history(dev, eng, 5, pc);
	return -ETIMEDOUT;
}

static void load_code_table(struct gd_engine *eng, const char *path)
{
	FILE *file;
	uint32_t header[3];
	size_t i;
	struct gd_device *dev = eng->dev;
	if (!path)
		return;
	file = fopen(path, "rb");
	if (!file) {
		device_error(dev, "cannot open decoded table", 0, 0);
		return;
	}
	if (fread(header, sizeof(header), 1, file) != 1 || header[0] != 0x31434447u) {
		fclose(file);
		device_error(dev, "bad decoded table", 0, 0);
		return;
	}
	eng->code_count = header[1];
	eng->code = calloc(eng->code_count, sizeof(*eng->code));
	eng->code_index = malloc(0x10000 * sizeof(*eng->code_index));
	if (!eng->code || !eng->code_index) {
		fclose(file);
		return;
	}
	for (i = 0; i < 0x10000; i++)
		eng->code_index[i] = -1;
	for (i = 0; i < eng->code_count; i++) {
		if (fread(&eng->code[i], sizeof(eng->code[i]), 1, file) != 1) {
			eng->code_count = i;
			break;
		}
		if (eng->code[i].pc < 0x10000)
			eng->code_index[eng->code[i].pc] = (int32_t)i;
	}
	fclose(file);
}

static bool decrypt_pio_code(struct gd_device *dev, struct gd_engine *eng)
{
	uint32_t lo = eng->pio_secure_lo, hi = eng->pio_secure_hi;

	if (hi <= lo)
		return true;
	if (!dev->config.key_set || (lo & 15) || ((hi - lo) & 15)) {
		device_error(dev, "secure PIO code range is not AES-block aligned", lo, hi - lo);
		return false;
	}
	if (gd_aes128_decrypt(dev->config.key, eng->imem + lo, hi - lo) != 0) {
		device_error(dev, "secure PIO code decrypt failed", lo, hi - lo);
		return false;
	}
	eng->pio_secure_lo = eng->pio_secure_hi = 0;
	return true;
}

static void engine_kick(struct gd_device *dev, struct gd_engine *eng)
{
	{
		if (eng->index == 0) {
			struct gd_dma_span *sp = dma_find_size(dev, 0x100);
			if (sp) {
				uint32_t i;
				GD_DIAG( "diag: wprmeta:");
				for (i = 0; i < 200; i += 8)
					GD_DIAG( " %016llx", (unsigned long long)*(uint64_t *)(sp->cpu+i));
				GD_DIAG( "\n");
			}
		}
		if (eng->index == 0 && (dev->bar[0x11821c / 4] & (1u << 17))) {
			uint32_t lo = dev->bar[0x1fa824 / 4], hi = dev->bar[0x1fa828 / 4];
			uint32_t want;

			/* The debug FWSEC image publishes an inclusive 4 KiB FRTS
			 * end; the 610 Booter decodes WPR2_HI with the 128 KiB
			 * alignment term from booter_wpr2_tu10x.c.  Convert the
			 * encoding and record that it happened. */
			if (!gd_wpr2_fwsec_hi(lo, &want) && want != hi) {
				dev->bar[0x1fa828 / 4] = want;
				dev->wpr2_normalized = true;
			}
		}
		if (eng->index == 0) {
			unsigned o;
			GD_DIAG( "diag: scratch:");
			for (o = 0x1400; o <= 0x1440; o += 4)
				GD_DIAG( " %#x=%#x", o, dev->bar[o / 4]);
			GD_DIAG( "\n");
		}

		if (eng->index == 1)
			dev->fwsec_launch_reached = true;
		else
			dev->booter_launch_reached = true;
		if (!dev->config.key_set) {
			device_error(dev, "debug firmware key missing; execution refused", 0, 0);
			eng->core_fault = true;
			eng->started = true;
			eng->halted = true;
			eng->cpuctl = CPUCTL_HALTED;
			return;
		}
		/* The real FWSEC needs more than the old 1M-instruction smoke
		 * budget after the SE PKA path; keep the run bounded. */
		if (!decrypt_pio_code(dev, eng) ||
		    engine_execute(dev, eng, 100000000) != 0) {
			eng->core_fault = true;
			eng->halted = true;
			eng->cpuctl = CPUCTL_HALTED;
			return;
		}
	}
	eng->started = true;
	eng->halted = true;
	eng->cpuctl = CPUCTL_HALTED;
	__atomic_add_fetch(&dev->launches[eng->index], 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&eng->instructions, 1, __ATOMIC_RELAXED);
	trace_add(dev, GD_TRACE_HALT, eng->index + 1, eng->base, eng->reset_plm, 0);
}

/* Resolve a DMA address against framebuffer or coherent sysmem.  The same
 * address can only belong to one; a miss is a modelling gap, never a no-op. */
static uint8_t *dma_target(struct gd_device *dev, uint64_t address, size_t size,
			   struct gd_dma_span **span_out)
{
	struct gd_dma_span *span;
	uint8_t *fb = fb_window(dev, address, size);

	if (fb) {
		*span_out = NULL;
		return fb;
	}
	span = dma_find(dev, address, size);
	*span_out = span;
	if (!span)
		return NULL;
	if ((uint64_t)(address - span->address) + size > span->size)
		return NULL;
	return span->cpu + (address - span->address);
}

/* Falcon DMATRFCMD: FINAL/IDLE status, SEC (2), IMEM (4), WRITE (5),
 * SIZE (10:8, byte count 4<<n), CTXDMA (14:12). */
static void dma_execute(struct gd_device *dev, struct gd_engine *eng, uint32_t cmd)
{
	uint64_t address;
	size_t size;
	bool imem = !!(cmd & (1u << 4));
	bool store = !!(cmd & (1u << 5));
	uint32_t base = eng->dma_base, base1 = eng->dma_base1;
	uint32_t moffs = eng->dma_moffs, fboffs = eng->dma_fboffs;
	struct gd_dma_span *span = NULL;
	uint8_t decoded[512];
	uint8_t *target;
	size_t limit = imem ? sizeof(eng->imem) : sizeof(eng->dmem);

	size = 4u << ((cmd >> 8) & 7);
	if (size > sizeof(decoded)) {
		device_error(dev, "DMA size", moffs, cmd);
		return;
	}
	address = ((uint64_t)(base1 & 0x1ffu) << 32 | base) << 8;
	address += fboffs;
	if (size == 0x100 && moffs >= 0x4e00)
		GD_DIAG( "diag: dmabounce eng=%d src=%#llx local=%#x cmd=%#x wr=%d base=%#x base1=%#x fboffs=%#x pc=%#x\n",
			eng->index, (unsigned long long)address, moffs, cmd, store, base, base1, fboffs, eng->pc);
	if (size == 0x100 && moffs == 0x400)
		GD_DIAG( "diag: dma100 src=%#llx store=%d eng=%d pc=%#x\n",
			(unsigned long long)address, store, eng->index, eng->pc);
	if ((size_t)moffs + size > limit) {
		device_error(dev, imem ? "DMA IMEM overrun" : "DMA DMEM overrun", moffs, cmd);
		return;
	}
	pthread_mutex_lock(&dev->dma_lock);
	target = dma_target(dev, address, size, &span);
	if (!target) {
		pthread_mutex_unlock(&dev->dma_lock);
		device_error(dev, store ? "DMA destination unmapped" : "DMA source unmapped",
			     (uint32_t)address, cmd);
		return;
	}
	if (store) {
		memcpy(target, (imem ? eng->imem : eng->dmem) + moffs, size);
	} else {
		memcpy(decoded, target, size);
		if (imem && (cmd & (1u << 2))) {
			if (!dev->config.key_set ||
			    gd_aes128_decrypt(dev->config.key, decoded, size) != 0) {
				pthread_mutex_unlock(&dev->dma_lock);
				device_error(dev, "secure code decrypt failed", moffs, cmd);
				return;
			}
		}
		if (!imem && moffs <= 0xffe0 && moffs + size > 0xff40)
			GD_DIAG( "diag: dma-stack moffs=%#x size=%zu src=%#llx cmd=%#x pc=%#x\n",
				moffs, size, (unsigned long long)address, cmd, eng->pc);
		memcpy((imem ? eng->imem : eng->dmem) + moffs, decoded, size);
		if (imem) {
			eng->imem_tag = fboffs >> 8;
			tlb_set(eng, moffs >> 8, fboffs >> 8,
				(cmd & (1u << 2)) ? 4 : 1);
		}
	}
	pthread_mutex_unlock(&dev->dma_lock);
	dev->engine_writes++;
	trace_add(dev, GD_TRACE_DMA, eng->index + 1, (uint32_t)address, cmd, 0);
	trace_add(dev, GD_TRACE_HALT, eng->index + 1, eng->base, eng->reset_plm, 0);
}

static void *engine_thread(void *arg)
{
	struct gd_engine *eng = arg;
	struct gd_device *dev = eng->dev;

	pthread_mutex_lock(&eng->lock);
	while (!eng->stop) {
		int action = 0;
		uint32_t cmd = 0;

		if (eng->qhead != eng->qtail) {
			cmd = eng->dma_q[eng->qhead++];
			eng->qhead %= 64;
			action = 1;
		} else if (eng->started && !eng->halted && !eng->faulted) {
			action = 2;
		}
		if (!action) {
			pthread_cond_wait(&eng->wake, &eng->lock);
			continue;
		}
		eng->busy = true;
		pthread_mutex_unlock(&eng->lock);
		if (action == 1)
			dma_execute(dev, eng, cmd);
		else
			engine_kick(dev, eng);
		pthread_mutex_lock(&eng->lock);
		if (action == 1)
			eng->dma_stat = 2;
		eng->busy = false;
		pthread_cond_broadcast(&eng->idle);
	}
	pthread_mutex_unlock(&eng->lock);
	return NULL;
}

static void engine_start_locked(struct gd_device *dev, struct gd_engine *eng)
{
	eng->started = true;
	eng->halted = false;
	if (eng->thread_up) {
		eng->pending_kick = false;
		pthread_cond_signal(&eng->wake);
		return;
	}
	eng->pending_kick = true;
}

static void engine_reset_locked(struct gd_device *dev, struct gd_engine *eng, uint32_t value)
{
	while (eng->busy)
		pthread_cond_wait(&eng->idle, &eng->lock);
	eng->pending_kick = false;
	eng->engine_reg = value;
	if (value & 1) {
		engine_reset_state(dev, eng);
		eng->engine_reg = value | 1;
		eng->scrub_pending = true;
		eng->scrub_done_us = dev->time_us + 2000;
		eng->hwcfg2 |= HWCFG2_SCRUB;
		gd_log(dev, GD_LOG_DEBUG, "device: engine %d reset asserted\n", eng->index);
	} else {
		eng->engine_reg = value & ~1u;
		settle_scrub(dev, eng);
		eng->hwcfg2 &= ~HWCFG2_SCRUB;
		gd_log(dev, GD_LOG_DEBUG, "device: engine %d reset released\n", eng->index);
	}
}

static uint32_t engine_read_locked(struct gd_device *dev, struct gd_engine *eng, uint32_t off, int priv)
{
	settle_scrub(dev, eng);
	if (!engine_plm_allowed(eng, off, priv, false))
		return 0xbadf0000u;
	switch (off) {
	case OFF_MAILBOX0: return eng->mailbox[0];
	case OFF_MAILBOX1: return eng->mailbox[1];
	case OFF_HWCFG2: return eng->hwcfg2 | (eng->scrub_pending ? HWCFG2_SCRUB : 0);
	case OFF_CPUCTL:
	case OFF_CPUCTL_ALT:
		return eng->halted ? CPUCTL_HALTED : 0;
	case OFF_BOOTVEC: return eng->bootvec;
	case OFF_HWCFG: return eng->hwcfg;
	case OFF_DMACTL: return eng->dmactl;
	case OFF_DMABASE: return eng->dma_base;
	case OFF_DMABASE1: return eng->dma_base1;
	case OFF_DMAMOFFS: return eng->dma_moffs;
	case OFF_DMAFBOFFS: return eng->dma_fboffs;
	case OFF_DMACMD: return eng->dma_stat;
	case OFF_ENGINE: return eng->engine_reg;
	case OFF_RESET_PLM: return eng->reset_plm;
	case OFF_IMEMC: return eng->imemc;
	case 0x1100: return 0x40000000u; /* FWSEC boot-progress gate */
	case 0x9000: return 0x2u; /* crypto engine available */
	case 0x1000: return eng->io[0x1000 / 4]; /* NV_CSEC_FALCON_MAILBOX0 */
	case 0x4f00: return gd_falcon_engid();
	case 0x1070: return gd_pbus_emulation_rev(0);
	case 0x1074: return gd_pbus_emulation_rev(1);
	case 0x10200: return dev->config.key_set ? 0x100000u : 0; /* SCP_CTL_STAT.DEBUG_MODE */
	case 0x9100: return gd_csberrstat(eng->csb_enable, eng->csb_valid, eng->csb_pc);
	case 0x1c000: case 0x1c100: case 0x1c200:
		return gd_csec_read(&eng->csec, off);
	case 0x1c300: return 0x0063bb0bu;
	case 0x40300: return eng->sha_in_addr;
	case 0x40400: return eng->sha_in_hi;
	case 0x40700: return eng->sha_msg_len;
	case 0x41f00: return eng->sha_op;
	case 0x42000: return eng->sha_status;
	case 0x44200: return eng->sha_mutex;
	case 0x47200: return 2; /* SHA hash selection/version: SHA-256 */
	case 0xd000: case 0xd100: case 0xd200: case 0xd300:
	case 0xd400: case 0xd500: case 0xd600: case 0xd700:
		return gd_doc_read(&eng->doc, off, priv);
	case 0x344: return eng->io[0x344 / 4];
	case 0x348: return eng->io[0x348 / 4];
	case 0x350: return eng->response_ready;
	case 0x354: return eng->response;
	case 0x700: return eng->io[0x700 / 4];
	case 0x704: return eng->io[0x704 / 4];
	case 0x708: return eng->io[0x708 / 4];
	case OFF_DMEMC: return eng->dmemc;
	case 0x404: case 0x10100: return eng->io[0x404 / 4];
	case 0x008: return 0;
	case 0x00c: return 0;
	case 0x018: return 0;
	case 0x01c: return 0;
	case 0x04c: return eng->started && !eng->halted;
	default:
		if (off >= 0x40f00 && off < 0x40f00 + 8 * 0x100 &&
		    ((off - 0x40f00) & 0xff) == 0)
			return eng->sha_result[(off - 0x40f00) / 0x100];
		if (off < GD_IO_SIZE) {
			if (eng->io_valid[off / 4])
				return eng->io[off / 4];
			gd_log(dev, GD_LOG_DEBUG, "device: unmodelled engine read %#x -> 0\n", eng->base + off);
			trace_add(dev, GD_TRACE_BAR_READ, eng->index + 1, eng->base + off, 0, 0);
			return 0;
		}
		record_unknown(dev, false, eng->base + off, 0, 0);
		return 0xffffffffu;
	}
}

static bool engine_write_locked(struct gd_device *dev, struct gd_engine *eng, uint32_t off, uint32_t value, int priv)
{
	settle_scrub(dev, eng);
	if (!engine_plm_allowed(eng, off, priv, true)) {
		dev->denied_writes++;
		dev->plm_violations++;
		gd_log(dev, GD_LOG_DEBUG, "device: PLM denied engine write base=%#x off=%#x priv=%d\n",
		       eng->base, off, priv);
		return false;
	}
	switch (off) {
	case OFF_MAILBOX0: eng->mailbox[0] = value; break;
	case OFF_MAILBOX1: eng->mailbox[1] = value; break;
	case OFF_HWCFG2: eng->hwcfg2 = value; break;
	case OFF_CPUCTL:
	case OFF_CPUCTL_ALT:
		eng->cpuctl = value;
		if (value & CPUCTL_START)
			engine_start_locked(dev, eng);
		break;
	case OFF_BOOTVEC: eng->bootvec = value; break;
	case OFF_DMACTL: eng->dmactl = value; break;
	case OFF_DMABASE:
		eng->dma_base = value;
		GD_DIAG( "diag: dma_base=%#x pc=%#x eng=%d\n", value, eng->pc, eng->index);
		break;
	case OFF_DMABASE1:
		eng->dma_base1 = value;
		GD_DIAG( "diag: dma_base1=%#x pc=%#x eng=%d\n", value, eng->pc, eng->index);
		break;
	case OFF_DMAMOFFS: eng->dma_moffs = value; break;
	case OFF_DMAFBOFFS: eng->dma_fboffs = value; break;
	case OFF_DMACMD:
		eng->dma_cmd = value;
		GD_DIAG( "diag: dmacmd=%#x pc=%#x eng=%d base=%#x fboffs=%#x\n",
			value, eng->pc, eng->index, eng->dma_base, eng->dma_fboffs);
		if (eng->thread_up) {
			eng->dma_stat = 1;
			eng->dma_q[eng->qtail++] = value;
			eng->qtail %= 64;
			pthread_cond_signal(&eng->wake);
		} else {
			pthread_mutex_unlock(&eng->lock);
			dma_execute(dev, eng, value);
			pthread_mutex_lock(&eng->lock);
			eng->dma_stat = 2;
		}
		break;
	case OFF_ENGINE: engine_reset_locked(dev, eng, value); break;
	case OFF_RESET_PLM: eng->reset_plm = value; break;
	case OFF_IMEMC:
		eng->imemc = value;
		eng->pio_imem = value & 0xffffu;
		break;
	case OFF_IMEMD:
		if (eng->pio_imem + 4 <= sizeof(eng->imem))
			memcpy(eng->imem + eng->pio_imem, &value, 4);
		else
			device_error(dev, "IMEM PIO overrun", eng->pio_imem, value);
		if (eng->imemc & (1u << 28)) {
			uint32_t at = eng->pio_imem;
			if (!eng->pio_secure_hi) {
				eng->pio_secure_lo = at;
				eng->pio_secure_hi = at + 4;
			} else {
				if (at < eng->pio_secure_lo)
					eng->pio_secure_lo = at;
				if (at + 4 > eng->pio_secure_hi)
					eng->pio_secure_hi = at + 4;
			}
		}
		if (eng->imemc & (1u << 24))
			eng->pio_imem += 4;
		break;
	case OFF_IMEMT: eng->imem_tag = value; break;
	case 0x1000: eng->io[0x1000 / 4] = value; break;
	case 0x9100:
		if (value & 0x40000000u)
			eng->csb_enable = true;
		if (value & 0x80000000u)
			eng->csb_valid = true;
		eng->csb_pc = value & 0xffffffu;
		break;
	case 0x344: {
		uint32_t address = value & 0xffffu;
		eng->io[0x344 / 4] = value;
		if (value & 0x10000u) {
			eng->response = (address >= 0x8e00 && address < 0x8f00) ?
				csb_read(dev, address, priv) : bar0_raw_read(dev, address);
			eng->response_ready = 1;
		} else {
			uint32_t data = eng->io[0x348 / 4];
			if (address >= 0x8e00 && address < 0x8f00)
				csb_write(dev, address, data, priv);
			else
				bar0_raw_write(dev, address, data);
		}
		break;
	}
	case 0x348: eng->io[0x348 / 4] = value; break;
	case 0x350:
		eng->io[0x350 / 4] = value;
		if (value & 0x100000u)
			eng->response_ready = 0;
		break;
	case 0x700: {
		eng->io[0x700 / 4] = value;
		if (value & 0x80000000u) {
			uint32_t address = eng->io[0x704 / 4];
			if ((value & 3u) == 1u) {
				eng->io[0x708 / 4] = bar0_raw_read(dev, address);
				eng->io[0x700 / 4] = 0x400u;
			} else {
				bar0_raw_write(dev, address, eng->io[0x708 / 4]);
				eng->io[0x700 / 4] = 0x400u;
			}
		}
		break;
	}
	case 0x704: eng->io[0x704 / 4] = value; break;
	case 0x708: eng->io[0x708 / 4] = value; break;
	case 0x1c000: case 0x1c100: case 0x1c200:
		gd_csec_write(&eng->csec, off, value);
		break;
	case 0x41f00:
		eng->sha_op = value;
		if (dev->config.verbose) gd_log(dev, GD_LOG_DEBUG, "device: sha-op pc=%#x value=%#x\n", eng->pc, value);
		sha_run(eng);
		break;
	case 0x42000: eng->sha_status = value; break;
	case 0x44000: eng->sha_mutex = value; break;
	case 0x40300: eng->sha_in_addr = value; break;
	case 0x40400: eng->sha_in_hi = value; break;
	case 0x40700:
		eng->sha_msg_len = value;
		eng->sha_total_bits = value;
		if (dev->config.verbose) gd_log(dev, GD_LOG_DEBUG, "device: sha-total pc=%#x value=%#x\n", eng->pc, value);
		break;
	case 0x40b00:
		eng->sha_update_bits = value;
		if (dev->config.verbose) gd_log(dev, GD_LOG_DEBUG, "device: sha-update pc=%#x value=%#x\n", eng->pc, value);
		sha_feed(eng);
		break;
	case 0xd000: case 0xd100: case 0xd200: case 0xd300:
	case 0xd400: case 0xd500: case 0xd600: case 0xd700:
		gd_doc_write(&eng->doc, off, value, priv);
		break;
	case 0x404: case 0x10100:
		eng->io[0x404 / 4] = value & ~0x100u;
		break;
	case OFF_DMEMC:
		eng->dmemc = value;
		eng->pio_dmem = value & 0xffffu;
		break;
	case OFF_DMEMD:
		if (eng->pio_dmem + 4 <= sizeof(eng->dmem))
			memcpy(eng->dmem + eng->pio_dmem, &value, 4);
		else
			device_error(dev, "DMEM PIO overrun", eng->pio_dmem, value);
		if (eng->dmemc & (1u << 24))
			eng->pio_dmem += 4;
		break;
	default:
		if (off < GD_IO_SIZE) {
			eng->io[off / 4] = value;
			eng->io_valid[off / 4] = 1;
		} else {
			record_unknown(dev, true, eng->base + off, value, 0);
		}
		break;
	}
	dev->engine_writes++;
	return true;
}

uint32_t gd_bar0_read(struct gd_device *dev, uint32_t address)
{
	struct gd_engine *eng;
	uint32_t off, value;

	dev->host_reads++;
	dev->time_us = now_us();
	trace_add(dev, GD_TRACE_BAR_READ, 0, address, 0, 0);
	if (address >= PROM_OFFSET && address < PROM_OFFSET + PROM_SIZE)
		return 0xffffffffu;
	eng = engine_for(dev, address, &off);
	if (eng) {
		pthread_mutex_lock(&eng->lock);
		value = engine_read_locked(dev, eng, off, 0);
		pthread_mutex_unlock(&eng->lock);
		return value;
	}
	if (!plm_allowed(dev, address, 0, false))
		return 0;
	pthread_mutex_lock(&dev->bar_lock);
	bool valid = address + 4 <= GD_BAR_SIZE && dev->bar_valid[address / 4];
	value = valid ? dev->bar[address / 4] : 0xffffffffu;
	pthread_mutex_unlock(&dev->bar_lock);
	if (!valid)
		record_unknown(dev, false, address, 0, 0);
	return value;
}

void gd_bar0_write(struct gd_device *dev, uint32_t address, uint32_t value)
{
	struct gd_engine *eng;
	uint32_t off;

	dev->host_writes++;
	dev->time_us = now_us();
	trace_add(dev, GD_TRACE_BAR_WRITE, 0, address, value, 0);
	if (address >= PROM_OFFSET && address < PROM_OFFSET + PROM_SIZE) {
		record_unknown(dev, true, address, value, 0);
		return;
	}
	eng = engine_for(dev, address, &off);
	if (eng) {
		pthread_mutex_lock(&eng->lock);
		bool allowed = engine_write_locked(dev, eng, off, value, 0);
		bool kick = eng->pending_kick;
		eng->pending_kick = false;
		pthread_mutex_unlock(&eng->lock);
		if (!allowed) {
			dev->denied_writes++;
			gd_log(dev, GD_LOG_DEBUG, "device: PLM denied host engine write %#x=%#x\n",
			       address, value);
			return;
		}
		if (kick) {
			gd_log(dev, GD_LOG_DEBUG, "device: host kick engine %d\n", eng->index);
			engine_kick(dev, eng);
		}
		return;
	}
	if (!plm_allowed(dev, address, 0, true)) {
		uint32_t ma = plm_mask_register(address) ? address : plm_mask_for(address);
		dev->denied_writes++;
		if (ma && ((dev->bar[ma / 4] >> 9) & 1))
			dev->plm_violations++;
		gd_log(dev, GD_LOG_DEBUG, "device: PLM denied host write %#x=%#x\n", address, value);
		return;
	}
	value &= bar_write_mask(address);
	if (address == 0x82381c || address == 0x823820)
		dev->plm_opened_seen = true;
	pthread_mutex_lock(&dev->bar_lock);
	if (address + 4 <= GD_BAR_SIZE) {
		dev->bar[address / 4] = value;
		dev->bar_valid[address / 4] = 1;
		if (address == 0x1180f8 && (value & (1u << 24)))
			csb_release_all(dev);
		plm_latch_write(dev, address, value);
		if (address == 0x823804 && dev->feat_count < 8)
			dev->feats[dev->feat_count++] = value;
	} else {
		record_unknown(dev, true, address, value, 0);
	}
	pthread_mutex_unlock(&dev->bar_lock);
}

int gd_prom_copy(struct gd_device *dev, uint32_t offset, void *dest, size_t size)
{
	if (offset < PROM_OFFSET || offset >= PROM_OFFSET + PROM_SIZE)
		return -EINVAL;
	offset -= PROM_OFFSET;
	if (offset + size > PROM_SIZE)
		size = PROM_SIZE - offset;
	memcpy(dest, dev->prom + offset, size);
	return (int)size;
}

int gd_pci_read(struct gd_device *dev, int space, int offset, int width, uint32_t *value)
{
	uint32_t v = 0xffffffffu;
	(void)width;

	if (space == 0) {
		switch (offset) {
		case 0x00: v = 0x10deu | (uint32_t)dev->board->device << 16; break;
		case 0x04: v = dev->pci_command; break;
		case 0x08: v = 0x030000a1u; break;
		case 0x10: case 0x14: case 0x18: case 0x1c: case 0x20: case 0x24:
			v = 0xf0000000u + (uint32_t)(offset - 0x10) * 4; break;
		default:
			if (offset >= 0x40 && offset < 0x60)
				v = 0;
			else
				v = 0;
			break;
		}
	} else if (space == 1) {
		switch (offset) {
		case 4: v = 1u << 28; break;
		case 10: v = 0; break;
		default: v = 0; break;
		}
	} else {
		return -EINVAL;
	}
	*value = v;
	return 0;
}

int gd_pci_write(struct gd_device *dev, int space, int offset, int width, uint32_t value)
{
	(void)width;
	if (space != 0)
		return -EINVAL;
	if (offset == 0x04) {
		dev->pci_command = value;
		dev->bar[0x200 / 4] = value;
		dev->bar_valid[0x200 / 4] = 1;
	}
	return 0;
}

int gd_dma_map(struct gd_device *dev, uint64_t address, void *cpu, size_t size)
{
	uint32_t i;
	if (dev->dma_count >= GD_MAX_DMA)
		return -ENOMEM;
	for (i = 0; i < dev->dma_count; i++) {
		struct gd_dma_span *s = &dev->dma[i];
		if (s->live && address < s->address + s->size && s->address < address + size)
			return -EINVAL;
	}
	struct gd_dma_span *s = &dev->dma[dev->dma_count++];
	s->address = address;
	s->cpu = cpu;
	s->size = size;
	s->live = true;
	return 0;
}

void gd_dma_unmap(struct gd_device *dev, uint64_t address)
{
	uint32_t i;
	for (i = 0; i < dev->dma_count; i++)
		if (dev->dma[i].live && dev->dma[i].address == address) {
			dev->dma[i].live = false;
			return;
		}
}

void gd_dma_barrier(struct gd_device *dev)
{
	__sync_synchronize();
	(void)dev;
}

static void load_bar_defaults(struct gd_device *dev)
{
	size_t i;
	uint32_t chip = dev->board->chip;

	for (i = 0; i < sizeof(gd_bar0_defaults) / sizeof(gd_bar0_defaults[0]); i++) {
		uint32_t addr = gd_bar0_defaults[i].addr;
		dev->bar[addr / 4] = gd_bar0_defaults[i].reset;
		dev->bar_valid[addr / 4] = 1;
	}
	/* The register table is a GA102 fixture; override identity per board. */
	set_bar_locked(dev, 0x0, (chip << 20) | 0xa1u);
	dev->wpr2_lo = 0x1ffffe00u;
	dev->wpr2_hi = 0;
	set_bar_locked(dev, 0x1fa824, dev->wpr2_lo);
	set_bar_locked(dev, 0x1fa828, dev->wpr2_hi);
	set_bar_locked(dev, 0x110100, CPUCTL_HALTED);
	set_bar_locked(dev, 0x1100f4, HWCFG2_RESET);
	set_bar_locked(dev, 0x118234, 0xff);
	set_bar_locked(dev, 0x1183a4, 0x6000);
	set_bar_locked(dev, 0x2240c, 0x2);
	set_bar_locked(dev, 0xa00, chip << 20);
		set_bar_locked(dev, 0x1180fc, 1u << 20);
	set_bar_locked(dev, 0x1180f8, 0u); /* BSI scratch 14: no level/error */
	set_bar_locked(dev, 0x118220, 0u);
	set_bar_locked(dev, 0x118244, 0u); /* declared pre-FWSEC boundary: FRTS not cached */
	set_bar_locked(dev, 0x118248, 0u);
	set_bar_locked(dev, 0x118c6c, 0u);

	/* GC6 AON secure-scratch bootstrap latches (groups 40/41).  The real
	 * early boot/SCP flow writes these before FWSEC; we do not emulate that
	 * flow, so seed the post-bootstrap values here.  This is a test fixture,
	 * not evidence of the hardware sequence.
	 */
	set_bar_locked(dev, 0x11839c, 0xff);
	set_bar_locked(dev, 0x1183a0, 0xff);
	dev->ss0 = dev->bar[0x82381c / 4];
	dev->ss1 = dev->bar[0x823820 / 4];
}

static uint32_t gd_doc_io_read(void *ctx, uint32_t address, int priv)
{
	struct gd_engine *eng = ctx;
	return dio_read(eng->dev, eng, address, priv);
}

static void gd_doc_io_write(void *ctx, uint32_t address, uint32_t value, int priv)
{
	struct gd_engine *eng = ctx;
	dio_write(eng->dev, eng, address, value, priv);
}

static bool csec_bar_read(void *ctx, uint32_t address, uint32_t *value)
{
	struct gd_device *dev = ctx;
	if (address + 4 > GD_BAR_SIZE || !plm_allowed(dev, address, 3, false))
		return false;
	*value = bar0_raw_read(dev, address);
	if (address == 0x118244 || address == 0x118248)
		GD_DIAG( "diag: group6 read [%#x] -> %#x booterpc=%#x\n",
			address, *value, dev->engine[0].pc);
	return true;
}

static bool csec_bar_write(void *ctx, uint32_t address, uint32_t value)
{
	struct gd_device *dev = ctx;
	if (address + 4 > GD_BAR_SIZE || !plm_allowed(dev, address, 3, true)) {
		dev->denied_writes++;
		dev->plm_violations++;
		return false;
	}
	if (address == 0x118244 || address == 0x118248)
		GD_DIAG( "diag: group6 write [%#x]=%#x fwsec=%d pc=%#x\n",
			address, value, !!dev->engine[1].pc, dev->engine[1].pc);
	if (address == 0x1fa824 || address == 0x1fa828) {
		uint32_t di;
		GD_DIAG( "diag: wpr2 write [%#x]=%#x pc=%#x r0=%#x r1=%#x r10=%#x r11=%#x stack:",
			address, value, dev->engine[1].pc, dev->engine[1].r[0],
			dev->engine[1].r[1], dev->engine[1].r[10], dev->engine[1].r[11]);
		for (di = 0; di < dev->engine[1].call_depth && di < 12; di++)
			GD_DIAG( " %#x", dev->engine[1].call_pc[di]);
		GD_DIAG( "\n");
	}
	/* The payload writes through the secure-bus bridge, not the host
	 * register path; record the same target-write evidence. */
	/* The Booter's release primitive pulses BAR0 0x1180f8 bit 24;
	 * that is the SE common-mutex release doorbell. */
	value &= bar_write_mask(address);
	if (address == 0x1180f8 && (value & (1u << 24)))
		csb_release_all(dev);
	plm_latch_write(dev, address, value);
	bar0_raw_write(dev, address, value);
	return true;
}

static void load_engine_defaults(struct gd_device *dev, struct gd_engine *eng, uint32_t base, int index)
{
	size_t i;
	memset(eng, 0, sizeof(*eng));
	eng->dev = dev;
	gd_doc_init(&eng->doc, gd_doc_io_read, gd_doc_io_write, eng);
	gd_trng_init(&eng->trng, eng->se_regs, sizeof(eng->se_regs) / 4);
	gd_csec_init(&eng->csec, csec_bar_read, csec_bar_write, dev);
	eng->index = index;
	eng->base = base;
	eng->hwcfg = 0x20100u;
	eng->hwcfg2 = HWCFG2_RESET;
	eng->dma_stat = 2;
	eng->csb_enable = true;
	eng->csb_valid = false;
	eng->cpuctl = 0;
	pthread_mutex_init(&eng->lock, NULL);
	pthread_cond_init(&eng->wake, NULL);
	pthread_cond_init(&eng->idle, NULL);
	for (i = 0; i < sizeof(gd_io_defaults) / sizeof(gd_io_defaults[0]); i++) {
		uint32_t addr = gd_io_defaults[i].addr;
		if (addr < 0x1000) {
			eng->io[addr / 4] = gd_io_defaults[i].reset;
			eng->io_valid[addr / 4] = 1;
		}
	}
	/* PLM reset values are all-levels until the Booter locks them. */
	eng->reset_plm = 0xffffffffu;
	eng->io[0x288 / 4] = 0xffffffffu;
	eng->io[0x194 / 4] = 0xffffffffu;
	eng->io[0x28c / 4] = 0xffffffffu;
	eng->io[0x190 / 4] = 0xffffffffu;
	eng->io[0x18c / 4] = 0xffffffffu;
	eng->io[0x2b0 / 4] = 0xffffffffu;
	eng->io[0x3c4 / 4] = eng->reset_plm;
	if (index == 1) {
		eng->cpuctl = CPUCTL_HALTED;
		eng->halted = true;
	}
}

void *gd_bar_pointer(struct gd_device *dev)
{
	return dev ? dev->bar : NULL;
}

const char *gd_asset_root(struct gd_device *dev)
{
	return dev && dev->config.asset_root ? dev->config.asset_root : ".";
}

struct gd_device *gd_create(const struct gd_board *board, const struct gd_run_config *config)
{
	struct gd_device *dev = calloc(1, sizeof(*dev));
	FILE *file;
	long size;

	if (!dev)
		return NULL;
	csb_init(dev);
	dev->fb_size = GD_FB_SIZE;
	dev->fb = calloc(dev->fb_size, 1);
	if (!dev->fb)
		goto fail;
	dev->board = board;
	dev->config = *config;
	if (dev->config.key_set) {
		/* Debug-fused SCP slots: 37 decrypts the HS image, 0 authenticates
		 * the debug DMEM MAC.  The debug secret 0 is the HS key with its
		 * first byte cleared; production slot 0x28 stays unavailable. */
		memcpy(dev->secrets[37], dev->config.key, 16);
		memcpy(dev->secrets[0], dev->config.key, 16);
		dev->secrets[0][0] = 0;
		dev->secret_set[37] = true;
		dev->secret_set[0] = true;
	}
	dev->bar = calloc(GD_BAR_WORDS, sizeof(uint32_t));
	dev->bar_valid = calloc(GD_BAR_WORDS, 1);
	dev->prom = calloc(PROM_SIZE, 1);
	if (!dev->bar || !dev->bar_valid || !dev->prom)
		goto fail;
	pthread_mutex_init(&dev->lock, NULL);
	pthread_mutex_init(&dev->bar_lock, NULL);
	pthread_mutex_init(&dev->dma_lock, NULL);
	pthread_mutex_init(&dev->trace_lock, NULL);
	if (!gd_aes128_selftest())
		gd_log(dev, GD_LOG_ERROR, "device: AES-128 self-test failed\n");
	load_bar_defaults(dev);
	/* The layout constrains every reservation to the top 256 MiB of usable
	 * framebuffer, so one window derived from the strap covers all of them. */
	dev->fb_base = (uint64_t)dev->bar[0x1183a4 / 4] << 20;
	dev->fb_base = dev->fb_base > GD_FB_SIZE ? dev->fb_base - GD_FB_SIZE : 0;
	load_engine_defaults(dev, &dev->engine[0], SEC2_BASE, 0);
	load_engine_defaults(dev, &dev->engine[1], GSP_BASE, 1);
	load_code_table(&dev->engine[1], config->code_path);
	load_code_table(&dev->engine[0], config->booter_code_path);
	dev->pci_command = 0x2;
	set_bar_locked(dev, 0x200, 0x2);
	dev->time_us = now_us();

	memset(dev->prom, 0xff, PROM_SIZE);
	file = fopen(config->rom_path, "rb");
	if (!file) {
		gd_log(dev, GD_LOG_ERROR, "device: cannot open ROM %s\n", config->rom_path);
		goto fail;
	}
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	if (size > 0) {
		if (size > (long)PROM_SIZE)
			size = PROM_SIZE;
		if (fread(dev->prom, 1, (size_t)size, file) != (size_t)size) {
			fclose(file);
			goto fail;
		}
	}
	fclose(file);
	return dev;
fail:
	gd_destroy(dev);
	return NULL;
}

void gd_destroy(struct gd_device *dev)
{
	int i;
	if (!dev)
		return;
	for (i = 0; i < 2; i++) {
		struct gd_engine *eng = &dev->engine[i];
		if (eng->thread_up) {
			pthread_mutex_lock(&eng->lock);
			eng->stop = true;
			pthread_cond_signal(&eng->wake);
			pthread_mutex_unlock(&eng->lock);
			pthread_join(eng->thread, NULL);
			eng->thread_up = false;
		}
		pthread_mutex_destroy(&eng->lock);
		pthread_cond_destroy(&eng->wake);
		pthread_cond_destroy(&eng->idle);
	}
	pthread_mutex_destroy(&dev->trace_lock);
	for (i = 0; i < 2; i++) {
		free(dev->engine[i].code);
		free(dev->engine[i].code_index);
	}
	free(dev->fb);
	free(dev->bar);
	free(dev->bar_valid);
	free(dev->prom);
	free(dev);
}

void gd_set_engines(struct gd_device *dev, bool enabled)
{
	int i;
	for (i = 0; i < 2; i++) {
		struct gd_engine *eng = &dev->engine[i];
		eng->enabled = enabled;
		if (enabled && !eng->thread_up) {
			eng->stop = false;
			if (pthread_create(&eng->thread, NULL, engine_thread, eng) == 0) {
				eng->thread_up = true;
				pthread_mutex_lock(&eng->lock);
				pthread_cond_signal(&eng->wake);
				pthread_mutex_unlock(&eng->lock);
			} else {
				eng->enabled = false;
			}
		}
	}
}

bool gd_engine_progress(struct gd_device *dev, int engine, uint64_t *instructions)
{
	struct gd_engine *eng;
	if (engine < 0 || engine > 1)
		return false;
	eng = &dev->engine[engine];
	pthread_mutex_lock(&eng->lock);
	if (instructions)
		*instructions = eng->instructions;
	bool progress = eng->instructions != 0;
	pthread_mutex_unlock(&eng->lock);
	return progress;
}

void gd_advance_us(struct gd_device *dev, uint64_t usec)
{
	int i;
	dev->time_us += usec;
	for (i = 0; i < 2; i++) {
		struct gd_engine *eng = &dev->engine[i];
		pthread_mutex_lock(&eng->lock);
		settle_scrub(dev, eng);
		pthread_mutex_unlock(&eng->lock);
	}
}

int gd_reset(struct gd_device *dev)
{
	int i;
	dev->resets++;
	dev->flr_seen = true;
	for (i = 0; i < 2; i++) {
		struct gd_engine *eng = &dev->engine[i];
		pthread_mutex_lock(&eng->lock);
		while (eng->busy)
			pthread_cond_wait(&eng->idle, &eng->lock);
		engine_reset_state(dev, eng);
		/* FLR resets the function; MAILBOX0/1 DATA_INIT is 0. */
		eng->mailbox[0] = eng->mailbox[1] = 0;
		eng->io[0x0f4 / 4] = HWCFG2_RESET;
		eng->io_valid[0x0f4 / 4] = 1;
		pthread_mutex_unlock(&eng->lock);
	}
	pthread_mutex_lock(&dev->bar_lock);
	uint32_t ss0 = dev->bar[0x82381c / 4];
	uint32_t ss1 = dev->bar[0x823820 / 4];
	load_bar_defaults(dev);
	set_bar_locked(dev, 0x2240c, 0x2);
	set_bar_locked(dev, 0xa00, dev->board->chip << 20);
		set_bar_locked(dev, 0x1180fc, 1u << 20);
	set_bar_locked(dev, 0x1180f8, 0u); /* BSI scratch 14: no level/error */
	set_bar_locked(dev, 0x118220, 0u);
	set_bar_locked(dev, 0x118244, 0u); /* declared pre-FWSEC boundary: FRTS not cached */
	set_bar_locked(dev, 0x118248, 0u);
	set_bar_locked(dev, 0x118c6c, 0u);

	/* GC6 AON secure-scratch bootstrap latches (groups 40/41).  The real
	 * early boot/SCP flow writes these before FWSEC; we do not emulate that
	 * flow, so seed the post-bootstrap values here.  This is a test fixture,
	 * not evidence of the hardware sequence.
	 */
	set_bar_locked(dev, 0x11839c, 0xff);
	set_bar_locked(dev, 0x1183a0, 0xff);
	set_bar_locked(dev, 0x82381c, ss0);
	set_bar_locked(dev, 0x823820, ss1);
	dev->ss0 = ss0;
	dev->ss1 = ss1;
	dev->pci_command &= ~0x4u;
	dev->bar[0x200 / 4] = dev->pci_command;
	dev->pci_saved_command = dev->pci_command;
	pthread_mutex_unlock(&dev->bar_lock);
	gd_log(dev, GD_LOG_INFO, "device: FLR\n");
	trace_add(dev, GD_TRACE_FLR, 0, 0, 0, 0);
	return 0;
}

uint32_t gd_trace_count(struct gd_device *dev)
{
	return dev ? dev->trace_count : 0;
}

int gd_trace_get(struct gd_device *dev, uint32_t index, struct gd_trace_record *record)
{
	uint32_t first;
	if (!dev || !record || index >= dev->trace_count)
		return -EINVAL;
	first = dev->trace_head >= GD_MAX_TRACE ? dev->trace_head - GD_MAX_TRACE : 0;
	*record = dev->trace[(first + index) % GD_MAX_TRACE];
	return 0;
}

int gd_trace_dump(struct gd_device *dev, const char *path)
{
	FILE *file;
	uint32_t i;
	if (!path)
		return 0;
	file = fopen(path, "wb");
	if (!file)
		return -EIO;
	fwrite(&dev->trace_count, sizeof(dev->trace_count), 1, file);
	for (i = 0; i < dev->trace_count; i++) {
		struct gd_trace_record record;
		gd_trace_get(dev, i, &record);
		fwrite(&record, sizeof(record), 1, file);
	}
	fclose(file);
	return 0;
}

void gd_collect(struct gd_device *dev, struct gd_result *out)
{
	uint32_t i;
	memset(out, 0, sizeof(*out));
	out->driver_result = 0;
	out->device_error = dev->device_error;
	out->fwsec_launch_reached = dev->fwsec_launch_reached;
	out->booter_launch_reached = dev->booter_launch_reached;
	out->fwsec_kdf_ok = dev->fwsec_kdf_ok;
	out->flr_seen = dev->flr_seen;
	out->launches[0] = dev->launches[0];
	out->launches[1] = dev->launches[1];
	out->resets = dev->resets;
	out->host_writes = dev->host_writes;
	out->host_reads = dev->host_reads;
	out->engine_writes = dev->engine_writes;
	out->engine_reads = dev->engine_reads;
	out->instructions[0] = dev->engine[0].instructions;
	out->instructions[1] = dev->engine[1].instructions;
	out->unknown_count = dev->unknown_count;
	for (i = 0; i < dev->unknown_count && i < GD_MAX_UNKNOWN; i++)
		out->unknown[i] = dev->unknown[i];
	out->feat_count = dev->feat_count;
	for (i = 0; i < dev->feat_count && i < 8; i++)
		out->feats[i] = dev->feats[i];
	out->wpr2_lo = dev->bar[0x1fa824 / 4];
	out->wpr2_hi = dev->bar[0x1fa828 / 4];
	out->ss0 = dev->bar[0x82381c / 4];
	out->ss1 = dev->bar[0x823820 / 4];
	out->pci_command = dev->pci_command;
	out->dma_live = dev->dma_count;
	out->fault_pc = dev->fault_pc;
	out->fault_opcode = dev->fault_opcode;
	out->plm_feat = dev->plm_feat_latched;
	out->plm_wpr_cfg = dev->plm_wpr_cfg_latched;
	out->plm_wpr = dev->bar[0x1fa7c4 / 4];
	out->denied_writes = dev->denied_writes;
	out->booter_error = dev->engine[0].io[0x1000 / 4];
	out->fwsec_executed = dev->engine[1].instructions > 0 &&
		!dev->engine[1].core_fault;
	out->wpr2_normalized = dev->wpr2_normalized;
	out->fwsec_mac_ok = dev->engine[1].fwsec_kdf_seen &&
		(!memcmp(dev->engine[1].fwsec_kdf_out, dev->engine[1].fwsec_expect, 16) ||
		 !memcmp(dev->engine[1].fwsec_kdf_out, dev->engine[1].fwsec_expect + 16, 16));
	out->fwsec_kdf_ok = out->fwsec_mac_ok && !(dev->bar[0x1438 / 4] >> 16);
	out->booter_executed = dev->engine[0].instructions > 0 &&
		!dev->engine[0].core_fault;
	out->booter_halted = dev->engine[0].instructions > 0 &&
		dev->engine[0].halted;
	out->plm_opened = dev->plm_opened_seen;
	out->target_written = dev->target_write_seen;
	out->unlock_verified = out->fwsec_executed &&
		out->booter_executed && out->plm_opened && out->target_written &&
		out->ss0 == 0x88888888u && out->ss1 == 0x8u &&
		dev->engine[0].reset_plm == 0xffffffffu;
	out->fwsec_halt_pc = dev->engine[1].halt_pc;
	out->booter_halt_pc = dev->engine[0].halt_pc;
	out->fwsec_status = dev->bar[0x1438 / 4];
	out->booter_status = dev->engine[0].mailbox[0];
	out->booter_mailbox0 = dev->engine[0].mailbox[0];
	out->booter_mailbox1 = dev->engine[0].mailbox[1];
	out->booter_reset_plm = dev->engine[0].reset_plm;
	out->group03 = dev->bar[0x11821c / 4];
	if (!dev->fwsec_launch_reached)
		out->stage = GD_STAGE_PRE_FWSEC;
	else if (!dev->engine[1].halted || dev->engine[1].core_fault)
		out->stage = GD_STAGE_FWSEC_RUNNING;
	else if (out->wpr2_hi >> 4 && (out->group03 & (3u << 16)) == (3u << 16))
		out->stage = dev->booter_launch_reached ? GD_STAGE_BOOTER_RUNNING
							: GD_STAGE_WPR2_READY;
	else
		out->stage = GD_STAGE_FWSEC_DONE;
	if (dev->booter_launch_reached && dev->engine[0].halted) {
		if (dev->engine[0].core_fault || out->booter_status)
			out->stage = GD_STAGE_BOOTER_FAILED;
		else if (out->stage == GD_STAGE_BOOTER_RUNNING)
			out->stage = GD_STAGE_BOOTER_HALTED;
	}
}
