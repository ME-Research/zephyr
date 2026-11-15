/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The always-on registers of the Intel PCH's PRTC agent (sideband port
 * 0x8e) as the CSME reaches them through an ATT window, exposed as
 * battery-backed RAM. They sit at +0x1000 of the window and survive CSME
 * resets and host power states:
 *   +0x00     a free-running seconds counter (read-only)
 *   +0x1c     bit 0 busy: a write must wait for it to clear
 *   +0x38     flags the CSME firmware keeps across resets
 *   +0x40...  scratch registers
 * The window takes 32-bit accesses only; partial words are written
 * read-modify-write. The agent's +0x44 (MSI control) is cleared when the
 * CSME kernel quiesces its devices: the suspend action.
 */

#define DT_DRV_COMPAT intel_csme_prtc

#include <zephyr/device.h>
#include <zephyr/drivers/bbram.h>
#include <zephyr/drivers/misc/intel_csme_att/intel_csme_att.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/util.h>

#define PRTC_BUSY    0x1cU
#define PRTC_MSI_CTL 0x44U

struct prtc_config {
	const struct device *att;
	struct intel_csme_sb_target sb;
	uint32_t regs;
	size_t size;
};

static int prtc_get_size(const struct device *dev, size_t *size)
{
	const struct prtc_config *cfg = dev->config;

	*size = cfg->size;
	return 0;
}

static int prtc_check(const struct prtc_config *cfg, size_t offset, size_t size)
{
	return (size == 0U || offset >= cfg->size || size > cfg->size - offset) ? -EFAULT : 0;
}

static int prtc_read(const struct device *dev, size_t offset, size_t size, uint8_t *data)
{
	const struct prtc_config *cfg = dev->config;

	if (prtc_check(cfg, offset, size) != 0) {
		return -EFAULT;
	}
	while (size > 0U) {
		uint32_t at = offset & 3U;
		size_t n = MIN(size, 4U - at);
		uint32_t v = intel_csme_att_sb_read32(cfg->att, &cfg->sb,
						      cfg->regs + (offset & ~3U));

		for (size_t i = 0; i < n; i++) {
			data[i] = (uint8_t)(v >> (8U * (at + i)));
		}
		data += n;
		offset += n;
		size -= n;
	}
	return 0;
}

static int prtc_write(const struct device *dev, size_t offset, size_t size, const uint8_t *data)
{
	const struct prtc_config *cfg = dev->config;

	if (prtc_check(cfg, offset, size) != 0) {
		return -EFAULT;
	}
	while (size > 0U) {
		uint32_t at = offset & 3U;
		size_t n = MIN(size, 4U - at);
		uint32_t reg = cfg->regs + (offset & ~3U);
		uint32_t mask = 0, v = 0;

		for (size_t i = 0; i < n; i++) {
			mask |= 0xffU << (8U * (at + i));
			v |= (uint32_t)data[i] << (8U * (at + i));
		}
		while ((intel_csme_att_sb_read32(cfg->att, &cfg->sb, cfg->regs + PRTC_BUSY) &
			BIT(0)) != 0U) {
		}
		if (mask == UINT32_MAX) {
			intel_csme_att_sb_write32(cfg->att, &cfg->sb, reg, v);
		} else {
			(void)intel_csme_att_sb_update32(cfg->att, &cfg->sb, reg, mask, v);
		}
		data += n;
		offset += n;
		size -= n;
	}
	return 0;
}

static DEVICE_API(bbram, prtc_api) = {
	.get_size = prtc_get_size,
	.read = prtc_read,
	.write = prtc_write,
};

static int prtc_pm_action(const struct device *dev, enum pm_device_action action)
{
	const struct prtc_config *cfg = dev->config;

	switch (action) {
	case PM_DEVICE_ACTION_SUSPEND:
		intel_csme_att_sb_write32(cfg->att, &cfg->sb, PRTC_MSI_CTL, 0);
		return 0;
	case PM_DEVICE_ACTION_RESUME:
		return 0;
	default:
		return -ENOTSUP;
	}
}

static int prtc_init(const struct device *dev)
{
	const struct prtc_config *cfg = dev->config;

	return device_is_ready(cfg->att) ? 0 : -ENODEV;
}

#define PRTC_INIT(n)                                                                       \
	static const struct prtc_config prtc_config_##n = {                                \
		.att = INTEL_CSME_SB_ATT_DT(DT_DRV_INST(n)),                               \
		.sb = INTEL_CSME_SB_TARGET_DT(DT_DRV_INST(n)),                             \
		.regs = DT_INST_PROP(n, regs_offset),                                      \
		.size = DT_INST_PROP(n, size),                                             \
	};                                                                                 \
	PM_DEVICE_DT_INST_DEFINE(n, prtc_pm_action);                                       \
	DEVICE_DT_INST_DEFINE(n, prtc_init, PM_DEVICE_DT_INST_GET(n), NULL,                \
			      &prtc_config_##n, POST_KERNEL, CONFIG_BBRAM_INIT_PRIORITY,    \
			      &prtc_api);

DT_INST_FOREACH_STATUS_OKAY(PRTC_INIT)
