// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Only the memory controllers, the framebuffer stop handshake and the PMU
 * activity counters are simulated. The policy is taken directly from hbm.c. */
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define __iomem
#define BUSY_COEFF 0x14001u
#define IDLE_COEFF 0x11401u
#define BUSY_REFRESH 0xc4028033u
#define MS 1000000ull

struct cpumask;
static u32 readl(const void *addr);
static void writel(u32 value, void *addr);
static void udelay(unsigned long us);
static void ndelay(unsigned long ns);
static int stop_machine(int (*fn)(void *), void *data, const struct cpumask *cpus);

#include "hbm.c"

/* A ten-FBP card: controllers 12, 13, 22 and 23 are absent. */
struct card {
	u32 refresh[24], coeff[24], cfg;
	u32 request, ack, count;
	unsigned int ack_reads, lock_reads;	/* reads until the ACK and the lock follow */
	unsigned int ack_wait, lock_wait;
	u64 ns, stopped_at, longest_stop, longest_poll;
	unsigned int stops, clock_writes;
	bool cpus_held;				/* no other CPU can reach the card */
};
static struct card card;
static char bar[1];

static bool present(unsigned int i)
{
	return i < 24 && i != 12 && i != 13 && i != 22 && i != 23;
}

static u32 fbpa_read(unsigned int i, u32 reg)
{
	if (!present(i))
		return 0xbadf2016;
	switch (reg) {
	case FBPA_CFG1: return 0x02779000;
	case REFRESH_PLM:
	case PLL_PLM: return ~0u;
	case REFRESH: return card.refresh[i];
	case PLL_COEFF: return card.coeff[i];
	case PLL_CFG:
		if (card.lock_wait) {
			card.lock_wait--;
			return card.cfg & ~PLL_LOCK;
		}
		return card.cfg;
	}
	return 0;
}

static u32 readl(const void *addr)
{
	u32 reg = (u32)((const char *)addr - bar);

	card.ns += 3000;
	if (reg >= FBPA_BASE && reg < FBPA_BASE + 24 * FBPA_STRIDE)
		return fbpa_read((reg - FBPA_BASE) / FBPA_STRIDE, reg % FBPA_STRIDE);
	switch (reg) {
	case GU_GFW_BOOT_OFFSET: return 0x3ff;
	case GU_GSP_RISCV_STATUS_OFFSET: return 0x33;
	case FB_STOP_OUTPUT: return 0x3ffffc00 | card.request;
	case FB_STOP_INPUT:
		if (card.ack_wait && !--card.ack_wait)
			card.ack = card.request;
		return 0x00501400 | card.ack;
	/* Counter 0 covers everything the governor requires. */
	case 0x10be40: return 0x00380081;
	case 0x10be80: return 0x00700000;
	case 0x10bec0: return 0x00000007;
	case 0x10bfc0: return 2;
	case 0x10bf80: return card.count;
	}
	return 0;
}

static void writel(u32 value, void *addr)
{
	u32 reg = (u32)((char *)addr - bar);
	unsigned int i;

	card.ns += 3000;
	if (reg >= FBPA_BASE && reg < FBPA_BASE + 24 * FBPA_STRIDE) {
		i = (reg - FBPA_BASE) / FBPA_STRIDE;
		if (present(i) && reg % FBPA_STRIDE == REFRESH) {
			/* Both fields change only while the framebuffer is stopped. */
			assert(card.ack);
			card.refresh[i] = value;
		}
		return;
	}
	switch (reg) {
	case FB_STOP_SET:
		/* Only with the other CPUs held: they cannot be served until the release. */
		assert(card.cpus_held);
		assert(value == FB_STOP && !card.request);
		card.request = FB_STOP;
		card.ack_wait = card.ack_reads + 1;
		card.stopped_at = card.ns;
		card.stops++;
		break;
	case FB_STOP_CLEAR:
		assert(value == FB_STOP && card.request);
		card.request = 0;
		card.ack_wait = 1;
		if (card.ns - card.stopped_at > card.longest_stop)
			card.longest_stop = card.ns - card.stopped_at;
		break;
	case PLL_COEFF_MC2:
		assert(card.ack);
		for (i = 0; i < 24; i++)
			card.coeff[i] = value;
		card.lock_wait = card.lock_reads;
		card.clock_writes++;
		break;
	case PLL_CFG_MC2:
		assert(card.ack);
		card.cfg = value;
		break;
	}
}

static int stop_machine(int (*fn)(void *), void *data, const struct cpumask *cpus)
{
	int ret;

	assert(!cpus && !card.cpus_held);
	card.cpus_held = true;
	ret = fn(data);
	card.cpus_held = false;
	/* The other CPUs resume once the framebuffer serves them again. Only a
	 * failed switch may leave it stopped, on a clock that cannot be trusted. */
	assert(ret || (!card.request && !card.ack));
	return ret;
}

static void udelay(unsigned long us) { card.ns += us * 1000ull; }
static void ndelay(unsigned long ns) { card.ns += ns; }

static void boot(struct gu_hbm *hbm)
{
	unsigned int i;

	memset(&card, 0, sizeof(card));
	for (i = 0; i < 24; i++) {
		card.refresh[i] = BUSY_REFRESH;
		card.coeff[i] = BUSY_COEFF;
	}
	card.cfg = 0x8000192d;
	gu_hbm_init(hbm);
}

/* One governor poll every 20 ms; the card is busy when a counter advanced. */
static int poll(struct gu_hbm *hbm, bool busy)
{
	u64 start;
	int ret;

	card.ns += GU_HBM_POLL_MS * MS;
	card.count += busy ? 1000 : 0;
	start = card.ns;
	ret = gu_hbm_poll(hbm, bar, start);
	if (card.ns - start > card.longest_poll)
		card.longest_poll = card.ns - start;
	return ret;
}

static void idle(struct gu_hbm *hbm)
{
	unsigned int i;

	assert(!poll(hbm, false) && hbm->baseline && hbm->fbpas == 0x3fcfff);
	for (i = 0; i < hbm->policy.idle_ms / GU_HBM_POLL_MS + 1 && !hbm->idle; i++)
		assert(!poll(hbm, false));
	assert(hbm->idle && !card.request && !card.ack);
	for (i = 0; i < 24; i++)
		assert(!present(i) || card.coeff[i] == IDLE_COEFF);
}

/* The card runs the profile it booted with and traffic is not held. */
static void check_card_busy(void)
{
	unsigned int i;

	assert(!card.request && !card.ack);
	for (i = 0; i < 24; i++)
		assert(!present(i) || (card.coeff[i] == BUSY_COEFF && card.refresh[i] == BUSY_REFRESH));
}

static void check_busy(const struct gu_hbm *hbm)
{
	assert(!hbm->idle && !hbm->fb_stopped);
	check_card_busy();
}

int main(void)
{
	struct gu_hbm hbm;
	unsigned int i;

	/* A quiet card idles after idle_ms and returns to the busy profile on activity. */
	boot(&hbm);
	assert(!poll(&hbm, false) && !hbm.idle);
	for (i = 0; i < hbm.policy.idle_ms / GU_HBM_POLL_MS - 1; i++)
		assert(!poll(&hbm, false) && !hbm.idle);
	assert(!poll(&hbm, false) && hbm.idle && card.stops == 1);
	assert((card.refresh[0] & REFRESH_MASK) >> 3 == hbm.policy.refresh);
	assert(!poll(&hbm, false) && card.stops == 1);
	assert(!poll(&hbm, true) && card.stops == 2);
	check_busy(&hbm);
	/* Either switch holds the other CPUs for about two milliseconds. */
	assert(card.longest_stop < 3 * MS && card.longest_poll < 3 * MS);

	/* Activity restarts the quiet period. */
	boot(&hbm);
	assert(!poll(&hbm, false));
	for (i = 0; i < 50; i++)
		assert(!poll(&hbm, i % 4 == 0) && !hbm.idle);
	assert(!card.stops);

	/* A late ACK or lock is waited for with the framebuffer stopped. */
	boot(&hbm);
	card.ack_reads = 200;
	card.lock_reads = 300;
	idle(&hbm);
	assert(!poll(&hbm, true));
	check_busy(&hbm);

	/* Without an ACK nothing is written and the request is withdrawn. */
	boot(&hbm);
	card.ack_reads = 100000;
	assert(!poll(&hbm, false));
	for (i = 0; i < hbm.policy.idle_ms / GU_HBM_POLL_MS + 5; i++)
		poll(&hbm, false);
	assert(card.stops && !card.clock_writes);
	check_card_busy();

	/* A PLL that does not lock is given up on within milliseconds, because
	 * every CPU waits with the governor. Traffic stays stopped. */
	boot(&hbm);
	assert(!poll(&hbm, false));
	card.lock_reads = ~0u;
	for (i = 0; i < hbm.policy.idle_ms / GU_HBM_POLL_MS && !card.clock_writes; i++)
		poll(&hbm, false);
	assert(card.clock_writes && card.request && card.longest_poll < 20 * MS);

	/* The detach and sleep paths restore the busy profile. */
	boot(&hbm);
	idle(&hbm);
	assert(!gu_hbm_restore(&hbm, bar));
	check_busy(&hbm);

	puts("hbm governor: ok");
	return 0;
}
