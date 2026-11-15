/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Intel PCH fuse controller as the CSME reaches it: two IOSF sideband
 * agents (port 0xd5) behind ATT windows, the fuse array (flags 0x31) and
 * the controller (flags 0x34). The controller is taken by setting bit 17 of
 * its +0x00 once bit 16 (busy) is clear; it is ours when the low byte reads
 * 0x90, and bit 18 releases it.
 *
 * Margin sensing (controller +0x04 to +0x18, +0x80, +0x1020/+0x1024) is
 * the CSME firmware's sequence; the meaning of its values is not known.
 */

#define DT_DRV_COMPAT intel_pch_fuse

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/misc/intel_csme_att/intel_csme_att.h>
#include <zephyr/drivers/otp.h>
#include <zephyr/drivers/otp/intel_pch_fuse.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

#define CTL           0x00U
#define CTL_OWNED     0x90U
#define CTL_BUSY      BIT(16)
#define CTL_TAKE      BIT(17)
#define CTL_RELEASE   BIT(18)
#define TAKE_TRIES    1000

#define ARRAY_SIZE_BYTES 0x800U

struct fuse_config {
	const struct device *att;
	struct intel_csme_sb_target array;
	struct intel_csme_sb_target ctrl;
};

struct fuse_data {
	struct k_mutex lock;
};

static uint32_t ctrl_rd(const struct fuse_config *cfg, uint32_t off)
{
	return intel_csme_att_sb_read32(cfg->att, &cfg->ctrl, off);
}

static void ctrl_wr(const struct fuse_config *cfg, uint32_t off, uint32_t v)
{
	intel_csme_att_sb_write32(cfg->att, &cfg->ctrl, off, v);
}

static uint32_t array_rd(const struct fuse_config *cfg, uint32_t off)
{
	return intel_csme_att_sb_read32(cfg->att, &cfg->array, off);
}

static int fuse_take(const struct fuse_config *cfg)
{
	uint32_t v = ctrl_rd(cfg, CTL);
	int i;

	if ((v & 0xffU) == CTL_OWNED) {
		return 0;
	}
	for (i = 0; (v & CTL_BUSY) != 0U; i++) {
		if (i == TAKE_TRIES) {
			return -ETIMEDOUT;
		}
		v = ctrl_rd(cfg, CTL);
	}
	ctrl_wr(cfg, CTL, v | CTL_TAKE);
	v = ctrl_rd(cfg, CTL);
	for (i = 0; (v & 0xffU) != CTL_OWNED; i++) {
		if (i == TAKE_TRIES) {
			return -ETIMEDOUT;
		}
		v = ctrl_rd(cfg, CTL);
	}
	return 0;
}

static void fuse_release(const struct fuse_config *cfg)
{
	ctrl_wr(cfg, CTL, ctrl_rd(cfg, CTL) | CTL_RELEASE);
}

static int fuse_read(const struct device *dev, off_t offset, void *data, size_t len)
{
	const struct fuse_config *cfg = dev->config;
	struct fuse_data *d = dev->data;
	uint8_t *p = data;
	int ret;

	if (offset < 0 || (offset & 3) != 0 || (len & 3U) != 0U ||
	    (size_t)offset + len > ARRAY_SIZE_BYTES) {
		return -EINVAL;
	}
	(void)k_mutex_lock(&d->lock, K_FOREVER);
	ret = fuse_take(cfg);
	for (size_t i = 0; i < len; i += 4U) {
		sys_put_le32(array_rd(cfg, (uint32_t)offset + i), p + i);
	}
	fuse_release(cfg);
	(void)k_mutex_unlock(&d->lock);
	return ret;
}

static void low_byte(const struct fuse_config *cfg, uint32_t off, uint32_t v)
{
	ctrl_wr(cfg, off, (ctrl_rd(cfg, off) & 0xffffff00U) | v);
	k_busy_wait(5);
}

static void margin_enter(const struct fuse_config *cfg)
{
	low_byte(cfg, 0x1024, 0x83);
	low_byte(cfg, 0x1020, 0x83);
	ctrl_wr(cfg, 0x04, 1);
	(void)ctrl_rd(cfg, 0x18);
	ctrl_wr(cfg, 0x18, 0x304b1);
}

static void margin_exit(const struct fuse_config *cfg)
{
	ctrl_wr(cfg, 0x04, 6);
	low_byte(cfg, 0x1020, 0);
	low_byte(cfg, 0x1024, 0);
}

static int margin_sense(const struct fuse_config *cfg, int margin, unsigned int bank,
			unsigned int sel)
{
	uint32_t v;

	ctrl_wr(cfg, 0x04, (sel & 7U) * 8U + 1U);
	(void)ctrl_rd(cfg, 0x04);
	ctrl_wr(cfg, 0x80, ctrl_rd(cfg, 0x80) | BIT(31));
	low_byte(cfg, 0x1020, 0x83);
	low_byte(cfg, 0x1024, 0x83);
	ctrl_wr(cfg, 0x14, ctrl_rd(cfg, 0x14) & ~BIT(22));
	k_busy_wait(5);
	ctrl_wr(cfg, 0x08, 0x44ce0864);
	ctrl_wr(cfg, 0x0c, BIT(17) | 0x400U | (bank & 7U) << 7);
	ctrl_wr(cfg, 0x10, 0x20);
	ctrl_wr(cfg, 0x14, margin == 0 ? 0x88e000U : margin == 2 ? 0x8a0000U : 0x880000U);
	do {
		v = ctrl_rd(cfg, 0x80);
	} while ((v & 6U) == 0U);
	return (v & 4U) != 0U ? -EIO : 0;
}

/* a known word (bank 0 word 16, selector 2) reads bits 31-30 set, 29-28 clear */
static int margin_selftest(const struct fuse_config *cfg)
{
	uint32_t v;

	if (margin_sense(cfg, 1, 0, 2) != 0) {
		return -EIO;
	}
	v = array_rd(cfg, 0x44U);
	return ((v & 0x30000000U) == 0U && v >> 30 == 3U) ? 0 : -EIO;
}

int intel_pch_fuse_margin_read(const struct device *dev, int margin, unsigned int bank,
			       unsigned int sel, bool selftest, uint32_t words[32])
{
	const struct fuse_config *cfg = dev->config;
	struct fuse_data *d = dev->data;
	int ret;

	(void)k_mutex_lock(&d->lock, K_FOREVER);
	ret = fuse_take(cfg);
	if (ret != 0) {
		goto out;
	}
	margin_enter(cfg);
	if (selftest && margin_selftest(cfg) != 0) {
		ret = -EIO;
		goto out;
	}
	ret = margin_sense(cfg, margin, bank, sel);
	if (ret != 0) {
		goto out;
	}
	for (unsigned int w = 0; w < 32U; w++) {
		words[w] = array_rd(cfg, INTEL_PCH_FUSE_SENSED(bank) + w * 4U);
	}
	if (selftest && margin_selftest(cfg) != 0) {
		ret = -EIO;
		goto out;
	}
	margin_exit(cfg);
	fuse_release(cfg);
out:
	(void)k_mutex_unlock(&d->lock);
	return ret;
}

int intel_pch_fuse_shadow_write(const struct device *dev, uint32_t offset,
				const uint32_t *words, size_t n)
{
	const struct fuse_config *cfg = dev->config;
	struct fuse_data *d = dev->data;
	int ret;

	(void)k_mutex_lock(&d->lock, K_FOREVER);
	ret = fuse_take(cfg);
	for (size_t i = 0; i < n; i++) {
		intel_csme_att_sb_write32(cfg->att, &cfg->array, offset + 4U * i, words[i]);
	}
	fuse_release(cfg);
	(void)k_mutex_unlock(&d->lock);
	return ret;
}

static DEVICE_API(otp, fuse_api) = {
	.read = fuse_read,
};

static int fuse_init(const struct device *dev)
{
	const struct fuse_config *cfg = dev->config;
	struct fuse_data *d = dev->data;

	k_mutex_init(&d->lock);
	return device_is_ready(cfg->att) ? 0 : -ENODEV;
}

#define FUSE_ARRAY(n) DT_INST_PHANDLE(n, intel_array)
#define FUSE_CTRL(n)  DT_INST_PHANDLE(n, intel_controller)

#define FUSE_INIT(n)                                                                       \
	static const struct fuse_config fuse_config_##n = {                                \
		.att = INTEL_CSME_SB_ATT_DT(FUSE_ARRAY(n)),                                \
		.array = INTEL_CSME_SB_TARGET_DT(FUSE_ARRAY(n)),                           \
		.ctrl = INTEL_CSME_SB_TARGET_DT(FUSE_CTRL(n)),                             \
	};                                                                                 \
	static struct fuse_data fuse_data_##n;                                             \
	DEVICE_DT_INST_DEFINE(n, fuse_init, NULL, &fuse_data_##n, &fuse_config_##n,        \
			      POST_KERNEL, CONFIG_OTP_INIT_PRIORITY, &fuse_api);

DT_INST_FOREACH_STATUS_OKAY(FUSE_INIT)
