/*
 * walleye boot shim: act as iBoot for xnu on the Pixel 2.
 *
 * ABL enters on little cpu0 (EL1, MMU off, x0 = DTB). loader.c copies the
 * SWEETXNU payload out of the ramdisk and starts xnu on big core 0x100.
 * Anything that goes wrong stays on screen: ABL leaves the command-mode
 * panel in pingpong autorefresh, so plain framebuffer writes show up.
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"
#include "loader.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

uint64_t psci_smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
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

static void __attribute__((noreturn)) halt(void)
{
	con_skip();
	con_str("HALT", "hold power to reboot");
	for (;;)
		__asm__ volatile("wfe");
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	con_clear(BLUE);
	con_hex2("PANIC", "slot=", slot, "cpu=", rsr("mpidr_el1"));
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("X30", regs[29]);	/* stashed lr is just above the frame */
	halt();
}

/* start.S jumps here for CPU_ON targets; this shim starts none of its own. */
void secondary_main(uint64_t idx)
{
	(void)idx;
	psci_smc(PSCI_CPU_OFF, 0, 0, 0);
}

void shim_main(uint64_t dtb)
{
	con_clear(RED);
	con_str("SHIM", "walleye xnu loader");
	con_hex2("CPU", "mpidr=", rsr("mpidr_el1"), "dtb=", dtb);
	con_skip();
	const char *why = loader_boot(dtb);
	con_str("LOADER", why);
	halt();
}
