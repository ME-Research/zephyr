/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Reset and power-off through the PCH PMC, the way the CSME kernel does
 * them (its power-transition taskcall): the PMC's BARs again, the AON agent
 * marked down, the system timer disarmed, every device quiesced, then the
 * request to the PMC and the engine's power-gate control.
 *
 * sys_reboot(SYS_REBOOT_WARM) resets the CSME alone; SYS_REBOOT_COLD
 * resets the platform; sys_poweroff() powers the platform down.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/misc/intel_pch_pmc/intel_pch_pmc.h>
#include <zephyr/drivers/syscon.h>
#include <zephyr/drivers/timer/system_timer.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/reboot.h>
#include <soc.h>

/* CSME control block (GEN): +0x00 power-gate control, +0x04 bit 0 request
 * the engine's fabric to drain, bit 1 its acknowledge
 */
#define GEN_PG_CTL        0x00U
#define GEN_PG_CSME_RESET 0x11U
#define GEN_DRAIN         0x04U
#define GEN_DRAIN_REQ     BIT(0)
#define GEN_DRAIN_ACK     BIT(1)

static const struct device *const pmc = DEVICE_DT_GET(DT_NODELABEL(pmc));
static const struct device *const gen = DEVICE_DT_GET(DT_NODELABEL(gen));

#define SUSPEND(node_id)                                                                   \
	(void)pm_device_action_run(DEVICE_DT_GET(node_id), PM_DEVICE_ACTION_SUSPEND);

/* setup_devs: the functions with no other driver, then the PRTC agent,
 * then the fabric drain
 */
static void quiesce(void)
{
	uint32_t v;

	DT_FOREACH_STATUS_OKAY(intel_csme_pci_function, SUSPEND)
#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(prtc))
	SUSPEND(DT_NODELABEL(prtc))
#endif
	(void)syscon_read_reg(gen, GEN_DRAIN, &v);
	(void)syscon_write_reg(gen, GEN_DRAIN, v | GEN_DRAIN_REQ);
	do {
		(void)syscon_read_reg(gen, GEN_DRAIN, &v);
	} while ((v & GEN_DRAIN_ACK) == 0U);
}

static FUNC_NORETURN void wait_for_pmc(void)
{
	for (;;) {
		k_cpu_idle();
	}
}

void sys_arch_reboot(int type)
{
	if (type == SYS_REBOOT_COLD) {
		intel_pch_pmc_platform_reset(pmc);
		wait_for_pmc();
	}
	intel_pch_pmc_enable(pmc);
#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(aon))
	SUSPEND(DT_NODELABEL(aon))
#endif
	sys_clock_disable();
	quiesce();
	intel_pch_pmc_csme_reset(pmc);
	(void)syscon_write_reg(gen, GEN_PG_CTL, GEN_PG_CSME_RESET);
	wait_for_pmc();
}

int intel_csme11_power_down(void)
{
	intel_pch_pmc_enable(pmc);
	quiesce();
	return intel_pch_pmc_power_down(pmc);
}

void z_sys_poweroff(void)
{
	(void)intel_csme11_power_down();
	wait_for_pmc();
}
