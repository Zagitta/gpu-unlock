// SPDX-License-Identifier: GPL-2.0-only
/*
 * Boot snapshot of each card's fuse block, taken once after the unlock and
 * served read-only as text. Raw rows are read through the fuse controller:
 * only FUSEADDR and FUSECTRL are written, and both are restored. DEBUGCTRL,
 * IFF_RECORD, FUSEWDATA, IFF_SW_FUSING and the PLMs are never written, so rows
 * inside the factory IFF window may read back IFF content. Topology comes from
 * the effective STATUS_OPT_TPC_GPC masks, which need no writes at all.
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kobject.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/seq_buf.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/timekeeping.h>
#include <linux/version.h>

#include "device.h"
#include "fuses.h"
#include "recipes.h"

/* Offsets from the generation's fuse block base. */
#define FUSECTRL               0x000u
#define FUSEADDR               0x004u
#define FUSERDATA              0x008u
#define IFF_RECORD             0x050u
#define STATUS_OPT_GPC         0xc1cu
#define CTRL_OPT_TPC_GPC(i)    (0x838u + (i) * 4u)
#define STATUS_OPT_TPC_GPC(i)  (0xc38u + (i) * 4u)
#define PTOP_SCAL_NUM_GPCS        0x00022430u
#define PTOP_SCAL_NUM_TPC_PER_GPC 0x00022434u

#define CMD_MASK   3u
#define CMD_READ   1u
#define STATE_MASK (0x1fu << 16)
#define STATE_IDLE (4u << 16)
#define MAX_ROWS   1024u
#define TEXT_MAX   SZ_16K

static const struct {
	const char *name;
	u32 offset;
	bool ampere; /* absent on TU10x */
} status_regs[] = {
	{ "FUSECTRL", FUSECTRL },
	{ "FUSEADDR", FUSEADDR },
	{ "FUSERDATA", FUSERDATA },
	{ "EN_SW_OVERRIDE", 0x040 },
	{ "IFF_RECORD", IFF_RECORD },
	{ "DEBUGCTRL", 0x060 },
	{ "DISABLE_SW_OVERRIDE_STATUS", 0x084, true },
	{ "IFF_SW_FUSING", 0x088, true },
	{ "TPC_FLOORSWEEP_PLM", 0x0d0, true },
	{ "RECONFIG_PLM", 0x0d8 },
	{ "FLOORSWEEP_PLM", 0x0dc },
	{ "DEBUGCTRL_PLM", 0x0e0 },
	{ "WDATA_PLM", 0x0f4 },
	{ "FUSECTRL_PLM", 0x0f8 },
	{ "OPT_PLM", 0x0fc },
	{ "OPT_GPC_DISABLE", 0x350 },
	{ "OPT_GPC_DEFECTIVE", 0x5c4 },
	{ "STATUS_OPT_GPC", STATUS_OPT_GPC },
};

struct rows {
	u32 words[MAX_ROWS];
	unsigned int count, read;
	int error, restore;
	s64 us;
};

struct snapshot {
	struct list_head node;
	struct bin_attribute attr;
	char name[16];
	char *text;
};

static LIST_HEAD(snapshots);
static DEFINE_MUTEX(snapshots_lock);
static struct kobject *fuses_kobj;

/* A PRI error, or a register that did not answer at all. */
static bool bad(u32 value)
{
	return value == ~0u || (value & 0xffff0000u) == 0xbadf0000u;
}

static int read_row(void __iomem *fuse, u32 row, u32 *word)
{
	unsigned int step;
	u32 ctrl = readl(fuse + FUSECTRL);

	if (bad(ctrl) || (ctrl & STATE_MASK) != STATE_IDLE || (ctrl & CMD_MASK) > CMD_READ)
		return -EBUSY;
	writel(row, fuse + FUSEADDR);
	if (readl(fuse + FUSEADDR) != row)
		return -EACCES;
	writel((ctrl & ~CMD_MASK) | CMD_READ, fuse + FUSECTRL);
	/* The idle state can show before the command is accepted. */
	usleep_range(50, 100);
	for (step = 0;; step++) {
		ctrl = readl(fuse + FUSECTRL);
		if (bad(ctrl) || (ctrl & CMD_MASK) > CMD_READ)
			return -EIO;
		if ((ctrl & STATE_MASK) == STATE_IDLE)
			break;
		if (step == 1000)
			return -ETIMEDOUT;
		udelay(10);
	}
	*word = readl(fuse + FUSERDATA);
	writel(ctrl & ~CMD_MASK, fuse + FUSECTRL);
	/* All-ones is a valid fuse word; only BADF marks a PRI error. */
	return (*word & 0xffff0000u) == 0xbadf0000u ? -ENODEV : 0;
}

static void read_rows(void __iomem *fuse, struct rows *r)
{
	u32 ctrl = readl(fuse + FUSECTRL), addr = readl(fuse + FUSEADDR);
	ktime_t start = ktime_get();

	if (bad(ctrl) || (ctrl & STATE_MASK) != STATE_IDLE || (ctrl & CMD_MASK)) {
		r->error = -EBUSY;
		return;
	}
	while (!r->error && r->read < r->count) {
		r->error = read_row(fuse, r->read, &r->words[r->read]);
		r->read += !r->error;
	}
	writel(addr, fuse + FUSEADDR);
	writel(ctrl, fuse + FUSECTRL);
	if (readl(fuse + FUSEADDR) != addr || readl(fuse + FUSECTRL) != ctrl)
		r->restore = -EIO;
	r->us = ktime_us_delta(ktime_get(), start);
}

static void print_reg(struct seq_buf *s, void __iomem *bar, const char *name, u32 addr)
{
	seq_buf_printf(s, "reg %s %08x %08x\n", name, addr, readl(bar + addr));
}

/* Factory, control and effective masks per GPC; the TPC count comes from the
 * effective STATUS masks only. */
static void print_topology(struct seq_buf *s, void __iomem *bar, const struct gu_fuse_layout *layout)
{
	u32 gpc_status = readl(bar + layout->base + STATUS_OPT_GPC);
	u32 mask = (1u << layout->tpc_bits) - 1;
	bool valid = !bad(gpc_status);
	unsigned int i, tpcs = 0;

	seq_buf_printf(s, "topology gpcs %u tpc_per_gpc %u\n",
		       readl(bar + PTOP_SCAL_NUM_GPCS), readl(bar + PTOP_SCAL_NUM_TPC_PER_GPC));
	for (i = 0; i < layout->gpcs; i++) {
		u32 status = readl(bar + layout->base + STATUS_OPT_TPC_GPC(i));

		seq_buf_printf(s, "gpc %u disable %08x defect %08x ctrl %08x status %08x\n", i,
			       readl(bar + layout->tpc_disable[i]), readl(bar + layout->tpc_defect[i]),
			       readl(bar + layout->base + CTRL_OPT_TPC_GPC(i)), status);
		if (bad(status) || status & ~mask)
			valid = false;
		else if (!(gpc_status & BIT(i)))
			tpcs += layout->tpc_bits - hweight32(status);
	}
	if (valid)
		seq_buf_printf(s, "active tpcs %u sms %u\n", tpcs, tpcs * 2);
}

static void print_rows(struct seq_buf *s, const struct rows *r)
{
	unsigned int i;

	seq_buf_printf(s, "rows %u read %u\n", r->count, r->read);
	for (i = 0; i < r->read; i++) {
		if (i % 8 == 0)
			seq_buf_printf(s, "fuse %03x", i);
		seq_buf_printf(s, " %08x", r->words[i]);
		if (i % 8 == 7 || i == r->read - 1)
			seq_buf_printf(s, "\n");
	}
	seq_buf_printf(s, "error %d restore %d time_us %lld\n", r->error, r->restore, r->us);
}

/* The whole report; BAR0 may be NULL when the card could not be mapped. */
static void print_report(struct seq_buf *s, const struct gu_device *device, void __iomem *bar,
			 int unlock, struct rows *r)
{
	struct pci_dev *pdev = device->pdev;
	const struct gu_family *family = device->recipe->family;
	const struct gu_fuse_layout *layout = family->fuse;
	u32 iff, field = (1u << layout->addr_bits) - 1, boot0;
	unsigned int i;

	seq_buf_printf(s, "format 1\npci %04x:%04x subsystem %04x:%04x revision %02x\n",
		       pdev->vendor, pdev->device, pdev->subsystem_vendor,
		       pdev->subsystem_device, pdev->revision);
	seq_buf_printf(s, "family %s\nunlock %d\n", family->name, unlock);
	if (!bar) {
		seq_buf_printf(s, "error %d\n", -ENOMEM);
		return;
	}
	boot0 = readl(bar);
	seq_buf_printf(s, "boot0 %08x\n", boot0);
	if (bad(boot0) || ((boot0 >> 20) & 0x1ff) != device->recipe->chip) {
		seq_buf_printf(s, "error %d\n", -ENODEV);
		return;
	}
	for (i = 0; i < ARRAY_SIZE(status_regs); i++)
		if (layout->ampere || !status_regs[i].ampere)
			print_reg(s, bar, status_regs[i].name, layout->base + status_regs[i].offset);
	print_reg(s, bar, "FEAT_PLM", family->feat->plm);
	print_reg(s, bar, "FEAT_SS0", family->feat->ss0);
	print_reg(s, bar, "FEAT_SS1", family->feat->ss1);
	print_reg(s, bar, "FEAT_READOUT", family->feat->readout);
	print_topology(s, bar, layout);
	/* START > LAST: no IFF window. Otherwise rows START..LAST may be IFF RAM. */
	iff = readl(bar + layout->base + IFF_RECORD);
	seq_buf_printf(s, "iff start %03x last %03x\n", iff & field, (iff >> 16) & field);
	r->count = 1u << layout->addr_bits;
	read_rows(bar + layout->base, r);
	print_rows(s, r);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 9, 0)
static ssize_t read_snapshot(struct file *file, struct kobject *kobj, struct bin_attribute *attr,
			     char *buf, loff_t off, size_t count)
{
	return memory_read_from_buffer(buf, count, &off, attr->private, attr->size);
}
#endif

void gu_fuses_capture(const struct gu_device *device, int unlock)
{
	struct pci_dev *pdev = device->pdev;
	struct snapshot *snap = kzalloc(sizeof(*snap), GFP_KERNEL);
	struct rows *r = kzalloc(sizeof(*r), GFP_KERNEL);
	char *buf = kvmalloc(TEXT_MAX, GFP_KERNEL);
	void __iomem *bar = NULL;
	bool enabled = false;
	struct seq_buf s;
	u16 command;

	if (!snap || !r || !buf) {
		pci_err(pdev, "gpu_unlock: fuse snapshot allocation failed\n");
		goto out;
	}
	/* Memory decoding is left as the unlock handed the card back. */
	pci_read_config_word(pdev, PCI_COMMAND, &command);
	if (!(command & PCI_COMMAND_MEMORY) && !pci_enable_device_mem(pdev))
		enabled = true;
	if (pci_resource_len(pdev, 0) >= SZ_16M)
		bar = pci_iomap(pdev, 0, SZ_16M);
	seq_buf_init(&s, buf, TEXT_MAX);
	print_report(&s, device, bar, unlock, r);
	if (bar)
		pci_iounmap(pdev, bar);
	if (enabled)
		pci_disable_device(pdev);

	snap->text = kmemdup(buf, seq_buf_used(&s), GFP_KERNEL);
	if (!snap->text) {
		pci_err(pdev, "gpu_unlock: fuse snapshot allocation failed\n");
		goto out;
	}
	strscpy(snap->name, pci_name(pdev), sizeof(snap->name));
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
	snap->attr = (struct bin_attribute)__BIN_ATTR_SIMPLE_RO(fuses, 0444);
#else
	snap->attr = (struct bin_attribute)__BIN_ATTR(fuses, 0444, read_snapshot, NULL, 0);
#endif
	sysfs_bin_attr_init(&snap->attr);
	snap->attr.attr.name = snap->name;
	snap->attr.private = snap->text;
	snap->attr.size = seq_buf_used(&s);
	mutex_lock(&snapshots_lock);
	list_add_tail(&snap->node, &snapshots);
	mutex_unlock(&snapshots_lock);
	snap = NULL;
out:
	kvfree(buf);
	kfree(r);
	kfree(snap);
}

void gu_fuses_publish(void)
{
	struct snapshot *snap;
	int ret;

	if (list_empty(&snapshots))
		return;
	fuses_kobj = kobject_create_and_add("fuses", &THIS_MODULE->mkobj.kobj);
	if (!fuses_kobj) {
		pr_err("gpu_unlock: fuse snapshots not published\n");
		return;
	}
	list_for_each_entry(snap, &snapshots, node) {
		ret = sysfs_create_bin_file(fuses_kobj, &snap->attr);
		if (ret)
			pr_err("gpu_unlock: fuse snapshot %s not published (%d)\n", snap->name, ret);
	}
}

void gu_fuses_remove(void)
{
	struct snapshot *snap, *next;

	/* kobject_del() removes the files and waits for open reads; the final
	 * put alone may release the directory later. */
	if (fuses_kobj) {
		kobject_del(fuses_kobj);
		kobject_put(fuses_kobj);
	}
	list_for_each_entry_safe(snap, next, &snapshots, node) {
		list_del(&snap->node);
		kfree(snap->text);
		kfree(snap);
	}
}
