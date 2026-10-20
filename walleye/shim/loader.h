#pragma once
#include <stdint.h>

#define PSCI_CPU_OFF		0x84000002UL
#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define XNU_BOOT_MPIDR		0x100	/* first big core: the only cores with the 16K granule */

uint64_t psci_smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3);

/* Boot the SWEETXNU payload linked into the Image; returns only on error. */
const char *loader_boot(uintptr_t dtb);
