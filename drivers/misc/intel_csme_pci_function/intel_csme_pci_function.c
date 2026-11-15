/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * A PCI function of the Intel CSME's root space or private bus that has no
 * other driver here but must be quiesced before the engine resets or
 * powers down. Suspending it does what the CSME kernel does for each such
 * function: MSI off (+0x44); if it is a bus master, its BAR programmed,
 * memory decode on, its DMA stopped (SMBus functions), bus master and
 * memory decode off; then D3hot with PME status cleared (PMCSR 0x8003).
 */

#define DT_DRV_COMPAT intel_csme_pci_function

#include <zephyr/device.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#define PCI_CMD          0x04U
#define PCI_CMD_MEM      BIT(1)
#define PCI_CMD_MASTER   BIT(2)
#define PCI_BAR0         0x10U
#define PCI_MSI_CTL      0x44U
#define PCI_PMCSR        0x54U
#define PMCSR_D3_PME_STS 0x8003U

/* SMBus (SMT) DMA: +0x04 bits 4, 5, 17 and +0x114 bit 1 busy, +0x158 bit 0 enable */
#define SMT_STS          0x004U
#define SMT_STS_BUSY     0x20030U
#define SMT_DMA_STS      0x114U
#define SMT_DMA_BUSY     BIT(1)
#define SMT_DMA_CTL      0x158U
#define SMT_DMA_EN       BIT(0)

struct pci_function_config {
	mm_reg_t cfg;
	uintptr_t bar;
	bool smbus;
};

static void smbus_stop_dma(mm_reg_t bar)
{
	while ((sys_read32(bar + SMT_STS) & SMT_STS_BUSY) != 0U) {
	}
	while ((sys_read32(bar + SMT_DMA_STS) & SMT_DMA_BUSY) != 0U) {
	}
	sys_write32(sys_read32(bar + SMT_DMA_CTL) & ~SMT_DMA_EN, bar + SMT_DMA_CTL);
}

static int pci_function_pm_action(const struct device *dev, enum pm_device_action action)
{
	const struct pci_function_config *cfg = dev->config;

	if (action == PM_DEVICE_ACTION_RESUME) {
		return 0;
	}
	if (action != PM_DEVICE_ACTION_SUSPEND) {
		return -ENOTSUP;
	}
	sys_write32(0, cfg->cfg + PCI_MSI_CTL);
	if ((sys_read32(cfg->cfg + PCI_CMD) & PCI_CMD_MASTER) != 0U && cfg->bar != 0U) {
		sys_write32(cfg->bar, cfg->cfg + PCI_BAR0);
		sys_write32(sys_read32(cfg->cfg + PCI_CMD) | PCI_CMD_MEM, cfg->cfg + PCI_CMD);
		if (cfg->smbus) {
			smbus_stop_dma(cfg->bar);
		}
		sys_write32(sys_read32(cfg->cfg + PCI_CMD) & ~(PCI_CMD_MEM | PCI_CMD_MASTER),
			    cfg->cfg + PCI_CMD);
	}
	sys_write32(PMCSR_D3_PME_STS, cfg->cfg + PCI_PMCSR);
	return 0;
}

static int pci_function_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

#define PCI_FUNCTION_INIT(n)                                                               \
	static const struct pci_function_config pci_function_config_##n = {                \
		.cfg = DT_INST_REG_ADDR(n),                                                \
		.bar = DT_INST_PROP_OR(n, bar, 0),                                         \
		.smbus = DT_INST_PROP(n, intel_smbus),                                     \
	};                                                                                 \
	PM_DEVICE_DT_INST_DEFINE(n, pci_function_pm_action);                               \
	DEVICE_DT_INST_DEFINE(n, pci_function_init, PM_DEVICE_DT_INST_GET(n), NULL,        \
			      &pci_function_config_##n, PRE_KERNEL_1,                      \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, NULL);

DT_INST_FOREACH_STATUS_OKAY(PCI_FUNCTION_INIT)
