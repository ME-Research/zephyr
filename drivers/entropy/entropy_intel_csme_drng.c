/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Intel PCH DRNG as the CSME sees it: a sideband agent (port 0xd2) behind
 * an ATT window. +0x00 reads a 32-bit sample, 0 while none is ready;
 * +0x08 bits 0-3 are the status the boot ROM checks after starting it
 * (+0x18 = 0xdc80, +0x0c = 0x44050), all set when the generator runs.
 */

#define DT_DRV_COMPAT intel_csme_drng

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/drivers/misc/intel_csme_att/intel_csme_att.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(entropy_intel_csme_drng, CONFIG_ENTROPY_LOG_LEVEL);

#define DRNG_DATA    0x00U
#define DRNG_STATUS  0x08U
#define DRNG_RUNNING 0xfU

struct drng_config {
	const struct device *att;
	struct intel_csme_sb_target sb;
};

static uint32_t drng_sample(const struct drng_config *cfg)
{
	uint32_t v;

	do {
		v = intel_csme_att_sb_read32(cfg->att, &cfg->sb, DRNG_DATA);
	} while (v == 0U);
	return v;
}

static int drng_get_entropy(const struct device *dev, uint8_t *buf, uint16_t len)
{
	const struct drng_config *cfg = dev->config;

	while (len > 0U) {
		uint32_t v = drng_sample(cfg);
		uint16_t n = MIN(len, sizeof(v));

		memcpy(buf, &v, n);
		buf += n;
		len -= n;
	}
	return 0;
}

static int drng_get_entropy_isr(const struct device *dev, uint8_t *buf, uint16_t len,
				uint32_t flags)
{
	ARG_UNUSED(flags);

	/* a sample is ready within a few register reads; always busy-wait */
	(void)drng_get_entropy(dev, buf, len);
	return len;
}

static DEVICE_API(entropy, drng_api) = {
	.get_entropy = drng_get_entropy,
	.get_entropy_isr = drng_get_entropy_isr,
};

static int drng_init(const struct device *dev)
{
	const struct drng_config *cfg = dev->config;
	uint32_t sts;

	if (!device_is_ready(cfg->att)) {
		return -ENODEV;
	}
	sts = intel_csme_att_sb_read32(cfg->att, &cfg->sb, DRNG_STATUS);
	if ((sts & DRNG_RUNNING) != DRNG_RUNNING) {
		LOG_ERR("DRNG not running (status %08x)", sts);
		return -EIO;
	}
	return 0;
}

#define DRNG_INIT(n)                                                                       \
	static const struct drng_config drng_config_##n = {                                \
		.att = INTEL_CSME_SB_ATT_DT(DT_DRV_INST(n)),                               \
		.sb = INTEL_CSME_SB_TARGET_DT(DT_DRV_INST(n)),                             \
	};                                                                                 \
	DEVICE_DT_INST_DEFINE(n, drng_init, NULL, NULL, &drng_config_##n, PRE_KERNEL_1,    \
			      CONFIG_ENTROPY_INIT_PRIORITY, &drng_api);

DT_INST_FOREACH_STATUS_OKAY(DRNG_INIT)
