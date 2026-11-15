/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The always-on SRAM of the Intel PCH's AON agent (sideband port 0xb9),
 * as the CSME reaches it through an ATT window. It keeps its contents
 * across CSME resets and host Sx states and is lost with the suspend well.
 * The window takes 32-bit accesses only, so partial words are written
 * read-modify-write.
 *
 * The agent's own registers sit below the SRAM: +0x54 is its PMCSR (bits
 * 0-1 power state) and +0x64 bit 2 a flag the CSME kernel sets with D3
 * when the CSME goes down (maybe_aon_mark_csme_down); that is the device's
 * suspend action. The boot ROM puts the agent back in D0.
 */

#define DT_DRV_COMPAT intel_csme_aon

#include <zephyr/device.h>
#include <zephyr/drivers/retained_mem.h>
#include <zephyr/drivers/misc/intel_csme_att/intel_csme_att.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/util.h>

#define AON_PMCSR      0x54U
#define AON_PMCSR_D3   0x3U
#define AON_DOWN       0x64U
#define AON_DOWN_FLAG  BIT(2)

struct aon_config {
	const struct device *att;
	struct intel_csme_sb_target sb;
	uint32_t sram_offset;
	size_t sram_size;
};

struct aon_data {
#ifdef CONFIG_RETAINED_MEM_MUTEXES
	struct k_mutex lock;
#endif
};

static void aon_lock(const struct device *dev)
{
#ifdef CONFIG_RETAINED_MEM_MUTEXES
	struct aon_data *data = dev->data;

	(void)k_mutex_lock(&data->lock, K_FOREVER);
#else
	ARG_UNUSED(dev);
#endif
}

static void aon_unlock(const struct device *dev)
{
#ifdef CONFIG_RETAINED_MEM_MUTEXES
	struct aon_data *data = dev->data;

	(void)k_mutex_unlock(&data->lock);
#else
	ARG_UNUSED(dev);
#endif
}

static ssize_t aon_size(const struct device *dev)
{
	const struct aon_config *cfg = dev->config;

	return (ssize_t)cfg->sram_size;
}

static int aon_read(const struct device *dev, off_t offset, uint8_t *buffer, size_t size)
{
	const struct aon_config *cfg = dev->config;

	aon_lock(dev);
	while (size > 0U) {
		uint32_t word = (uint32_t)offset & ~3U;
		uint32_t at = (uint32_t)offset & 3U;
		size_t n = MIN(size, 4U - at);
		uint32_t v = intel_csme_att_sb_read32(cfg->att, &cfg->sb, cfg->sram_offset + word);

		for (size_t i = 0; i < n; i++) {
			buffer[i] = (uint8_t)(v >> (8U * (at + i)));
		}
		buffer += n;
		offset += n;
		size -= n;
	}
	aon_unlock(dev);
	return 0;
}

static int aon_write(const struct device *dev, off_t offset, const uint8_t *buffer, size_t size)
{
	const struct aon_config *cfg = dev->config;

	aon_lock(dev);
	while (size > 0U) {
		uint32_t word = (uint32_t)offset & ~3U;
		uint32_t at = (uint32_t)offset & 3U;
		size_t n = MIN(size, 4U - at);
		uint32_t mask = 0, v = 0;

		for (size_t i = 0; i < n; i++) {
			mask |= 0xffU << (8U * (at + i));
			v |= (uint32_t)buffer[i] << (8U * (at + i));
		}
		if (mask == UINT32_MAX) {
			intel_csme_att_sb_write32(cfg->att, &cfg->sb, cfg->sram_offset + word, v);
		} else {
			(void)intel_csme_att_sb_update32(cfg->att, &cfg->sb,
							 cfg->sram_offset + word, mask, v);
		}
		buffer += n;
		offset += n;
		size -= n;
	}
	aon_unlock(dev);
	return 0;
}

static int aon_clear(const struct device *dev)
{
	const struct aon_config *cfg = dev->config;

	aon_lock(dev);
	for (uint32_t off = 0; off < cfg->sram_size; off += 4U) {
		intel_csme_att_sb_write32(cfg->att, &cfg->sb, cfg->sram_offset + off, 0);
	}
	aon_unlock(dev);
	return 0;
}

static DEVICE_API(retained_mem, aon_api) = {
	.size = aon_size,
	.read = aon_read,
	.write = aon_write,
	.clear = aon_clear,
};

static int aon_pm_action(const struct device *dev, enum pm_device_action action)
{
	const struct aon_config *cfg = dev->config;

	switch (action) {
	case PM_DEVICE_ACTION_SUSPEND:
		(void)intel_csme_att_sb_update32(cfg->att, &cfg->sb, AON_DOWN, AON_DOWN_FLAG,
						 AON_DOWN_FLAG);
		(void)intel_csme_att_sb_update32(cfg->att, &cfg->sb, AON_PMCSR, AON_PMCSR_D3,
						 AON_PMCSR_D3);
		return 0;
	case PM_DEVICE_ACTION_RESUME:
		(void)intel_csme_att_sb_update32(cfg->att, &cfg->sb, AON_PMCSR, AON_PMCSR_D3, 0);
		return 0;
	default:
		return -ENOTSUP;
	}
}

static int aon_init(const struct device *dev)
{
	const struct aon_config *cfg = dev->config;

	if (!device_is_ready(cfg->att)) {
		return -ENODEV;
	}
#ifdef CONFIG_RETAINED_MEM_MUTEXES
	struct aon_data *data = dev->data;

	k_mutex_init(&data->lock);
#endif
	return 0;
}

#define AON_INIT(n)                                                                        \
	static const struct aon_config aon_config_##n = {                                  \
		.att = INTEL_CSME_SB_ATT_DT(DT_DRV_INST(n)),                               \
		.sb = INTEL_CSME_SB_TARGET_DT(DT_DRV_INST(n)),                             \
		.sram_offset = DT_INST_PROP(n, sram_offset),                               \
		.sram_size = DT_INST_PROP(n, sram_size),                                   \
	};                                                                                 \
	static struct aon_data aon_data_##n;                                               \
	PM_DEVICE_DT_INST_DEFINE(n, aon_pm_action);                                        \
	DEVICE_DT_INST_DEFINE(n, aon_init, PM_DEVICE_DT_INST_GET(n), &aon_data_##n,        \
			      &aon_config_##n, POST_KERNEL, CONFIG_RETAINED_MEM_INIT_PRIORITY,  \
			      &aon_api);

DT_INST_FOREACH_STATUS_OKAY(AON_INIT)
