/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Intel PCH PMC as the CSME sees it: PCI function 0:1a.0 of the CSME root
 * space, registers in BAR0, SRAM (firmware patches, CPU descriptors) in
 * BAR1. The driver programs both BARs and memory decoding at init.
 */

#define DT_DRV_COMPAT intel_pch_pmc

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/drivers/misc/intel_pch_pmc/intel_pch_pmc.h>

#define PCI_CMD        0x04U
#define PCI_CMD_MEM    BIT(1)
#define PCI_BAR0       0x10U
#define PCI_BAR1       0x14U

#define IPC_CTL_ACK    BIT(0)
#define IPC_CTL_SEND   BIT(1)
#define IPC_CMD_RETENTION_WRITE 3U

#define CTL_POWER_DOWN     BIT(0)
#define CTL_PLATFORM_RESET (BIT(0) | BIT(6))
#define STATE_EVENT_HELD   BIT(18)
#define RST_TYPE_CSME      BIT(2)
#define RST_TYPE_BIT0      BIT(0)
#define PATCH_CTL_LOCK     BIT(31)

struct pmc_config {
	mm_reg_t regs;
	mm_reg_t sram;
	mm_reg_t cfg;
};

struct pmc_data {
	struct k_spinlock lock;
	bool power_down_returned;
};

uint32_t intel_pch_pmc_read(const struct device *dev, uint32_t reg)
{
	const struct pmc_config *cfg = dev->config;

	return sys_read32(cfg->regs + reg);
}

void intel_pch_pmc_write(const struct device *dev, uint32_t reg, uint32_t val)
{
	const struct pmc_config *cfg = dev->config;

	sys_write32(val, cfg->regs + reg);
}

uint32_t intel_pch_pmc_update(const struct device *dev, uint32_t reg, uint32_t mask, uint32_t val)
{
	const struct pmc_config *cfg = dev->config;
	struct pmc_data *data = dev->data;
	k_spinlock_key_t key = k_spin_lock(&data->lock);
	uint32_t old = sys_read32(cfg->regs + reg);

	sys_write32((old & ~mask) | (val & mask), cfg->regs + reg);
	k_spin_unlock(&data->lock, key);
	return old;
}

uint32_t intel_pch_pmc_cfg_read(const struct device *dev, uint32_t reg)
{
	const struct pmc_config *cfg = dev->config;

	return sys_read32(cfg->cfg + reg);
}

void intel_pch_pmc_cfg_write(const struct device *dev, uint32_t reg, uint32_t val)
{
	const struct pmc_config *cfg = dev->config;

	sys_write32(val, cfg->cfg + reg);
}

void intel_pch_pmc_ipc_send(const struct device *dev, uint32_t msg, bool wait)
{
	intel_pch_pmc_write(dev, INTEL_PCH_PMC_IPC_MSG, msg);
	if (!wait) {
		intel_pch_pmc_write(dev, INTEL_PCH_PMC_IPC_CTL, IPC_CTL_SEND);
		return;
	}
	(void)intel_pch_pmc_update(dev, INTEL_PCH_PMC_IPC_CTL, IPC_CTL_SEND, IPC_CTL_SEND);
	while ((intel_pch_pmc_read(dev, INTEL_PCH_PMC_IPC_CTL) & IPC_CTL_SEND) != 0U) {
	}
}

uint32_t intel_pch_pmc_ipc_reply(const struct device *dev)
{
	return intel_pch_pmc_read(dev, INTEL_PCH_PMC_IPC_REPLY);
}

void intel_pch_pmc_ipc_retention_write(const struct device *dev, uint8_t off,
				       const uint8_t *buf, size_t len)
{
	for (size_t i = 0; i + 1U < len; i += 2U) {
		uint32_t half = buf[i] | (uint32_t)buf[i + 1U] << 8;

		intel_pch_pmc_ipc_send(dev, half << 16 | (uint32_t)(uint8_t)(off + i) << 8 |
					    IPC_CMD_RETENTION_WRITE, true);
	}
}

void intel_pch_pmc_sram_write(const struct device *dev, uint32_t off, const void *buf,
			      size_t len)
{
	const struct pmc_config *cfg = dev->config;
	const uint8_t *p = buf;

	while (len > 0U) {
		if ((off & 3U) == 0U && len >= 4U) {
			sys_write32(sys_get_le32(p), cfg->sram + off);
			off += 4U;
			p += 4;
			len -= 4U;
		} else {
			sys_write8(*p, cfg->sram + off);
			off++;
			p++;
			len--;
		}
	}
}

void intel_pch_pmc_sram_read(const struct device *dev, uint32_t off, void *buf, size_t len)
{
	const struct pmc_config *cfg = dev->config;
	uint8_t *p = buf;

	while (len > 0U) {
		if ((off & 3U) == 0U && len >= 4U) {
			sys_put_le32(sys_read32(cfg->sram + off), p);
			off += 4U;
			p += 4;
			len -= 4U;
		} else {
			*p = sys_read8(cfg->sram + off);
			off++;
			p++;
			len--;
		}
	}
}

void intel_pch_pmc_csme_reset(const struct device *dev)
{
	uint32_t state = intel_pch_pmc_read(dev, INTEL_PCH_PMC_STATE);
	uint32_t v = intel_pch_pmc_read(dev, INTEL_PCH_PMC_RST_TYPE);

	intel_pch_pmc_write(dev, INTEL_PCH_PMC_RST_TYPE, (v | RST_TYPE_CSME) & ~RST_TYPE_BIT0);
	if ((state & STATE_EVENT_HELD) != 0U) {
		/* acknowledge the power event the PMC holds the reset for */
		(void)intel_pch_pmc_update(dev, INTEL_PCH_PMC_IPC_CTL, IPC_CTL_ACK, IPC_CTL_ACK);
		while ((intel_pch_pmc_read(dev, INTEL_PCH_PMC_IPC_CTL) & IPC_CTL_ACK) != 0U) {
		}
	}
}

int intel_pch_pmc_power_down(const struct device *dev)
{
	struct pmc_data *data = dev->data;

	(void)intel_pch_pmc_update(dev, INTEL_PCH_PMC_CTL, CTL_POWER_DOWN, CTL_POWER_DOWN);
	if (!data->power_down_returned &&
	    (intel_pch_pmc_cfg_read(dev, INTEL_PCH_PMC_CFG_PATCH_CTL) & PATCH_CTL_LOCK) == 0U) {
		data->power_down_returned = true;
		return 0;
	}
	return 1;
}

void intel_pch_pmc_platform_reset(const struct device *dev)
{
	(void)intel_pch_pmc_update(dev, INTEL_PCH_PMC_CTL, CTL_PLATFORM_RESET,
				   CTL_PLATFORM_RESET);
}

void intel_pch_pmc_enable(const struct device *dev)
{
	const struct pmc_config *cfg = dev->config;

	sys_write32((uint32_t)cfg->regs, cfg->cfg + PCI_BAR0);
	sys_write32((uint32_t)cfg->sram, cfg->cfg + PCI_BAR1);
	sys_write32(sys_read32(cfg->cfg + PCI_CMD) | PCI_CMD_MEM, cfg->cfg + PCI_CMD);
}

static int pmc_init(const struct device *dev)
{
	intel_pch_pmc_enable(dev);
	return 0;
}

#define PMC_INIT(n)                                                                        \
	static const struct pmc_config pmc_config_##n = {                                  \
		.regs = DT_INST_REG_ADDR_BY_NAME(n, regs),                                 \
		.sram = DT_INST_REG_ADDR_BY_NAME(n, sram),                                 \
		.cfg = DT_INST_REG_ADDR_BY_NAME(n, cfg),                                   \
	};                                                                                 \
	static struct pmc_data pmc_data_##n;                                               \
	DEVICE_DT_INST_DEFINE(n, pmc_init, NULL, &pmc_data_##n, &pmc_config_##n,           \
			      PRE_KERNEL_1, CONFIG_INTEL_PCH_PMC_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(PMC_INIT)
