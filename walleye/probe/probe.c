/*
 * walleye probe: one boot answers the questions that decide the port.
 *   - per-core ID registers (16K granule, LSE, PA range) via PSCI CPU_ON
 *   - PSCI/SMCCC versions
 *   - GICv3 Group 1 IRQ from the virtual timer
 *   - MDSS: which pipe scans out the splash buffer
 * The command-mode panel only shows a new frame after a CTL_START kick, so
 * every line is drawn into the splash buffer and then kicked. (Probe v1
 * logged to ramoops instead: ABL clears it on a PSCI reset.)
 */
#include <stdint.h>

#define PSHOLD			0x010ac000UL

#define GICD_BASE		0x17a00000UL
#define GICR_BASE		0x17b00000UL
#define GICR_SIZE		0x100000UL
#define GICR_PE_SIZE		0x20000UL
#define GICR_TYPER		0x08
#define GICR_WAKER		0x14
#define GICR_SGI		0x10000
#define GICR_IGROUPR0		(GICR_SGI + 0x080)
#define GICR_ISENABLER0		(GICR_SGI + 0x100)
#define GICR_ICENABLER0		(GICR_SGI + 0x180)
#define GICR_IPRIORITYR		(GICR_SGI + 0x400)
#define VTIMER_INTID		19	/* Qualcomm: <GIC_PPI 3 ...> */

#define MDSS_BASE		0x0c900000UL
#define SPLASH_LO		0x9d400000UL
#define SPLASH_HI		0x9f800000UL

#define PSCI_VERSION		0x84000000UL
#define PSCI_CPU_OFF		0x84000002UL
#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define PSCI_FEATURES		0x8400000aUL
#define SMCCC_VERSION		0x80000000UL

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))
#define isb() __asm__ volatile("isb" ::: "memory")
#define dsb() __asm__ volatile("dsb sy" ::: "memory")

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline uint64_t rd64(uintptr_t a) { return *(volatile uint64_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }

extern char secondary_entry[];

/* ---- screen console: text into the splash buffer, then a CTL_START kick ---- */

#include "../../shim/font.h"

#define SCALE		2
#define CH_W		(8 * SCALE)
#define CH_H		(16 * SCALE)
#define MARGIN		16

static const uint32_t ctl_off[] = { 0x2000, 0x2200, 0x2400, 0x2600, 0x2800 };
static const uint32_t pipe_off[] = { 0x5000, 0x7000, 0x9000, 0xb000, 0x25000, 0x27000, 0x29000, 0x2b000 };

static const uint32_t pp_off[] = { 0x71000, 0x71800, 0x72000, 0x72800 };
#define PP_AUTOREFRESH_CONFIG	0x30	/* bit 31: enable, low bits: frame interval */

static uintptr_t fb;
static int autorefresh;
static uint32_t fb_w, fb_h, fb_stride, fb_pipe;
static int row;
static char line[128];
static int line_len;

/* ABL leaves the panel in autorefresh; a manual START on top of it stalls the pipe. */
static void kick(void)
{
	dsb();
	if (autorefresh)
		return;
	for (unsigned i = 0; i < sizeof(ctl_off) / sizeof(ctl_off[0]); i++)
		if (rd32(MDSS_BASE + ctl_off[i] + 0x14))	/* CTL_TOP: in use */
			wr32(MDSS_BASE + ctl_off[i] + 0x1c, 1);	/* CTL_START */
}

static void fb_clear(uint32_t c)
{
	for (uint32_t y = 0; y < fb_h; y++) {
		volatile uint32_t *p = (volatile uint32_t *)(fb + (uintptr_t)y * fb_stride);
		for (uint32_t x = 0; x < fb_w; x++)
			p[x] = c;
	}
	row = 0;
}

/* The pipe that scans out of the splash carve-out is the panel's. */
static void fb_find(void)
{
	for (unsigned i = 0; i < sizeof(pp_off) / sizeof(pp_off[0]); i++)
		if (rd32(MDSS_BASE + pp_off[i] + PP_AUTOREFRESH_CONFIG) & (1U << 31))
			autorefresh = 1;
	for (unsigned i = 0; i < sizeof(pipe_off) / sizeof(pipe_off[0]); i++) {
		uintptr_t p = MDSS_BASE + pipe_off[i];
		uint32_t addr = rd32(p + 0x14);	/* SRC0_ADDR */
		if (addr >= SPLASH_LO && addr < SPLASH_HI) {
			fb = addr;
			fb_w = rd32(p + 0x04) & 0xffff;	/* SRC_IMG_SIZE */
			fb_h = rd32(p + 0x04) >> 16;
			fb_stride = rd32(p + 0x24) & 0xffff;	/* SRC_YSTRIDE0 */
			fb_pipe = pipe_off[i];
			break;
		}
	}
}

static void put_char(int col, char ch, uint32_t fg)
{
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	uint32_t x0 = MARGIN + col * CH_W, y0 = MARGIN + row * CH_H;
	if (x0 + CH_W > fb_w || y0 + CH_H > fb_h)
		return;
	for (int gy = 0; gy < 16 * SCALE; gy++) {
		volatile uint32_t *p = (volatile uint32_t *)(fb + (uintptr_t)(y0 + gy) * fb_stride) + x0;
		for (int gx = 0; gx < 8 * SCALE; gx++)
			p[gx] = (g[gy / SCALE] & (0x80 >> (gx / SCALE))) ? fg : 0xff000000;
	}
}

static void log_putc(char c)
{
	if (line_len < (int)sizeof(line) - 1)
		line[line_len++] = c;
}

static void log_s(const char *s)
{
	while (*s)
		log_putc(*s++);
}

static void log_x(uint64_t v)
{
	for (int i = 60; i >= 0; i -= 4)
		if (v >> i || i == 0)
			log_putc("0123456789abcdef"[(v >> i) & 0xf]);
}

static void log_flush(void)
{
	if (fb) {
		for (int i = 0; i < line_len; i++)
			put_char(i, line[i], (i < 6) ? 0xffffff00 : 0xffffffff);
		row++;
		kick();
	}
	line_len = 0;
}

/* "what k1=v1 k2=v2" in hex, one screen line. */
static void logl(const char *what, const char *k1, uint64_t v1, const char *k2, uint64_t v2)
{
	log_s(what);
	if (k1) {
		log_putc(' ');
		log_s(k1);
		log_putc('=');
		log_x(v1);
	}
	if (k2) {
		log_putc(' ');
		log_s(k2);
		log_putc('=');
		log_x(v2);
	}
	log_flush();
}

/* ---- basics ---- */

static uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static uint64_t now(void)
{
	isb();
	return rsr("cntpct_el0");
}

static void delay_ms(unsigned ms)
{
	uint64_t end = now() + rsr("cntfrq_el0") / 1000 * ms;
	while (now() < end)
		;
}

static void __attribute__((noreturn)) reset(void)
{
	logl("reset", 0, 0, 0, 0);
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	wr32(PSHOLD, 0);
	for (;;)
		__asm__ volatile("wfe");
}

static volatile uint32_t irq_count, irq_other;

void exc_handler(uint64_t slot, uint64_t *regs)
{
	if (slot == 5) {	/* IRQ, current EL, SPx */
		uint64_t iar = rsr("icc_iar1_el1");
		uint32_t id = iar & 0xffffff;
		if (id >= 1020)
			return;
		if (id == VTIMER_INTID) {
			irq_count++;
			wsr("cntv_tval_el0", rsr("cntfrq_el0") / 100);
		} else {
			irq_other++;
		}
		wsr("icc_eoir1_el1", iar);
		isb();
		return;
	}
	logl("PANIC", "slot", slot, "mpidr", rsr("mpidr_el1"));
	logl("PANIC", "esr", rsr("esr_el1"), "far", rsr("far_el1"));
	logl("PANIC", "elr", rsr("elr_el1"), "x30", regs[29]);
	reset();
}

/* ---- CPUs ---- */

#define NR_CPUS 8
struct cpu_ids {
	uint64_t mpidr, midr, mmfr0, isar0, pfr0, done;
};
static volatile struct cpu_ids cpus[NR_CPUS];

static const uint64_t cpu_mpidr[NR_CPUS] = { 0x0, 0x1, 0x2, 0x3, 0x100, 0x101, 0x102, 0x103 };

static void read_ids(int idx)
{
	cpus[idx].mpidr = rsr("mpidr_el1");
	cpus[idx].midr = rsr("midr_el1");
	cpus[idx].mmfr0 = rsr("id_aa64mmfr0_el1");
	cpus[idx].isar0 = rsr("id_aa64isar0_el1");
	cpus[idx].pfr0 = rsr("id_aa64pfr0_el1");
	dsb();
	cpus[idx].done = 1;
	dsb();
}

void secondary_main(uint64_t idx)
{
	read_ids(idx);
	smc(PSCI_CPU_OFF, 0, 0, 0);
}

static void probe_cpus(void)
{
	uint64_t self = rsr("mpidr_el1") & 0xffffff;
	int boot = -1;

	for (int i = 0; i < NR_CPUS; i++)
		if (cpu_mpidr[i] == self)
			boot = i;
	logl("boot cpu", "mpidr", self, "idx", boot);
	if (boot >= 0)
		read_ids(boot);
	for (int i = 0; i < NR_CPUS; i++) {
		if (i == boot)
			continue;
		uint64_t r = smc(PSCI_CPU_ON, cpu_mpidr[i], (uintptr_t)secondary_entry, i);
			if (r)
			logl("cpu_on", "mpidr", cpu_mpidr[i], "ret", r);
		for (int t = 0; t < 200 && !cpus[i].done; t++)
			delay_ms(1);
	}
	for (int i = 0; i < NR_CPUS; i++) {
		const volatile struct cpu_ids *c = &cpus[i];
		if (!c->done) {
			logl("cpu", "idx", i, "done", 0);
			continue;
		}
		logl("cpu", "mpidr", c->mpidr, "midr", c->midr);
		logl(" ids", "mmfr0", c->mmfr0, "isar0", c->isar0);
		logl(" feat", "tgran16", (c->mmfr0 >> 20) & 0xf, "atomics", (c->isar0 >> 20) & 0xf);
	}
}

/* ---- GIC + virtual timer ---- */

static uintptr_t find_gicr(void)
{
	uint64_t aff = rsr("mpidr_el1");
	uint32_t want = (aff & 0xffffff) | ((aff >> 8) & 0xff000000);

	for (uintptr_t off = 0; off < GICR_SIZE; off += GICR_PE_SIZE) {
		uint64_t typer = rd64(GICR_BASE + off + GICR_TYPER);
		if ((uint32_t)(typer >> 32) == want)
			return GICR_BASE + off;
		if (typer & (1 << 4))	/* Last */
			break;
	}
	return 0;
}

static void probe_gic(void)
{
	uint64_t sre = rsr("icc_sre_el1");
	if (!(sre & 1)) {
		wsr("icc_sre_el1", sre | 1);
		isb();
	}
	logl("gic", "sre", rsr("icc_sre_el1"), "typer", rd32(GICD_BASE + 4));
	if (!(rsr("icc_sre_el1") & 1))
		return;

	uint32_t dctlr = rd32(GICD_BASE);
	wr32(GICD_BASE, dctlr | 0x13);
	for (int i = 0; i < 1000000 && (rd32(GICD_BASE) & (1U << 31)); i++)
		;
	logl("gicd", "ctlr_was", dctlr, "ctlr_now", rd32(GICD_BASE));

	uintptr_t r = find_gicr();
	logl("gicr", "base", r, 0, 0);
	if (!r)
		return;
	uint32_t waker = rd32(r + GICR_WAKER);
	if (waker & 2) {
		wr32(r + GICR_WAKER, waker & ~2U);
		for (int i = 0; i < 100000 && (rd32(r + GICR_WAKER) & 4); i++)
			;
	}
	wr32(r + GICR_IGROUPR0, 0xffffffff);
	for (int id = 0; id < 32; id++)
		wr8(r + GICR_IPRIORITYR + id, 0xa0);
	wr32(r + GICR_ISENABLER0, 1U << VTIMER_INTID);
	wsr("icc_ctlr_el1", rsr("icc_ctlr_el1") & ~2ULL);
	wsr("icc_pmr_el1", 0xf0);
	wsr("icc_bpr1_el1", 0);
	wsr("icc_igrpen1_el1", 1);
	isb();

	uint64_t f = rsr("cntfrq_el0");
	irq_count = irq_other = 0;
	wsr("cntv_tval_el0", f / 100);
	wsr("cntv_ctl_el0", 1);
	isb();
	__asm__ volatile("msr daifclr, #2" ::: "memory");
	delay_ms(300);
	__asm__ volatile("msr daifset, #2" ::: "memory");
	wsr("cntv_ctl_el0", 0);
	wr32(r + GICR_ICENABLER0, 0xffffffff);
	isb();
	logl("vtimer 300ms", "irqs", irq_count, "other", irq_other);
}

static void report_mdss(void)
{
	logl("mdss", "hw_rev", rd32(MDSS_BASE), "pipe", fb_pipe);
	logl("fb", "addr", fb, "stride", fb_stride);
	logl("fb", "w", fb_w, "h", fb_h);
	logl("fb", "format", rd32(MDSS_BASE + fb_pipe + 0x30), "op", rd32(MDSS_BASE + fb_pipe + 0x38));
	for (unsigned i = 0; i < sizeof(pp_off) / sizeof(pp_off[0]); i++)
		logl("pp", "off", pp_off[i], "autorefresh", rd32(MDSS_BASE + pp_off[i] + PP_AUTOREFRESH_CONFIG));
	for (unsigned i = 0; i < sizeof(ctl_off) / sizeof(ctl_off[0]); i++) {
		uintptr_t c = MDSS_BASE + ctl_off[i];
		if (rd32(c + 0x14))
			logl("ctl", "off", ctl_off[i], "top", rd32(c + 0x14));
	}
}

void shim_main(uint64_t dtb)
{
	fb_find();
	if (fb)
		fb_clear(0xff000000);
	logl("probe v3", "dtb", dtb, "el", rsr("currentel") >> 2);
	logl("psci", "ver", smc(PSCI_VERSION, 0, 0, 0), "smccc", smc(SMCCC_VERSION, 0, 0, 0));
	logl("psci", "cpu_on_feat", smc(PSCI_FEATURES, PSCI_CPU_ON, 0, 0), "cntfrq", rsr("cntfrq_el0"));
	logl("sctlr", "val", rsr("sctlr_el1"), "image", (uintptr_t)shim_main);
	probe_cpus();
	probe_gic();
	report_mdss();
	logl("done: hold power to reboot", 0, 0, 0, 0);
	for (;;)
		__asm__ volatile("wfe");
}
