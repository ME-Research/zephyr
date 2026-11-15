/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Intel CSME crypto engine (PCI function 0:01.0 of the CSME root space;
 * the boot ROM programs its BAR): units at +0x8000 AES, +0xa000 AES-P,
 * +0xb000 SHA, +0xf000 secure key storage (SKS).
 *
 * The cipher is AES-P's single-block path (+0x600 in, +0x700 out): one
 * ECB block per start/finish. A session's key is raw (the key registers)
 * or loaded from an SKS slot. SKS: +0x400 slot select (bit 24 load into
 * AES-P), +0x404 command, +0x408 bit 0 busy / bit 1 error, +0x42c + 4 *
 * slot the slot attributes (bit 0 valid, bits 5-6 access level), +0xc04
 * bits 0-1 the current access level.
 *
 * The register layout comes from reverse engineering the CSME firmware.
 */

#define DT_DRV_COMPAT intel_csme_crypto

#include <errno.h>
#include <string.h>
#include <zephyr/crypto/crypto.h>
#include <zephyr/device.h>
#include <zephyr/drivers/crypto/intel_csme_crypto.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/sys_io.h>

#define AESP_MODE      0xa000U
#define AESP_MODE_DEC  BIT(6)
#define AESP_KEY       0xa004U
#define AESP_CTRL      0xa034U
#define AESP_START     1U
#define AESP_FINISH    2U
#define AESP_BUSY      0xa038U
#define AESP_KEY_SIZE  0xa044U
#define AESP_IN        0xa600U
#define AESP_OUT       0xa700U

#define SHA_MODE       0xb000U
#define SHA_MODE_HMAC_SKS 0xa8420000U
#define SHA_CTRL       0xb008U
#define SHA_STS        0xb050U
#define SHA_STS_DONE   BIT(2)
#define SHA_INT        0xb080U
#define SHA_IN         0xb600U

#define SKS_SEL        0xf400U
#define SKS_SEL_AESP   BIT(24)
#define SKS_CMD        0xf404U
#define SKS_CMD_GO     BIT(31)
#define SKS_LOAD_AES   0x100000U
#define SKS_LOAD_SHA   0x200000U
#define SKS_STORE_HMAC 0x70000U
#define SKS_STS        0xf408U
#define SKS_STS_BUSY   BIT(0)
#define SKS_STS_ERR    BIT(1)
#define SKS_ATTR(n)    (0xf42cU + 4U * (n))
#define SKS_ATTR_VALID BIT(0)
#define SKS_ACCESS     0xfc04U

#define AES_BLOCK 16U

struct crypto_config {
	mm_reg_t regs;
};

struct crypto_data {
	struct k_mutex lock;
	bool in_use;
	bool raw_key;
};

static inline uint32_t rd(const struct device *dev, uint32_t off)
{
	const struct crypto_config *cfg = dev->config;

	return sys_read32(cfg->regs + off);
}

static inline void wr(const struct device *dev, uint32_t off, uint32_t v)
{
	const struct crypto_config *cfg = dev->config;

	sys_write32(v, cfg->regs + off);
}

static uint32_t sks_wait(const struct device *dev)
{
	uint32_t v;

	do {
		v = rd(dev, SKS_STS);
	} while ((v & SKS_STS_BUSY) != 0U);
	return v;
}

/* Load a slot into AES-P at the slot's access level, then restore the
 * selection and the level
 */
static void sks_load_aesp(const struct device *dev, uint32_t slot)
{
	uint32_t sel, access;

	(void)sks_wait(dev);
	sel = rd(dev, SKS_SEL);
	access = rd(dev, SKS_ACCESS);
	wr(dev, SKS_ACCESS, (rd(dev, SKS_ACCESS) & ~3U) | ((rd(dev, SKS_ATTR(slot)) >> 5) & 3U));
	wr(dev, SKS_SEL, (slot & 0xffffffU) | SKS_SEL_AESP);
	wr(dev, SKS_CMD, SKS_CMD_GO | SKS_LOAD_AES);
	(void)sks_wait(dev);
	wr(dev, SKS_SEL, sel);
	wr(dev, SKS_ACCESS, (rd(dev, SKS_ACCESS) & ~3U) | (access & 3U));
}

bool intel_csme_crypto_sks_valid(const struct device *dev, uint32_t slot)
{
	return (rd(dev, SKS_ATTR(slot)) & SKS_ATTR_VALID) != 0U;
}

int intel_csme_crypto_sks_hmac_store(const struct device *dev, uint32_t key_slot,
				     const uint8_t *data, size_t len, uint32_t dst_slot)
{
	const struct crypto_config *cfg = dev->config;
	struct crypto_data *d = dev->data;
	uint32_t v;
	size_t i;
	int rc = 0;

	(void)k_mutex_lock(&d->lock, K_FOREVER);
	wr(dev, SHA_INT, 7);
	wr(dev, SHA_MODE, SHA_MODE_HMAC_SKS);
	if (!intel_csme_crypto_sks_valid(dev, key_slot)) {
		rc = -ENOENT;
		goto out;
	}
	wr(dev, SKS_STS, rd(dev, SKS_STS) | SKS_STS_ERR);
	wr(dev, SKS_SEL, key_slot);
	wr(dev, SKS_CMD, SKS_CMD_GO | SKS_LOAD_SHA);
	if ((sks_wait(dev) & SKS_STS_ERR) != 0U) {
		rc = -EIO;
		goto out;
	}
	wr(dev, SHA_CTRL, 1);
	for (i = 0; i + 4U <= len; i += 4U) {
		wr(dev, SHA_IN, sys_get_le32(data + i));
	}
	for (; i < len; i++) {
		sys_write8(data[i], cfg->regs + SHA_IN);
	}
	wr(dev, SHA_CTRL, 2);
	do {
		v = rd(dev, SHA_STS);
	} while ((v & SHA_STS_DONE) == 0U);
	wr(dev, SHA_STS, v);
	if (intel_csme_crypto_sks_valid(dev, dst_slot)) {
		rc = -EEXIST;
		goto out;
	}
	wr(dev, SKS_SEL, dst_slot);
	wr(dev, SKS_CMD, SKS_CMD_GO | SKS_STORE_HMAC);
	if ((sks_wait(dev) & SKS_STS_ERR) != 0U || !intel_csme_crypto_sks_valid(dev, dst_slot)) {
		rc = -EIO;
	}
out:
	(void)k_mutex_unlock(&d->lock);
	return rc;
}

static int aesp_block(struct cipher_ctx *ctx, struct cipher_pkt *pkt)
{
	const struct device *dev = ctx->device;
	uint8_t *out = pkt->out_buf != NULL ? pkt->out_buf : pkt->in_buf;
	uint32_t w[4];

	if (pkt->in_len != (int)AES_BLOCK ||
	    (pkt->out_buf != NULL && pkt->out_buf_max < (int)AES_BLOCK)) {
		return -EINVAL;
	}
	for (unsigned int i = 0; i < 4U; i++) {
		w[i] = sys_get_le32(pkt->in_buf + 4U * i);
	}
	/* drv_sessn_state: 1 for a decrypting session */
	wr(dev, AESP_MODE, (uintptr_t)ctx->drv_sessn_state != 0U ? AESP_MODE_DEC : 0U);
	wr(dev, AESP_CTRL, AESP_START);
	for (unsigned int i = 0; i < 4U; i++) {
		wr(dev, AESP_IN, w[i]);
	}
	wr(dev, AESP_CTRL, AESP_FINISH);
	while ((rd(dev, AESP_BUSY) & 1U) != 0U) {
	}
	for (unsigned int i = 0; i < 4U; i++) {
		sys_put_le32(rd(dev, AESP_OUT), out + 4U * i);
	}
	pkt->out_len = (int)AES_BLOCK;
	return 0;
}

static int crypto_query_caps(const struct device *dev)
{
	ARG_UNUSED(dev);

	return CAP_RAW_KEY | CAP_OPAQUE_KEY_HNDL | CAP_INPLACE_OPS | CAP_SEPARATE_IO_BUFS |
	       CAP_SYNC_OPS;
}

static int crypto_begin(const struct device *dev, struct cipher_ctx *ctx, enum cipher_algo algo,
			enum cipher_mode mode, enum cipher_op op)
{
	struct crypto_data *d = dev->data;

	if (algo != CRYPTO_CIPHER_ALGO_AES || mode != CRYPTO_CIPHER_MODE_ECB ||
	    (ctx->flags & CAP_SYNC_OPS) == 0U || (ctx->keylen != 16U && ctx->keylen != 32U)) {
		return -EINVAL;
	}
	(void)k_mutex_lock(&d->lock, K_FOREVER);
	if (d->in_use) {
		(void)k_mutex_unlock(&d->lock);
		return -EBUSY;
	}
	d->in_use = true;
	d->raw_key = (ctx->flags & CAP_OPAQUE_KEY_HNDL) == 0U;
	if (d->raw_key) {
		for (uint32_t i = 0; i < ctx->keylen; i += 4U) {
			wr(dev, AESP_KEY + i, sys_get_le32(ctx->key.bit_stream + i));
		}
		wr(dev, AESP_KEY_SIZE, ctx->keylen == 32U ? 1U : 0U);
	} else {
		sks_load_aesp(dev, (uint32_t)(uintptr_t)ctx->key.handle);
	}
	ctx->device = dev;
	ctx->drv_sessn_state = (void *)(uintptr_t)(op == CRYPTO_CIPHER_OP_DECRYPT ? 1U : 0U);
	ctx->ops.cipher_mode = mode;
	ctx->ops.block_crypt_hndlr = aesp_block;
	(void)k_mutex_unlock(&d->lock);
	return 0;
}

static int crypto_free(const struct device *dev, struct cipher_ctx *ctx)
{
	struct crypto_data *d = dev->data;

	(void)k_mutex_lock(&d->lock, K_FOREVER);
	if (d->raw_key) {
		/* the key registers do not keep a raw key past its session */
		for (uint32_t i = 0; i < ctx->keylen; i += 4U) {
			wr(dev, AESP_KEY + i, 0);
		}
		wr(dev, AESP_KEY_SIZE, 0);
	}
	d->in_use = false;
	(void)k_mutex_unlock(&d->lock);
	return 0;
}

static DEVICE_API(crypto, crypto_api) = {
	.query_hw_caps = crypto_query_caps,
	.cipher_begin_session = crypto_begin,
	.cipher_free_session = crypto_free,
};

static int crypto_init(const struct device *dev)
{
	struct crypto_data *d = dev->data;

	k_mutex_init(&d->lock);
	return 0;
}

#define CRYPTO_INIT(n)                                                                     \
	static const struct crypto_config crypto_config_##n = {                            \
		.regs = DT_INST_REG_ADDR(n),                                               \
	};                                                                                 \
	static struct crypto_data crypto_data_##n;                                         \
	DEVICE_DT_INST_DEFINE(n, crypto_init, NULL, &crypto_data_##n, &crypto_config_##n,  \
			      POST_KERNEL, CONFIG_CRYPTO_INIT_PRIORITY, &crypto_api);

DT_INST_FOREACH_STATUS_OKAY(CRYPTO_INIT)
