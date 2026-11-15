/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Intel CSME 11 application timer as the system timer.
 *
 * A free-running 64-bit counter and one comparator that fires when the
 * counter reaches the compare value; a value already passed is missed, so
 * this is the generic core's COMPARE_EXACT backend. The interrupt is an MSI
 * to the local APIC, programmed in the function's PCI configuration space.
 */

#define DT_DRV_COMPAT intel_csme_app_timer

#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/drivers/timer/system_timer.h>
#include <zephyr/sys/clock.h>
#include <zephyr/linker/sections.h>

#define REGS DT_INST_REG_ADDR_BY_NAME(0, regs)
#define CTL  DT_INST_REG_ADDR_BY_NAME(0, ctl)
#define CFG  DT_INST_REG_ADDR_BY_NAME(0, cfg)

#define TMR_CONTROL    (REGS + 0x00)
#define TMR_COUNTER_LO (REGS + 0x08)
#define TMR_COUNTER_HI (REGS + 0x0c)
#define TMR_COMPARE_LO (REGS + 0x10)
#define TMR_COMPARE_HI (REGS + 0x14)
#define TMR_INT_STATUS (REGS + 0x18)
#define TMR_ARM        (REGS + 0x1c)

#define TMR_ENABLE (CTL + 0x00)
#define TMR_MASK   (CTL + 0x04)

#define CFG_MSI_CTRL (CFG + 0x8c)
#define CFG_MSI_ADDR (CFG + 0x90)
#define CFG_MSI_DATA (CFG + 0x98)
#define MSI_ENABLE   BIT(16)

#define LOAPIC_MSI_ADDR 0xfee00000U

#if defined(CONFIG_TEST)
const int32_t z_sys_timer_irq_for_test = DT_INST_IRQN(0);
#endif

BUILD_ASSERT(DT_INST_PROP(0, clock_frequency) == CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC,
	     "CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC must match the app timer's clock-frequency");

static inline uint64_t app_timer_counter(void)
{
	uint32_t hi, lo;

	do {
		hi = sys_read32(TMR_COUNTER_HI);
		lo = sys_read32(TMR_COUNTER_LO);
	} while (hi != sys_read32(TMR_COUNTER_HI));

	return ((uint64_t)hi << 32) | lo;
}

static inline void app_timer_set_compare(uint64_t cycles)
{
	/* High word first; the low word completes the new value */
	sys_write32((uint32_t)(cycles >> 32), TMR_COMPARE_HI);
	sys_write32((uint32_t)cycles, TMR_COMPARE_LO);
}

/* The comparator matches on reaching the value: a value the counter
 * already passed is lost. A counter read is not free on this bus, so ask
 * for some lead (the CSME kernel only arms deadlines over 0x80 ticks out).
 */
#define TIMER_CORE_BACKEND_COMPARE_EXACT
#define TIMER_CORE_COUNTER_WIDTH 64
#define TIMER_CORE_ALARM_LEAD_CYCLES 0x100U

static inline uint64_t timer_driver_cycle_get(void)
{
	return app_timer_counter();
}

static inline void timer_driver_set_compare(uint64_t cycles)
{
	app_timer_set_compare(cycles);
}

#include "system_timer_generic.h"

__isr
static void app_timer_isr(const void *arg)
{
	ARG_UNUSED(arg);

	/* Clear the status before arming again: the comparator is held off
	 * while it is set.
	 */
	sys_write32(BIT(0), TMR_INT_STATUS);
	timer_core_announce();
}

void sys_clock_no_timeout(void)
{
	if (!IS_ENABLED(CONFIG_TICKLESS_KERNEL)) {
		return;
	}
	app_timer_set_compare(UINT64_MAX);
}

__boot_func
static int sys_clock_driver_init(void)
{
	IRQ_CONNECT(DT_INST_IRQN(0), DT_INST_IRQ(0, priority), app_timer_isr, NULL, 0);

	/* Interrupt by MSI to the local APIC at the vector the IRQ got */
	sys_write32(LOAPIC_MSI_ADDR, CFG_MSI_ADDR);
	sys_write32(Z_IRQ_TO_INTERRUPT_VECTOR(DT_INST_IRQN(0)), CFG_MSI_DATA);
	sys_write32(sys_read32(CFG_MSI_CTRL) | MSI_ENABLE, CFG_MSI_CTRL);

	sys_write32(sys_read32(TMR_CONTROL) & ~BIT(27), TMR_CONTROL);
	app_timer_set_compare(UINT64_MAX);
	sys_write32(BIT(0), TMR_INT_STATUS);
	sys_write32(0, TMR_MASK);
	sys_write32(BIT(0), TMR_ENABLE);
	sys_write32(BIT(0), TMR_ARM);
	irq_enable(DT_INST_IRQN(0));

	timer_core_init();

	return 0;
}

SYS_INIT(sys_clock_driver_init, PRE_KERNEL_2, CONFIG_SYSTEM_CLOCK_INIT_PRIORITY);
