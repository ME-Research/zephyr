/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Intel CSME ATT sideband windows. Each window: +0x00 base, +0x04 size,
 * +0x08 bit 0 enable, +0x18 target (port | read opcode 6 << 8 | write
 * opcode 7 << 16 | flags << 24), +0x1c attribute; windows are 0x20 apart.
 * Agents are mapped on demand, a window whose range overlaps the new one is
 * released first, and otherwise the least recently used window is reused.
 */

#define DT_DRV_COMPAT intel_csme_att

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/drivers/misc/intel_csme_att/intel_csme_att.h>

#define WIN_BASE   0x00U
#define WIN_SIZE   0x04U
#define WIN_CTL    0x08U
#define WIN_RSVD0  0x10U
#define WIN_RSVD1  0x14U
#define WIN_TARGET 0x18U
#define WIN_ATTR   0x1cU
#define WIN_STRIDE 0x20U
#define WIN_ENABLE BIT(0)

#define SB_OPCODE_READ  0x06U
#define SB_OPCODE_WRITE 0x07U

#define MAX_WINDOWS 16

struct att_config {
	mm_reg_t sb_windows;
	uint8_t num_windows;
};

struct att_data {
	struct k_spinlock lock;
	const struct intel_csme_sb_target *mapped[MAX_WINDOWS];
	uint32_t last_use[MAX_WINDOWS];
	uint32_t clock;
};

static inline mm_reg_t win_reg(const struct att_config *cfg, unsigned int w, uint32_t reg)
{
	return cfg->sb_windows + w * WIN_STRIDE + reg;
}

static void att_unmap(const struct att_config *cfg, struct att_data *data, unsigned int w)
{
	sys_write32(0, win_reg(cfg, w, WIN_CTL));
	data->mapped[w] = NULL;
}

static bool same_target(const struct intel_csme_sb_target *a, const struct intel_csme_sb_target *b)
{
	return a->base == b->base && a->size == b->size && a->port == b->port &&
	       a->flags == b->flags && a->attr == b->attr;
}

static bool overlaps(const struct intel_csme_sb_target *a, const struct intel_csme_sb_target *b)
{
	return a->base < b->base + b->size && b->base < a->base + a->size;
}

/* Called with the lock held */
static mm_reg_t att_map(const struct device *dev, const struct intel_csme_sb_target *t)
{
	const struct att_config *cfg = dev->config;
	struct att_data *data = dev->data;
	int victim = -1;
	unsigned int w;
	uint32_t target;

	for (w = 0; w < cfg->num_windows; w++) {
		if (data->mapped[w] != NULL && same_target(data->mapped[w], t)) {
			data->last_use[w] = ++data->clock;
			return t->base;
		}
	}
	for (w = 0; w < cfg->num_windows; w++) {
		if (data->mapped[w] != NULL && overlaps(data->mapped[w], t)) {
			att_unmap(cfg, data, w);
		}
	}
	for (w = 0; w < cfg->num_windows; w++) {
		if (data->mapped[w] == NULL) {
			victim = w;
			break;
		}
		if (victim < 0 || data->last_use[w] < data->last_use[victim]) {
			victim = w;
		}
	}
	if (data->mapped[victim] != NULL) {
		att_unmap(cfg, data, victim);
	}

	sys_write32(t->base, win_reg(cfg, victim, WIN_BASE));
	sys_write32(t->size, win_reg(cfg, victim, WIN_SIZE));
	sys_write32(0, win_reg(cfg, victim, WIN_RSVD0));
	sys_write32(0, win_reg(cfg, victim, WIN_RSVD1));
	target = t->port | SB_OPCODE_READ << 8 | SB_OPCODE_WRITE << 16 | (uint32_t)t->flags << 24;
	sys_write32(target, win_reg(cfg, victim, WIN_TARGET));
	sys_write32(t->attr, win_reg(cfg, victim, WIN_ATTR));
	sys_write32(WIN_ENABLE, win_reg(cfg, victim, WIN_CTL));
	data->mapped[victim] = t;
	data->last_use[victim] = ++data->clock;
	return t->base;
}

uint32_t intel_csme_att_sb_read32(const struct device *att, const struct intel_csme_sb_target *t,
				  uint32_t off)
{
	struct att_data *data = att->data;
	k_spinlock_key_t key = k_spin_lock(&data->lock);
	uint32_t v = sys_read32(att_map(att, t) + off);

	k_spin_unlock(&data->lock, key);
	return v;
}

void intel_csme_att_sb_write32(const struct device *att, const struct intel_csme_sb_target *t,
			       uint32_t off, uint32_t val)
{
	struct att_data *data = att->data;
	k_spinlock_key_t key = k_spin_lock(&data->lock);

	sys_write32(val, att_map(att, t) + off);
	k_spin_unlock(&data->lock, key);
}

uint32_t intel_csme_att_sb_update32(const struct device *att,
				    const struct intel_csme_sb_target *t, uint32_t off,
				    uint32_t mask, uint32_t val)
{
	struct att_data *data = att->data;
	k_spinlock_key_t key = k_spin_lock(&data->lock);
	mm_reg_t a = att_map(att, t) + off;
	uint32_t old = sys_read32(a);

	sys_write32((old & ~mask) | (val & mask), a);
	k_spin_unlock(&data->lock, key);
	return old;
}

static int att_init(const struct device *dev)
{
	const struct att_config *cfg = dev->config;
	struct att_data *data = dev->data;

	/* the boot ROM leaves no window this driver knows about */
	for (unsigned int w = 0; w < cfg->num_windows; w++) {
		if ((sys_read32(win_reg(cfg, w, WIN_CTL)) & WIN_ENABLE) != 0U) {
			att_unmap(cfg, data, w);
		}
	}
	return 0;
}

#define ATT_INIT(n)                                                                        \
	BUILD_ASSERT(DT_INST_PROP(n, sideband_windows) <= MAX_WINDOWS);                    \
	static const struct att_config att_config_##n = {                                  \
		.sb_windows = DT_INST_REG_ADDR_BY_NAME(n, sideband),                       \
		.num_windows = DT_INST_PROP(n, sideband_windows),                          \
	};                                                                                 \
	static struct att_data att_data_##n;                                               \
	DEVICE_DT_INST_DEFINE(n, att_init, NULL, &att_data_##n, &att_config_##n,           \
			      PRE_KERNEL_1, CONFIG_INTEL_CSME_ATT_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(ATT_INIT)
