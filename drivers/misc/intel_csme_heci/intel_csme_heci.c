/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Intel CSME HECI function, engine side: HFSTS1 +0x04, HFSTS2-6 +0x10 to
 * +0x20, ME_CSR +0x30, UMA request +0x5c (bits 0-6 MiB, bit 16 valid).
 */

#define DT_DRV_COMPAT intel_csme_heci

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/drivers/misc/intel_csme_heci/intel_csme_heci.h>

#define HECI_ME_CSR    0x30U
#define HECI_UMA       0x5cU
#define UMA_SIZE_MASK  0x7fU
#define UMA_VALID      BIT(16)

struct heci_config {
	mm_reg_t regs;
};

struct heci_data {
	struct k_spinlock lock;
};

static const uint8_t fwsts_reg[] = {0x04, 0x10, 0x14, 0x18, 0x1c, 0x20};

static mm_reg_t fwsts_addr(const struct device *dev, unsigned int n)
{
	const struct heci_config *cfg = dev->config;

	__ASSERT(n >= 1U && n <= ARRAY_SIZE(fwsts_reg), "no HFSTS%u", n);
	return cfg->regs + fwsts_reg[n - 1U];
}

static uint32_t heci_update(const struct device *dev, mm_reg_t a, uint32_t mask, uint32_t val)
{
	struct heci_data *data = dev->data;
	k_spinlock_key_t key = k_spin_lock(&data->lock);
	uint32_t old = sys_read32(a);

	sys_write32((old & ~mask) | (val & mask), a);
	k_spin_unlock(&data->lock, key);
	return old;
}

uint32_t intel_csme_heci_fwsts_read(const struct device *dev, unsigned int n)
{
	return sys_read32(fwsts_addr(dev, n));
}

uint32_t intel_csme_heci_fwsts_update(const struct device *dev, unsigned int n, uint32_t mask,
				      uint32_t val)
{
	return heci_update(dev, fwsts_addr(dev, n), mask, val);
}

void intel_csme_heci_fwsts_write(const struct device *dev, unsigned int n, uint32_t val)
{
	sys_write32(val, fwsts_addr(dev, n));
}

uint32_t intel_csme_heci_csr_update(const struct device *dev, uint32_t mask, uint32_t val)
{
	const struct heci_config *cfg = dev->config;

	return heci_update(dev, cfg->regs + HECI_ME_CSR, mask, val);
}

void intel_csme_heci_uma_request(const struct device *dev, uint32_t mib)
{
	const struct heci_config *cfg = dev->config;

	(void)heci_update(dev, cfg->regs + HECI_UMA, UMA_SIZE_MASK, mib);
	(void)heci_update(dev, cfg->regs + HECI_UMA, UMA_VALID, UMA_VALID);
}

#define HECI_INIT(n)                                                                       \
	static const struct heci_config heci_config_##n = {                                \
		.regs = DT_INST_REG_ADDR(n),                                               \
	};                                                                                 \
	static struct heci_data heci_data_##n;                                             \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL, &heci_data_##n, &heci_config_##n,             \
			      PRE_KERNEL_1, CONFIG_INTEL_CSME_HECI_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(HECI_INIT)
