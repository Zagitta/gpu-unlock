/* SPDX-License-Identifier: GPL-2.0-only */
/* Native GA102 device library used by the driver integration harness.
 *
 * The device owns the BAR0 shadow, two Falcon execution contexts, DMA
 * mappings and the reset/interrupt state. The host driver reaches it through
 * the kernel shim in tests/integration/shim. No Python is involved in an
 * access.
 */
#ifndef GD_DEVICE_H
#define GD_DEVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GD_BAR_SIZE          0x01000000u
#define GD_BAR_WORDS         (GD_BAR_SIZE / 4u)
#define GD_MAX_DMA           32768
#define GD_MAX_UNKNOWN       16
#define GD_MAX_TRACE         65536

enum gd_trace_kind { GD_TRACE_BAR_READ, GD_TRACE_BAR_WRITE, GD_TRACE_DMA, GD_TRACE_START, GD_TRACE_HALT, GD_TRACE_FLR, GD_TRACE_FAULT, GD_TRACE_PC };

struct gd_trace_record {
	uint64_t time_us;
	uint32_t pc, address, value;
	uint16_t kind, source;
	uint16_t size;
};

enum gd_log_level { GD_LOG_DEBUG, GD_LOG_INFO, GD_LOG_ERROR };

struct gd_device;

struct gd_board {
	uint16_t device;            /* PCI device id, selects the driver recipe */
	uint32_t chip;              /* PMC_BOOT_0 architecture+implementation */
};

struct gd_run_config {
	const char *asset_root;     /* directory holding the driver's firmware names */
	const char *rom_path;
	bool verbose;
	bool key_set;              /* debug key present; execution permitted */
	uint8_t key[16];           /* AES-128 debug key for FWSEC IMEM code */
	const char *code_path;     /* pre-decoded FWSEC instruction table, optional */
	const char *booter_code_path; /* pre-decoded SEC2 Booter instruction table */
};

struct gd_dma_span {
	uint64_t address;
	uint8_t *cpu;
	size_t size;
	bool live;
};

struct gd_unknown {
	bool write;
	uint32_t address;
	uint32_t value;
	uint32_t pc;
};

struct gd_result {
	int driver_result;          /* the driver's return value */
	int device_error;           /* first device-side failure */
	bool fwsec_launch_reached;
	bool booter_launch_reached;
	bool fwsec_executed;
	bool fwsec_kdf_ok;
	bool booter_executed;
	bool booter_halted;
	bool plm_opened;
	bool target_written;
	bool unlock_verified;
	bool flr_seen;
	uint32_t launches[2];
	uint32_t resets;
	uint64_t host_writes;
	uint64_t host_reads;
	uint64_t engine_writes;
	uint64_t engine_reads;
	uint64_t instructions[2];
	uint32_t unknown_count;
	struct gd_unknown unknown[GD_MAX_UNKNOWN];
	uint32_t feats[8];
	uint32_t feat_count;
	uint32_t wpr2_lo, wpr2_hi;
	uint32_t ss0, ss1;
	uint32_t pci_command;
	uint32_t dma_live;
	uint32_t fault_pc, fault_opcode;
	uint32_t plm_feat, plm_wpr_cfg, plm_wpr;
	uint32_t denied_writes;
	uint32_t booter_error;      /* NV_CSEC_FALCON_MAILBOX0 at 0x1000 */
	/* Boot-chain evidence.  The stage is derived from the same registers
	 * and scratch the driver observes. */
	uint32_t stage;             /* enum gd_stage */
	uint32_t fwsec_halt_pc;
	uint32_t booter_halt_pc;
	uint32_t fwsec_status;      /* SW_SCRATCH(14) value left by FWSEC */
	uint32_t booter_status;     /* SEC2 CSB MAILBOX0 status */
	uint32_t booter_mailbox0;
	uint32_t booter_mailbox1;
	uint32_t booter_reset_plm;
	uint32_t group03;           /* AON secure scratch group 03(2) */
	uint32_t fwsec_mac_ok;      /* debug/prod MAC matched the image */
	uint32_t wpr2_normalized;   /* debug FWSEC end encoding converted */
};

enum gd_stage {
	GD_STAGE_PRE_FWSEC = 0,
	GD_STAGE_FWSEC_RUNNING,
	GD_STAGE_FWSEC_DONE,
	GD_STAGE_WPR2_READY,
	GD_STAGE_BOOTER_RUNNING,
	GD_STAGE_BOOTER_HALTED,
	GD_STAGE_BOOTER_FAILED,
};

struct gd_device *gd_create(const struct gd_board *board, const struct gd_run_config *config);
void gd_destroy(struct gd_device *dev);

/* Host BAR0 access. Unknown registers are recorded and fail closed. */
uint32_t gd_bar0_read(struct gd_device *dev, uint32_t address);
void gd_bar0_write(struct gd_device *dev, uint32_t address, uint32_t value);

/* PROM window copy for memcpy_fromio(). */
int gd_prom_copy(struct gd_device *dev, uint32_t offset, void *dest, size_t size);

/* PCI configuration space. space 0 is config, 1 is the PCIe capability. */
int gd_pci_read(struct gd_device *dev, int space, int offset, int width, uint32_t *value);
int gd_pci_write(struct gd_device *dev, int space, int offset, int width, uint32_t value);

/* DMA mappings are identified by their device address. */
int gd_dma_map(struct gd_device *dev, uint64_t address, void *cpu, size_t size);
void gd_dma_unmap(struct gd_device *dev, uint64_t address);
void gd_dma_barrier(struct gd_device *dev);

/* FLR / device reset. */
int gd_reset(struct gd_device *dev);

void gd_advance_us(struct gd_device *dev, uint64_t usec);
void gd_log(struct gd_device *dev, enum gd_log_level level, const char *format, ...)
	__attribute__((format(printf, 3, 4)));

/* Engine threads start when enabled and are joined by gd_destroy(). */
void gd_set_engines(struct gd_device *dev, bool enabled);
bool gd_engine_progress(struct gd_device *dev, int engine, uint64_t *instructions);

void gd_collect(struct gd_device *dev, struct gd_result *out);
uint32_t gd_trace_count(struct gd_device *dev);
int gd_trace_get(struct gd_device *dev, uint32_t index, struct gd_trace_record *record);
int gd_trace_dump(struct gd_device *dev, const char *path);

/* BAR0 shadow used by the shim to turn a kernel pointer into an offset. */
void *gd_bar_pointer(struct gd_device *dev);
const char *gd_asset_root(struct gd_device *dev);

#endif
