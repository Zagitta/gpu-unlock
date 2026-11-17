// SPDX-License-Identifier: GPL-2.0-only
/*
 * VRAM temperatures through hwmon: one channel per GDDR chip or HBM stack,
 * plus the hottest. The memory controllers (FBPA) latch what the firmware
 * polls from the DRAM, so this only reads BAR0 and never writes the card.
 *
 * GDDR channels use the MODS names, the controller as the letter and the
 * subpartition as the digit, then the byte of the subpartition's word. On a
 * clamshell 3090 the two bytes are the chips on either side of the board; what
 * they are on other boards is unknown. The controllers report nothing until
 * nvidia has started the card, so each card is polled until they do and
 * registered only then, with the controllers it has.
 */
#include <linux/hwmon.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "recipes.h"
#include "vram.h"

#define FBPA_BASE      0x00900000u
#define FBPA_STRIDE    0x00004000u
#define FBPAS          24u
#define FBPA_CFG1      0x0204u /* 0xbadf.... when the controller is floorswept */
/* GDDR: a word per subpartition, a chip per byte in [31:16]. [15:0] mirrors
 * them at idle clocks only. */
#define FBPA_DQR       0x24c0u
#define FBPA_DQR_VALID 0x24d0u /* [27:24] all set when the words are current */
/* HBM: the stack's temperature in [30:24], [31] set when invalid. */
#define FBPA_WDR       0x3cbcu

#define CHANNELS       (1 + FBPAS * 4)
#define POLL_INTERVAL  (5 * HZ)

/* The two FBPs, four controllers, under each GA100 HBM stack, as measured by
 * hbmmon. */
static const u8 hbm_stacks[][2] = { { 0, 2 }, { 3, 5 }, { 7, 9 }, { 8, 10 }, { 1, 4 }, { 6, 11 } };

#ifdef GU_PM_ACTIVE_HAS_IGNORE
#define get_if_active(dev) pm_runtime_get_if_active(dev, true)
#else
#define get_if_active(dev) pm_runtime_get_if_active(dev)
#endif

struct sensor {
	u32 off;   /* from FBPA_BASE */
	u8 shift;  /* of the chip's byte, GDDR only */
};

struct vram {
	struct list_head node;
	struct pci_dev *pdev;
	enum gu_vram kind;
	void __iomem *fbpa; /* FBPA_BASE onwards */
	struct device *hwmon;
	bool done; /* registered, or never will be */
	unsigned int count; /* channels, the hottest first */
	struct sensor sensors[CHANNELS];
	char labels[CHANNELS][12];
	u32 config[CHANNELS + 1];
	struct hwmon_channel_info info;
	const struct hwmon_channel_info *infos[2];
	struct hwmon_chip_info chip;
};

static LIST_HEAD(vrams);
static void poll(struct work_struct *work);
static DECLARE_DELAYED_WORK(poll_work, poll);

/* Holds the card awake for a few reads, provided nvidia drives it and nobody
 * holds its device lock: an unlock or a probe may be resetting it. Pass the
 * result to end() when it is not negative. */
static int begin(struct vram *v)
{
	struct device *dev = &v->pdev->dev;
	int pm = get_if_active(dev), ret = -ENODATA;
	u16 command = 0;

	if (pm == 0)
		return -ENODATA;
	if (!device_trylock(dev)) {
		ret = -EBUSY;
	} else {
		pci_read_config_word(v->pdev, PCI_COMMAND, &command);
		if (v->pdev->driver && !strcmp(v->pdev->driver->name, "nvidia") &&
		    v->pdev->current_state == PCI_D0 && (command & PCI_COMMAND_MEMORY))
			return pm > 0;
		device_unlock(dev);
	}
	if (pm > 0)
		pm_runtime_put(dev);
	return ret;
}

static void end(struct vram *v, int token)
{
	device_unlock(&v->pdev->dev);
	if (token > 0)
		pm_runtime_put(&v->pdev->dev);
}

static u32 rd(struct vram *v, u32 off)
{
	return readl(v->fbpa + off);
}

static bool live(struct vram *v, unsigned int fbpa)
{
	return (rd(v, fbpa * FBPA_STRIDE + FBPA_CFG1) & 0xffff0000u) != 0xbadf0000u;
}

static int read_sensor(struct vram *v, const struct sensor *s, long *val)
{
	u32 raw = rd(v, s->off);
	u32 code = (raw >> s->shift) & 0xff;

	if ((raw & 0xffff0000u) == 0xbadf0000u)
		return -ENODATA;
	if (v->kind == GU_VRAM_HBM) {
		if (raw & BIT(31))
			return -ENODATA;
		*val = ((raw >> 24) & 0x7f) * 1000;
		return 0;
	}
	if (!code ||
	    ((rd(v, rounddown(s->off, FBPA_STRIDE) + FBPA_DQR_VALID) >> 24) & 0xf) != 0xf)
		return -ENODATA;
	/* GDDR temperature code: 2 degrees a step, 20 is 0 C. */
	*val = ((long)code - 20) * 2000;
	return 0;
}

static int read_hottest(struct vram *v, long *val)
{
	long temp, hottest = LONG_MIN;
	unsigned int i;

	for (i = 1; i < v->count; i++)
		if (!read_sensor(v, &v->sensors[i], &temp) && temp > hottest)
			hottest = temp;
	if (hottest == LONG_MIN)
		return -ENODATA;
	*val = hottest;
	return 0;
}

static void add(struct vram *v, u32 off, u8 shift, const char *fmt, ...)
{
	struct sensor *s = &v->sensors[v->count];
	va_list args;

	s->off = off;
	s->shift = shift;
	va_start(args, fmt);
	vscnprintf(v->labels[v->count], sizeof(v->labels[0]), fmt, args);
	va_end(args);
	v->count++;
}

/* Lists the sensors of the live controllers. Returns how many report now. */
static unsigned int find_sensors(struct vram *v)
{
	unsigned int i, j, reporting = 0;
	long temp;

	v->count = 0;
	add(v, 0, 0, "Hottest");
	if (v->kind == GU_VRAM_GDDR) {
		for (i = 0; i < FBPAS; i++) {
			if (!live(v, i))
				continue;
			for (j = 0; j < 2; j++) {
				add(v, i * FBPA_STRIDE + FBPA_DQR + j * 4, 24, "%c%u.0", 'A' + i, j);
				add(v, i * FBPA_STRIDE + FBPA_DQR + j * 4, 16, "%c%u.1", 'A' + i, j);
			}
		}
	} else {
		for (i = 0; i < ARRAY_SIZE(hbm_stacks); i++) {
			const u8 fbpas[] = { 2 * hbm_stacks[i][0], 2 * hbm_stacks[i][0] + 1,
					     2 * hbm_stacks[i][1], 2 * hbm_stacks[i][1] + 1 };

			for (j = 0; j < ARRAY_SIZE(fbpas); j++) {
				if (live(v, fbpas[j])) {
					add(v, fbpas[j] * FBPA_STRIDE + FBPA_WDR, 0, "HBM %c", 'A' + i);
					break;
				}
			}
		}
	}
	for (i = 1; i < v->count; i++)
		reporting += !read_sensor(v, &v->sensors[i], &temp);
	return reporting;
}

static umode_t vram_visible(const void *data, enum hwmon_sensor_types type, u32 attr,
			    int channel)
{
	return 0444;
}

static int vram_read(struct device *dev, enum hwmon_sensor_types type, u32 attr,
		     int channel, long *val)
{
	struct vram *v = dev_get_drvdata(dev);
	int token = begin(v), ret;

	if (token < 0)
		return token;
	ret = channel ? read_sensor(v, &v->sensors[channel], val) : read_hottest(v, val);
	end(v, token);
	return ret;
}

static int vram_read_string(struct device *dev, enum hwmon_sensor_types type, u32 attr,
			    int channel, const char **str)
{
	struct vram *v = dev_get_drvdata(dev);

	*str = v->labels[channel];
	return 0;
}

static const struct hwmon_ops vram_ops = {
	.is_visible = vram_visible,
	.read = vram_read,
	.read_string = vram_read_string,
};

/* Registers the card once its controllers report. */
static void try_register(struct vram *v)
{
	struct pci_dev *pdev = v->pdev;
	unsigned int i, reporting;
	int token;

	if (!v->fbpa) {
		if (pci_resource_len(pdev, 0) < FBPA_BASE + FBPAS * FBPA_STRIDE ||
		    !(v->fbpa = pci_iomap_range(pdev, 0, FBPA_BASE, FBPAS * FBPA_STRIDE))) {
			pci_err(pdev, "gpu_unlock: memory controllers not mapped, no VRAM temperatures\n");
			v->done = true;
			return;
		}
	}
	token = begin(v);
	if (token < 0)
		return;
	reporting = find_sensors(v);
	end(v, token);
	if (!reporting)
		return;

	v->done = true;
	for (i = 0; i < v->count; i++)
		v->config[i] = HWMON_T_INPUT | HWMON_T_LABEL;
	v->info.type = hwmon_temp;
	v->info.config = v->config;
	v->infos[0] = &v->info;
	v->chip.ops = &vram_ops;
	v->chip.info = v->infos;
	v->hwmon = hwmon_device_register_with_info(&pdev->dev, "vram", v, &v->chip, NULL);
	if (IS_ERR(v->hwmon)) {
		pci_err(pdev, "gpu_unlock: VRAM hwmon not registered (%ld)\n", PTR_ERR(v->hwmon));
		v->hwmon = NULL;
		return;
	}
	pci_info(pdev, "gpu_unlock: %u VRAM temperatures as %s\n", v->count - 1,
		 dev_name(v->hwmon));
}

static void poll(struct work_struct *work)
{
	struct vram *v;
	bool pending = false;

	list_for_each_entry(v, &vrams, node) {
		if (!v->done)
			try_register(v);
		pending |= !v->done;
	}
	if (pending)
		schedule_delayed_work(&poll_work, POLL_INTERVAL);
}

void gu_vram_publish(void)
{
	struct pci_dev *pdev = NULL;
	const struct gu_card *recipe;
	struct vram *v;

	while ((pdev = pci_get_device(PCI_VENDOR_ID_NVIDIA, PCI_ANY_ID, pdev))) {
		recipe = gu_card_find(pdev->device);
		if (!recipe || recipe->family->vram == GU_VRAM_NONE)
			continue;
		v = kzalloc(sizeof(*v), GFP_KERNEL);
		if (!v) {
			pci_dev_put(pdev);
			break;
		}
		v->pdev = pci_dev_get(pdev);
		v->kind = recipe->family->vram;
		list_add_tail(&v->node, &vrams);
	}
	if (!list_empty(&vrams))
		schedule_delayed_work(&poll_work, 0);
}

void gu_vram_remove(void)
{
	struct vram *v, *next;

	cancel_delayed_work_sync(&poll_work);
	list_for_each_entry_safe(v, next, &vrams, node) {
		if (v->hwmon)
			hwmon_device_unregister(v->hwmon);
		if (v->fbpa)
			pci_iounmap(v->pdev, v->fbpa);
		pci_dev_put(v->pdev);
		list_del(&v->node);
		kfree(v);
	}
}
