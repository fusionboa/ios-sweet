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

static void probe_gic(void);
static volatile int big_gic_done;

/* idx 4 = MPIDR 0x100, the core xnu boots on: run the GIC/vtimer test there too. */
void secondary_main(uint64_t idx)
{
	read_ids(idx);
	if (idx == 4) {
		logl("gic test on big core", "mpidr", rsr("mpidr_el1"), "sre", rsr("icc_sre_el1"));
		probe_gic();
		dsb();
		big_gic_done = 1;
		dsb();
	}
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
		if (i == 4)
			for (int t = 0; t < 3000 && !big_gic_done; t++)
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

/* ---- touch: Synaptics RMI4 at 0x20 on BLSP1 QUP5 (I2C, GPIO 87/88) ---- */

#define GCC_BASE		0x00100000UL
#define GCC_APCS_VOTE		(GCC_BASE + 0x52004)	/* bit 17: blsp1_ahb */
#define GCC_QUP5_I2C_CBCR	(GCC_BASE + 0x21008)	/* bit 0 enable, bit 31 off */
#define TLMM_EAST		(0x03400000UL + 0x900000)
#define TLMM_WEST		(0x03400000UL + 0x100000)
#define GPIO_CFG(t, n)		((t) + 0x1000UL * (n))
#define GPIO_IO(t, n)		((t) + 0x1000UL * (n) + 4)
#define QUP5			0x0c179000UL

#define QUP_CONFIG		0x000
#define QUP_STATE		0x004
#define QUP_IO_MODE		0x008
#define QUP_SW_RESET		0x00c
#define QUP_OPERATIONAL		0x018
#define QUP_ERROR_FLAGS		0x01c
#define QUP_HW_VERSION		0x030
#define QUP_MX_OUTPUT_CNT	0x100
#define QUP_OUT_FIFO		0x110
#define QUP_MX_WRITE_CNT	0x150
#define QUP_MX_INPUT_CNT	0x200
#define QUP_MX_READ_CNT		0x208
#define QUP_IN_FIFO		0x218
#define QUP_I2C_CLK_CTL		0x400
#define QUP_I2C_STATUS		0x404
#define TAG_START		(1 << 8)
#define TAG_DATA		(2 << 8)
#define TAG_STOP		(3 << 8)
#define TAG_REC			(4 << 8)
#define OP_OUT_NOT_EMPTY	(1u << 4)
#define OP_IN_NOT_EMPTY		(1u << 5)
#define I2C_BUS_ACTIVE		(1u << 8)
#define I2C_ERRORS		0x38000fcu
static uint8_t TS_ADDR = 0x20;

static int qup_state(uint32_t st)
{
	for (int i = 0; i < 1000; i++) {
		uint32_t v = rd32(QUP5 + QUP_STATE);
		if ((v & 4) && (v & 3) == st)
			return 0;
		if (i == 0)
			wr32(QUP5 + QUP_STATE, st);
		delay_ms(0);
	}
	return -1;
}

static uint32_t i2c_err;

/* Wait for the bus to go idle with the output FIFO drained; 0 or I2C_STATUS. */
static uint32_t qup_wait_idle(void)
{
	uint64_t end = now() + rsr("cntfrq_el0") / 50;	/* 20 ms */
	for (;;) {
		uint32_t st = rd32(QUP5 + QUP_I2C_STATUS), op = rd32(QUP5 + QUP_OPERATIONAL);
		if (st & I2C_ERRORS)
			return st | 0x80000000u;
		if (!(op & OP_OUT_NOT_EMPTY) && !(st & I2C_BUS_ACTIVE))
			return 0;
		if (now() > end)
			return st | 0x40000000u;
	}
}

static int qup_begin(uint32_t wr_cnt, uint32_t rd_cnt)
{
	if (qup_state(0))
		return -1;
	wr32(QUP5 + QUP_I2C_STATUS, 0xfffffc);	/* clear status */
	wr32(QUP5 + QUP_OPERATIONAL, 0xff0);
	wr32(QUP5 + QUP_CONFIG, (2 << 8) | 15 | (rd_cnt ? 0 : (1 << 7)));
	wr32(QUP5 + QUP_IO_MODE, (1u << 14) | (1u << 15));	/* repack, FIFO mode */
	wr32(QUP5 + QUP_MX_OUTPUT_CNT, 0);
	wr32(QUP5 + QUP_MX_WRITE_CNT, wr_cnt);
	wr32(QUP5 + QUP_MX_INPUT_CNT, 0);
	wr32(QUP5 + QUP_MX_READ_CNT, rd_cnt);
	return qup_state(1);
}

static int i2c_write(const uint8_t *buf, int len)
{
	if (qup_begin(len + 1, 0))
		return -2;
	uint32_t val = TAG_START | (TS_ADDR << 1);
	for (int pos = 0, idx = 1; pos < len; pos++, idx++) {
		uint32_t t = (pos == len - 1 ? TAG_STOP : TAG_DATA) | buf[pos];
		if (idx & 1)
			val |= t << 16;
		else
			val = t;
		if ((idx & 1) || pos == len - 1)
			wr32(QUP5 + QUP_OUT_FIFO, val);
	}
	i2c_err = qup_wait_idle();
	qup_state(0);
	return i2c_err ? -3 : 0;
}

static int i2c_read(uint8_t *buf, int len)
{
	if (qup_begin(2, len))
		return -2;
	wr32(QUP5 + QUP_OUT_FIFO, ((TAG_REC | len) << 16) | TAG_START | (TS_ADDR << 1) | 1);
	uint64_t end = now() + rsr("cntfrq_el0") / 50;
	int pos = 0;
	while (pos < len) {
		uint32_t st = rd32(QUP5 + QUP_I2C_STATUS);
		if (st & I2C_ERRORS) {
			i2c_err = st | 0x80000000u;
			qup_state(0);
			return -3;
		}
		if (rd32(QUP5 + QUP_OPERATIONAL) & OP_IN_NOT_EMPTY) {
			uint32_t w = rd32(QUP5 + QUP_IN_FIFO);
			buf[pos++] = w & 0xff;
			if (pos < len)
				buf[pos++] = (w >> 16) & 0xff;
		} else if (now() > end) {
			i2c_err = st | 0x40000000u;
			qup_state(0);
			return -4;
		}
	}
	i2c_err = 0;
	qup_state(0);
	return 0;
}

/* RMI4 register read: page select, address, then the data (separate transfers). */
static int rmi_read(uint16_t addr, uint8_t *buf, int len)
{
	uint8_t page[2] = { 0xff, (uint8_t)(addr >> 8) };
	uint8_t a = (uint8_t)addr;
	int r = i2c_write(page, 2);
	if (r)
		return r;
	r = i2c_write(&a, 1);
	if (r)
		return r - 10;
	return i2c_read(buf, len) - (r ? 0 : 0);
}

static void gpio_out(uintptr_t tile, unsigned n, int v)
{
	wr32(GPIO_IO(tile, n), v ? 2 : 0);
	wr32(GPIO_CFG(tile, n), (1u << 9) | (0 << 2));	/* OE, gpio function, 2 mA, no pull */
}

/* ---- SPMI (PMIC arbiter v3) for the touch power switch on PMI8998 GPIO 2 ---- */

#define SPMI_CORE		0x0800f000UL
#define SPMI_CHNLS		0x08400000UL
#define SPMI_OBSRVR		0x09400000UL
#define SPMI_CNFG		0x0800a000UL

static int spmi_apid(uint16_t ppid, uint32_t *owner)
{
	for (int apid = 0; apid < 512; apid++) {
		uint32_t v = rd32(SPMI_CORE + 0x800 + 4 * apid);
		if (v && ((v >> 8) & 0xfff) == ppid) {
			*owner = rd32(SPMI_CNFG + 0x700 + 4 * apid) & 7;
			return apid;
		}
	}
	return -1;
}

static int spmi_cmd(uintptr_t chan, uint32_t opc, uint16_t addr)
{
	wr32(chan + 0x00, (opc << 27) | ((addr & 0xff) << 4));	/* 1 byte */
	for (int i = 0; i < 100000; i++) {
		uint32_t st = rd32(chan + 0x08);
		if (st & 1)
			return (st & 0xe) ? -(int)st : 0;
	}
	return -100;
}

static int spmi_read(int apid, uint16_t addr, uint8_t *v)
{
	uintptr_t chan = SPMI_OBSRVR + 0x8000UL * apid;
	int r = spmi_cmd(chan, 1, addr);	/* EXT_READL */
	if (!r)
		*v = rd32(chan + 0x18) & 0xff;
	return r;
}

static int spmi_write(int apid, uint16_t addr, uint8_t v)
{
	uintptr_t chan = SPMI_CHNLS + 0x8000UL * apid;
	wr32(chan + 0x10, v);
	return spmi_cmd(chan, 0, addr);	/* EXT_WRITEL */
}

/* PMI8998 (SID 2) GPIO 2 at 0xc100 -> output high: the touch IC's power switch. */
static void touch_power_on(void)
{
	uint32_t owner = 0xff;
	int apid = spmi_apid(0x2c1, &owner);
	logl(" spmi", "arb_ver", rd32(SPMI_CORE), "apid", (uint32_t)apid);
	if (apid < 0)
		return;
	uint8_t sub = 0, mode = 0, en = 0, out = 0, src = 0, vin = 0;
	spmi_read(apid, 0x05, &sub);
	spmi_read(apid, 0x40, &mode);
	spmi_read(apid, 0x41, &vin);
	spmi_read(apid, 0x44, &src);
	spmi_read(apid, 0x45, &out);
	int r = spmi_read(apid, 0x46, &en);
	logl(" pmigpio2", "owner", owner, "read_ret", (uint32_t)r);
	logl(" pmigpio2", "sub/mode/vin", sub << 16 | mode << 8 | vin, "src/out/en", src << 16 | out << 8 | en);
	if (sub >= 0x10) {	/* LV/MV: direction in MODE_CTL, level via output invert */
		spmi_write(apid, 0x40, 1);
		spmi_write(apid, 0x44, 0x80);
	} else {
		spmi_write(apid, 0x40, (1 << 4) | 1);
	}
	if (!(out & 3))
		spmi_write(apid, 0x45, (out & ~3) | 1);	/* drive strength low */
	r = spmi_write(apid, 0x46, 0x80);
	spmi_read(apid, 0x40, &mode);
	spmi_read(apid, 0x44, &src);
	spmi_read(apid, 0x46, &en);
	logl(" pmigpio2 on", "write_ret", (uint32_t)r, "mode/src/en", mode << 16 | src << 8 | en);
}

/* PM8998 (SID 1) LDOn at 0x4000 + (n-1) * 0x100. walleye xd/xe touch: L28 3.3 V. */
static void ldo_report(unsigned n)
{
	uint32_t owner = 0xff;
	int apid = spmi_apid(0x100 | (0x40 + n - 1), &owner);
	if (apid < 0) {
		logl(" ldo", "n", n, "no apid", 0);
		return;
	}
	uint8_t st = 0, en = 0, v0 = 0, v1 = 0;
	spmi_read(apid, 0x08, &st);
	spmi_read(apid, 0x46, &en);
	spmi_read(apid, 0x40, &v0);
	spmi_read(apid, 0x41, &v1);
	logl(" ldo", "n/owner", n << 8 | owner, "st/en/mv", st << 24 | en << 16 | v1 << 8 | v0);
}

static void __attribute__((unused)) touch_vdd_on(void)
{
	uint32_t owner = 0xff;
	int apid = spmi_apid(0x145, &owner);
	if (apid < 0) {
		logl(" ldo6", "no apid", 0, 0, 0);
		return;
	}
	uint8_t st = 0, en = 0, v0 = 0, v1 = 0;
	spmi_read(apid, 0x08, &st);
	spmi_read(apid, 0x46, &en);
	spmi_read(apid, 0x40, &v0);
	spmi_read(apid, 0x41, &v1);
	logl(" ldo6", "owner/apid", owner << 16 | (uint32_t)apid, "st/en/vset", st << 24 | en << 16 | v1 << 8 | v0);
	if (!(en & 0x80) && owner == 0) {
		int r = spmi_write(apid, 0x46, en | 0x80);
		delay_ms(5);
		spmi_read(apid, 0x08, &st);
		spmi_read(apid, 0x46, &en);
		logl(" ldo6 on", "write_ret", (uint32_t)r, "st/en", st << 8 | en);
	}
}

/* Which 7-bit addresses ACK a 1-byte read? */
static void i2c_scan(void)
{
	uint32_t found[4] = { 0 };
	uint8_t b;
	uint8_t save = TS_ADDR;
	for (int a = 0x08; a < 0x78; a++) {
		TS_ADDR = (uint8_t)a;
		if (!i2c_read(&b, 1))
			found[a >> 5] |= 1u << (a & 31);
	}
	TS_ADDR = save;
	logl(" i2c scan", "08-3f", (uint64_t)found[1] << 32 | found[0], "40-7f", (uint64_t)found[3] << 32 | found[2]);
}

static void probe_touch(void)
{
	logl("touch", "gcc_vote", rd32(GCC_APCS_VOTE), "qup5_cbcr", rd32(GCC_QUP5_I2C_CBCR));
	wr32(GCC_APCS_VOTE, rd32(GCC_APCS_VOTE) | (1u << 17));
	wr32(GCC_QUP5_I2C_CBCR, rd32(GCC_QUP5_I2C_CBCR) | 1);
	for (int i = 0; i < 1000 && (rd32(GCC_QUP5_I2C_CBCR) & (1u << 31)); i++)
		;
	logl(" clk", "qup5_cbcr", rd32(GCC_QUP5_I2C_CBCR), 0, 0);
	if (rd32(GCC_QUP5_I2C_CBCR) & (1u << 31))
		return;

	logl(" gpio", "87cfg", rd32(GPIO_CFG(TLMM_EAST, 87)), "88cfg", rd32(GPIO_CFG(TLMM_EAST, 88)));
	logl(" gpio", "89cfg", rd32(GPIO_CFG(TLMM_EAST, 89)), "89io", rd32(GPIO_IO(TLMM_EAST, 89)));
	logl(" gpio", "75cfg", rd32(GPIO_CFG(TLMM_WEST, 75)), "75io", rd32(GPIO_IO(TLMM_WEST, 75)));
	logl(" gpio", "125cfg", rd32(GPIO_CFG(TLMM_EAST, 125)), "125io", rd32(GPIO_IO(TLMM_EAST, 125)));
	wr32(GPIO_CFG(TLMM_EAST, 87), 3 << 2);	/* blsp_i2c5, 2 mA, bias disable */
	wr32(GPIO_CFG(TLMM_EAST, 88), 3 << 2);

	logl(" qup", "hw_ver", rd32(QUP5 + QUP_HW_VERSION), "state", rd32(QUP5 + QUP_STATE));
	wr32(QUP5 + QUP_SW_RESET, 1);
	delay_ms(1);
	if (qup_state(0)) {
		logl(" qup", "reset state stuck", rd32(QUP5 + QUP_STATE), 0, 0);
		return;
	}
	/* 19.2 MHz source: fs_div = (19.2M / 100k) / 2 - 3, hs_div 3 */
	wr32(QUP5 + QUP_I2C_CLK_CTL, (3u << 8) | 93);

	/* Power-up as in the Pixel 2's synaptics_dsx_htc driver: switch GPIO 75
	 * low (bus to the AP), power GPIO high, reset pulse on GPIO 89, 20 ms. */
	gpio_out(TLMM_WEST, 75, 0);
	gpio_out(TLMM_EAST, 89, 0);
	ldo_report(28);
	ldo_report(6);
	touch_power_on();
	delay_ms(10);
	gpio_out(TLMM_EAST, 89, 1);
	delay_ms(100);
	uint8_t pdt[6] = { 0 };
	int r = rmi_read(0x00e9, pdt, 6);
	logl(" rmi pdt", "ret", (uint32_t)r, "i2c_status", i2c_err);
	if (r) {
		delay_ms(300);
		r = rmi_read(0x00e9, pdt, 6);
		logl(" +300ms", "ret", (uint32_t)r, "i2c_status", i2c_err);
	}
	logl(" attn", "125io", rd32(GPIO_IO(TLMM_EAST, 125)), "89io", rd32(GPIO_IO(TLMM_EAST, 89)));
	i2c_scan();
	if (r) {	/* maybe the bus switch is the other way round */
		gpio_out(TLMM_WEST, 75, 1);
		delay_ms(10);
		r = rmi_read(0x00e9, pdt, 6);
		logl(" sw75=1", "ret", (uint32_t)r, "i2c_status", i2c_err);
		i2c_scan();
	}
	if (!r) {
		logl(" pdt e9", "lo", pdt[0] | pdt[1] << 8 | pdt[2] << 16 | (uint32_t)pdt[3] << 24,
		    "hi", pdt[4] | pdt[5] << 8);
		/* Walk the page-0 PDT: 6-byte entries downwards from 0xe9 */
		for (uint16_t a = 0xe9; a >= 0x0a; a -= 6) {
			if (rmi_read(a, pdt, 6) || pdt[5] == 0)
				break;
			logl(" fn", "id", pdt[5], "q/c/c/d", pdt[0] | pdt[1] << 8 | pdt[2] << 16 | (uint32_t)pdt[3] << 24);
		}
	}
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
	logl("probe v8", "dtb", dtb, "el", rsr("currentel") >> 2);
	logl("psci", "ver", smc(PSCI_VERSION, 0, 0, 0), "smccc", smc(SMCCC_VERSION, 0, 0, 0));
	logl("psci", "cpu_on_feat", smc(PSCI_FEATURES, PSCI_CPU_ON, 0, 0), "cntfrq", rsr("cntfrq_el0"));
	logl("sctlr", "val", rsr("sctlr_el1"), "image", (uintptr_t)shim_main);
	/* v5: CPU and GIC results are known (v3/v4); keep the screen for touch. */
	report_mdss();
	probe_touch();
	logl("done: hold power to reboot", 0, 0, 0, 0);
	for (;;)
		__asm__ volatile("wfe");
}
