/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Intel PCH SPI flash controller as the CSME drives it: PCI function
 * 0:19.0 of the CSME root space, registers in BAR0 (the boot ROM programs
 * it). Reads, writes and erases are hardware-sequenced cycles of up to 64
 * bytes through FDATA; RPMC commands are software-sequenced. Offsets are
 * linear flash addresses (the descriptor at 0).
 *
 * At init the driver sets up the controller the way the CSME firmware
 * does: the opcode menu, the regions, the components from the descriptor,
 * each component's VSCC (from its SFDP or the descriptor's VSCC table),
 * the block-protect bits cleared, and an RPMC found through SFDP.
 *
 * The register layout comes from reverse engineering the CSME firmware.
 */

#define DT_DRV_COMPAT intel_pch_spi_flash

#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/flash/intel_pch_spi_flash_api_ex.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/sys_io.h>

LOG_MODULE_REGISTER(flash_intel_pch_spi, CONFIG_FLASH_LOG_LEVEL);

#define HSFSTS_CTL   0x04U
#define FADDR        0x08U
#define FDATA0       0x10U
#define FRAP         0x50U
#define FREG0        0x54U
#define SSFSTS_CTL   0xa0U
#define PREOP_OPTYPE 0xa4U
#define OPMENU0      0xa8U
#define OPMENU1      0xacU
#define VSCC0        0xc4U
#define VSCC1        0xc8U
#define PTINX        0xccU
#define PTDATA       0xd0U

#define FRAP_HOST_ME_READ  BIT(17)
#define FRAP_HOST_ME_WRITE BIT(25)

/* hardware sequencing cycles (FCYCLE << 1) */
#define OP_READ         0x00U
#define OP_WRITE        0x04U
#define OP_ERASE_4K     0x06U
#define OP_READ_SFDP    0x0aU
#define OP_READ_JEDEC   0x0cU
#define OP_WRITE_STATUS 0x0eU
#define OP_READ_STATUS  0x10U

#define MAX_XFER       0x40U
#define PAGE_SIZE      0x1000U
#define NUM_REGIONS    9U
#define SFDP_SIGNATURE 0x50444653U /* "SFDP" */

struct component {
	uint32_t size_kib;
	uint32_t jedec;
	uint8_t wp_type;
	bool vscc_fixup;
};

struct spi_config {
	mm_reg_t regs;
};

struct spi_data {
	struct k_mutex lock;
	struct component comp[2];
	uint32_t ncomp;
	uint32_t vscc[2];
	uint32_t freg[NUM_REGIONS][2];
	struct intel_pch_spi_rpmc_info rpmc;
	struct flash_pages_layout layout;
	uint8_t opmenu[8];
};

static const struct flash_parameters spi_flash_params = {
	.write_block_size = 1,
	.erase_value = 0xff,
};

static inline uint32_t rd(const struct device *dev, uint32_t off)
{
	const struct spi_config *cfg = dev->config;

	return sys_read32(cfg->regs + off);
}

static inline void wr(const struct device *dev, uint32_t off, uint32_t v)
{
	const struct spi_config *cfg = dev->config;

	sys_write32(v, cfg->regs + off);
}

/* FDATA: dwords, then bytes */
static void fdata_put(const struct device *dev, const uint8_t *src, uint32_t n)
{
	const struct spi_config *cfg = dev->config;
	uint32_t off = 0;

	for (; n - off >= 4U; off += 4U) {
		wr(dev, FDATA0 + off, sys_get_le32(src + off));
	}
	for (; off < n; off++) {
		sys_write8(src[off], cfg->regs + FDATA0 + off);
	}
}

static void fdata_get(const struct device *dev, uint8_t *dst, uint32_t n)
{
	const struct spi_config *cfg = dev->config;
	uint32_t off = 0;

	for (; n - off >= 4U; off += 4U) {
		sys_put_le32(rd(dev, FDATA0 + off), dst + off);
	}
	for (; off < n; off++) {
		dst[off] = sys_read8(cfg->regs + FDATA0 + off);
	}
}

static bool op_reads(uint8_t op)
{
	return op == OP_READ || op == OP_READ_SFDP || op == OP_READ_JEDEC || op == OP_READ_STATUS;
}

static bool op_writes(uint8_t op)
{
	return op == OP_WRITE || op == OP_WRITE_STATUS;
}

/* One hardware-sequenced operation, split into cycles of at most 64 bytes
 * that do not cross a 4 KiB page, nor a 256-byte page for writes on a part
 * whose VSCC asks for it.
 */
static int hwseq(const struct device *dev, uint8_t op, uint32_t addr, uint32_t len, void *buf)
{
	struct spi_data *data = dev->data;
	uint8_t *p = buf;
	uint32_t done = 0, sts, n;
	int rc = 0;

	if (addr + len < len) {
		return -EINVAL;
	}
	while (done < len && rc == 0) {
		uint32_t off = addr & (PAGE_SIZE - 1U);
		unsigned int ci = ((data->comp[0].size_kib << 10) <= addr) ? 1U : 0U;

		n = MIN(len - done, (off + MAX_XFER <= PAGE_SIZE) ? MAX_XFER : PAGE_SIZE - off);
		wr(dev, FADDR, addr);
		if ((data->vscc[ci] & 4U) != 0U && (addr >> 8) != ((addr + n - 1U) >> 8)) {
			n -= (addr + n) & 0xffU;
		}
		if (op_writes(op)) {
			fdata_put(dev, p, n);
		}
		sts = rd(dev, HSFSTS_CTL);
		wr(dev, HSFSTS_CTL, sts | 0x15fU);
		wr(dev, HSFSTS_CTL,
		   ((uint32_t)((op | 1U) | ((n - 1U) << 8)) << 16) | (sts & 0xffffU) | 0x15fU);
		do {
			sts = rd(dev, HSFSTS_CTL);
		} while ((sts & 0x14fU) == 0U);
		if ((sts & 4U) != 0U) {
			rc = -EACCES;
		}
		if ((sts & 0x14aU) != 0U) {
			rc = -EIO;
		}
		if (rc == 0) {
			if (op_reads(op)) {
				fdata_get(dev, p, n);
			}
			done += n;
		}
		addr += n;
		p += n;
	}
	sts = rd(dev, HSFSTS_CTL);
	wr(dev, HSFSTS_CTL, sts | 0x15fU);
	return rc;
}

/* One software-sequenced cycle: opcode menu index in cop, control bits in
 * ctl_hi (data byte count - 1, data phase)
 */
static int swseq(const struct device *dev, uint8_t cop, uint16_t ctl_hi, uint32_t addr,
		 bool write, uint8_t len, void *buf)
{
	uint32_t v;

	wr(dev, SSFSTS_CTL, 0xcU);
	wr(dev, FADDR, addr);
	if (len != 0U && write) {
		fdata_put(dev, buf, len);
	}
	v = (((uint32_t)ctl_hi << 8 | cop) | 0x3c0000U) & 0xffffffU;
	wr(dev, SSFSTS_CTL, v << 8 | 0x200U);
	do {
		v = rd(dev, SSFSTS_CTL);
	} while ((v & 0xcU) == 0U);
	if ((v & 0x18U) != 0U) {
		return -EIO;
	}
	if (!write && len != 0U) {
		fdata_get(dev, buf, len);
	}
	return 0;
}

static uint64_t flash_size(const struct spi_data *data)
{
	uint64_t s = 0;

	for (uint32_t c = 0; c < data->ncomp; c++) {
		s += (uint64_t)data->comp[c].size_kib << 10;
	}
	return s;
}

static int spi_read(const struct device *dev, off_t offset, void *buf, size_t len)
{
	struct spi_data *data = dev->data;
	int rc;

	if (len == 0U) {
		return 0;
	}
	(void)k_mutex_lock(&data->lock, K_FOREVER);
	rc = hwseq(dev, OP_READ, (uint32_t)offset, len, buf);
	(void)k_mutex_unlock(&data->lock);
	return rc;
}

static int spi_write(const struct device *dev, off_t offset, const void *buf, size_t len)
{
	struct spi_data *data = dev->data;
	int rc;

	if (len == 0U) {
		return 0;
	}
	(void)k_mutex_lock(&data->lock, K_FOREVER);
	rc = hwseq(dev, OP_WRITE, (uint32_t)offset, len, (void *)buf);
	(void)k_mutex_unlock(&data->lock);
	return rc;
}

static int spi_erase(const struct device *dev, off_t offset, size_t size)
{
	struct spi_data *data = dev->data;
	int rc = 0;

	if ((offset % PAGE_SIZE) != 0 || (size % PAGE_SIZE) != 0U) {
		return -EINVAL;
	}
	(void)k_mutex_lock(&data->lock, K_FOREVER);
	for (size_t i = 0; i < size && rc == 0; i += PAGE_SIZE) {
		rc = hwseq(dev, OP_ERASE_4K, (uint32_t)offset + i, 1, NULL);
	}
	(void)k_mutex_unlock(&data->lock);
	return rc;
}

static const struct flash_parameters *spi_get_parameters(const struct device *dev)
{
	ARG_UNUSED(dev);
	return &spi_flash_params;
}

static int spi_get_size(const struct device *dev, uint64_t *size)
{
	*size = flash_size(dev->data);
	return 0;
}

#if defined(CONFIG_FLASH_PAGE_LAYOUT)
static void spi_page_layout(const struct device *dev, const struct flash_pages_layout **layout,
			    size_t *layout_size)
{
	struct spi_data *data = dev->data;

	*layout = &data->layout;
	*layout_size = 1;
}
#endif

/* a dword of component c's SFDP space */
static int sfdp_dw(const struct device *dev, uint32_t c, uint32_t addr, uint32_t *out)
{
	struct spi_data *data = dev->data;
	uint32_t base = c != 0U ? data->comp[c - 1U].size_kib << 10 : 0U;
	uint8_t b[4];
	int rc = hwseq(dev, OP_READ_SFDP, base + addr, 4, b);

	*out = sys_get_le32(b);
	return rc;
}

#if defined(CONFIG_FLASH_JESD216_API)
static int spi_sfdp_read(const struct device *dev, off_t offset, void *buf, size_t len)
{
	struct spi_data *data = dev->data;
	int rc;

	(void)k_mutex_lock(&data->lock, K_FOREVER);
	rc = hwseq(dev, OP_READ_SFDP, (uint32_t)offset, len, buf);
	(void)k_mutex_unlock(&data->lock);
	return rc;
}

static int spi_read_jedec_id(const struct device *dev, uint8_t *id)
{
	struct spi_data *data = dev->data;

	id[0] = (uint8_t)data->comp[0].jedec;
	id[1] = (uint8_t)(data->comp[0].jedec >> 8);
	id[2] = (uint8_t)(data->comp[0].jedec >> 16);
	return 0;
}
#endif

/* sfdp_rpmc_decode_delay: bits 0-4 value, bits 5-6 unit (1/16/128/1000), x20 */
static uint32_t rpmc_delay(uint32_t v)
{
	static const uint32_t unit[4] = {1, 16, 128, 1000};

	return ((v & 0x1fU) + 1U) * unit[(v >> 5) & 3U] * 20U;
}

static int rpmc_transact(const struct device *dev, const struct intel_pch_spi_rpmc_cmd *cmd,
			 uint8_t *resp)
{
	struct spi_data *data = dev->data;
	const struct intel_pch_spi_rpmc_info *r = &data->rpmc;
	uint32_t tmo;
	int rc;

	if (cmd == NULL || cmd->data == NULL || cmd->len == 0U || resp == NULL) {
		return -EINVAL;
	}
	if (!r->present) {
		return -ENODEV;
	}
	switch (cmd->data[0]) {
	case 0:
	case 2:
		tmo = r->timeout_counter_us;
		break;
	case 1:
	case 3:
		tmo = r->timeout_other_us;
		break;
	default:
		return -EINVAL;
	}
	/* OPMENU 4 = OP1, 5 = OP2 */
	rc = swseq(dev, r->op1_polls ? 0x4cU : 0x40U, (uint16_t)((cmd->len - 1U) | 0x40U),
		   r->base, true, cmd->len, (void *)cmd->data);
	if (rc != 0) {
		return rc;
	}
	if (!r->op1_polls) {
		uint8_t st[2] = {0, 0};
		int64_t t0 = k_uptime_ticks();

		do {
			rc = swseq(dev, 0x50, 0x41, r->base, false, 2, st);
			if (rc != 0) {
				return rc;
			}
			if ((st[1] & 1U) == 0U) {
				break;
			}
			if (k_ticks_to_us_floor64(k_uptime_ticks() - t0) > tmo) {
				return -ETIMEDOUT;
			}
			k_usleep(100);
		} while (true);
	}
	return swseq(dev, 0x50, 0x71, r->base, false, INTEL_PCH_SPI_RPMC_RESPONSE, resp);
}

#if defined(CONFIG_FLASH_EX_OP_ENABLED)
static int spi_ex_op(const struct device *dev, uint16_t code, const uintptr_t in, void *out)
{
	struct spi_data *data = dev->data;
	int rc = 0;

	(void)k_mutex_lock(&data->lock, K_FOREVER);
	switch (code) {
	case FLASH_INTEL_PCH_SPI_EX_OP_REGION:
		if (in >= NUM_REGIONS || out == NULL) {
			rc = -EINVAL;
			break;
		}
		((struct intel_pch_spi_region *)out)->base = data->freg[in][0];
		((struct intel_pch_spi_region *)out)->limit = data->freg[in][1];
		break;
	case FLASH_INTEL_PCH_SPI_EX_OP_STATUS:
		*(uint32_t *)out = rd(dev, HSFSTS_CTL);
		break;
	case FLASH_INTEL_PCH_SPI_EX_OP_LOCKDOWN:
		wr(dev, HSFSTS_CTL, rd(dev, HSFSTS_CTL) | INTEL_PCH_SPI_HSFSTS_FLOCKDN);
		break;
	case FLASH_INTEL_PCH_SPI_EX_OP_HOST_ME_ACCESS: {
		uint32_t v = rd(dev, FRAP);

		v = (in & 1U) != 0U ? (v | FRAP_HOST_ME_READ) : (v & ~FRAP_HOST_ME_READ);
		v = (in & 2U) != 0U ? (v | FRAP_HOST_ME_WRITE) : (v & ~FRAP_HOST_ME_WRITE);
		wr(dev, FRAP, v);
		break;
	}
	case FLASH_INTEL_PCH_SPI_EX_OP_RPMC_INFO:
		*(struct intel_pch_spi_rpmc_info *)out = data->rpmc;
		break;
	case FLASH_INTEL_PCH_SPI_EX_OP_RPMC_TRANSACT:
		rc = rpmc_transact(dev, (const struct intel_pch_spi_rpmc_cmd *)in, out);
		break;
	default:
		rc = -ENOTSUP;
		break;
	}
	(void)k_mutex_unlock(&data->lock);
	return rc;
}
#endif

static DEVICE_API(flash, spi_api) = {
	.read = spi_read,
	.write = spi_write,
	.erase = spi_erase,
	.get_parameters = spi_get_parameters,
	.get_size = spi_get_size,
#if defined(CONFIG_FLASH_PAGE_LAYOUT)
	.page_layout = spi_page_layout,
#endif
#if defined(CONFIG_FLASH_JESD216_API)
	.sfdp_read = spi_sfdp_read,
	.read_jedec_id = spi_read_jedec_id,
#endif
#if defined(CONFIG_FLASH_EX_OP_ENABLED)
	.ex_op = spi_ex_op,
#endif
};

static void write_opmenu(const struct device *dev)
{
	struct spi_data *data = dev->data;

	wr(dev, OPMENU0, sys_get_le32(&data->opmenu[0]));
	wr(dev, OPMENU1, sys_get_le32(&data->opmenu[4]));
}

/* The VSCC of a JEDEC ID in the descriptor's VSCC table: {JEDEC ID, VSCC}
 * pairs at (vtl & 0xff) << 4, (vtl >> 8) & 0xff dwords long, read 32 bytes
 * at a time. 1 found, 0 not found.
 */
static int vscc_table_lookup(const struct device *dev, uint32_t vtl, uint32_t jedec,
			     uint32_t *vscc)
{
	uint32_t buf[8];
	uint32_t tlen = ((vtl >> 8) & 0xffU) * 4U;
	uint32_t a = (vtl & 0xffU) << 4;

	for (uint32_t done = 0; done < tlen; done += sizeof(buf)) {
		uint32_t n = MIN(tlen - done, sizeof(buf));

		if (hwseq(dev, OP_READ, a + done, n, buf) != 0) {
			LOG_ERR("cannot read the VSCC table");
			return -EIO;
		}
		for (uint32_t i = 0; i + 1U < n / 4U; i += 2U) {
			if (buf[i] == jedec) {
				*vscc = buf[i + 1U];
				return 1;
			}
		}
	}
	return 0;
}

/* the components' VSCC: from the registers (programmed from SFDP) or from
 * the descriptor's VSCC table ({JEDEC ID, VSCC} pairs at its 0xefc pointer)
 */
static int setup_vscc(const struct device *dev, bool desc_readable)
{
	struct spi_data *data = dev->data;
	uint32_t vscc[2] = {UINT32_MAX, UINT32_MAX};
	bool legacy[2] = {true, true};
	uint32_t found = 0, v;
	bool table = false;

	for (uint32_t c = 0; c < data->ncomp; c++) {
		v = rd(dev, c == 0U ? VSCC0 : VSCC1);
		if ((int32_t)v < 0) {
			uint32_t d;

			vscc[c] = v;
			wr(dev, PTINX, (c & 3U) << 14 | 0x1000U);
			d = rd(dev, PTDATA);
			if ((d & 0xff0000U) <= 0xffU ||
			    ((d & 0xff0000U) <= 0x1ffffU && (d & 0xff00U) <= 0x4ffU)) {
				legacy[c] = false;
			} else {
				found++;
			}
		}
	}
	if (found < data->ncomp) {
		uint32_t vtl = 0;

		if (desc_readable) {
			(void)hwseq(dev, OP_READ, 0xefc, 4, &vtl);
		}
		if (desc_readable && (vtl & 0xffffU) != 0xffffU && ((vtl >> 8) & 0xffU) != 0U) {
			for (uint32_t c = 0; c < data->ncomp; c++) {
				uint32_t hit;
				int rc;

				if (vscc[c] != UINT32_MAX && legacy[c]) {
					continue;
				}
				rc = vscc_table_lookup(dev, vtl, data->comp[c].jedec, &hit);
				if (rc < 0) {
					return rc;
				}
				if (rc == 1) {
					vscc[c] = vscc[c] == UINT32_MAX ? hit :
						  (vscc[c] & 0xffffff1fU) | (hit & 0xe0U);
					found++;
				}
			}
			table = true;
		}
	}
	if (found < data->ncomp) {
		for (uint32_t c = 0; c < data->ncomp; c++) {
			if (vscc[c] != UINT32_MAX && !legacy[c]) {
				found++;
				vscc[c] &= 0xffffff3fU;
				data->comp[c].vscc_fixup = true;
			}
		}
	}
	if (found != data->ncomp) {
		LOG_ERR("no VSCC for every component (%s)", table ? "table" : "registers");
		return -EIO;
	}
	for (uint32_t c = 0; c < found; c++) {
		uint32_t t = vscc[c] & 0xe0U;

		data->comp[c].wp_type = (t == 0x20U || t == 0x40U || t == 0x60U || t == 0x80U ||
					 t == 0xa0U) ? (uint8_t)t : 0U;
	}
	data->vscc[0] = vscc[0];
	data->vscc[1] = found == 1U ? 0U : vscc[1];
	for (unsigned int i = 0; i < 2U; i++) {
		uint32_t reg = i == 0U ? VSCC0 : VSCC1;

		if ((int32_t)rd(dev, reg) >= 0) {
			wr(dev, reg, data->vscc[i]);
		}
	}
	return 0;
}

/* clear the block-protect bits in each component's status register(s) */
static void clear_block_protect(const struct device *dev)
{
	struct spi_data *data = dev->data;

	for (uint32_t c = 0; c < data->ncomp; c++) {
		uint32_t base = c != 0U ? data->comp[c - 1U].size_kib << 10 : 0U;
		unsigned int ci = ((data->comp[0].size_kib << 10) <= base) ? 1U : 0U;
		uint8_t s[2] = {0xff, 0xff};

		if (hwseq(dev, OP_READ_STATUS, base, 1, &s[0]) == 0 &&
		    data->comp[ci].wp_type == 0x20U) {
			/* OPMENU 7 (0x35): status register 2 */
			(void)swseq(dev, 0x70, 0x40, base, false, 1, &s[1]);
		}
		if ((s[0] & 0x3cU) != 0U) {
			uint8_t w[2] = {0, 0};
			uint32_t wl = 1;

			if (data->comp[base != 0U ? 1 : 0].vscc_fixup) {
				w[0] = s[0] & 0xc3U;
			}
			switch (data->comp[ci].wp_type) {
			case 0x40:
				w[0] |= s[0] & 0x40U;
				break;
			case 0x60:
				w[0] |= s[0] & 0x80U;
				break;
			case 0x20:
				w[1] = s[1] & 2U;
				wl = 2;
				break;
			default:
				break;
			}
			(void)hwseq(dev, OP_WRITE_STATUS, base, wl, w);
		}
	}
}

/* SFDP: the RPMC parameter table (ID LSB 0x03) of the first component with one */
static void find_rpmc(const struct device *dev)
{
	struct spi_data *data = dev->data;

	for (uint32_t c = 0; c < data->ncomp; c++) {
		uint32_t d0, d1, dw1, nph, i = 0, off = 0;
		bool found = false;

		(void)sfdp_dw(dev, c, 0, &d0);
		if (d0 != SFDP_SIGNATURE) {
			continue;
		}
		(void)sfdp_dw(dev, c, 4, &d0);
		if (((d0 >> 8) & 0xffU) > 1U) {
			continue;
		}
		nph = ((d0 >> 16) & 0xffU) + 1U;
		do {
			i++;
			off = i * 8U;
			(void)sfdp_dw(dev, c, off, &d0);
			if ((d0 & 0xffU) == 3U) {
				found = true;
				break;
			}
		} while (i < nph);
		if (!found) {
			continue;
		}
		(void)sfdp_dw(dev, c, off + 4U, &d1);
		d1 &= 0xffffffU;
		(void)sfdp_dw(dev, c, d1, &dw1);
		if ((dw1 & 1U) != 0U) {
			continue;
		}
		data->rpmc.present = true;
		data->rpmc.component = (uint8_t)c;
		data->rpmc.update_rate = 5U << ((dw1 >> 24) & 0xfU);
		data->rpmc.op1 = (dw1 >> 8) & 0xffU;
		data->rpmc.op2 = (dw1 >> 16) & 0xffU;
		data->rpmc.counters = (uint8_t)(((dw1 & 0xffU) >> 4) + 1U);
		data->rpmc.op1_polls = ((dw1 >> 2) & 1U) != 0U;
		data->rpmc.base = c != 0U ? data->comp[c - 1U].size_kib << 10 : 0U;
		(void)sfdp_dw(dev, c, d1 + 4U, &d0);
		data->rpmc.timeout_counter_us = rpmc_delay((d0 >> 16) & 0xffU) * 1000U;
		data->rpmc.timeout_other_us = rpmc_delay(d0 & 0xffU);
		/* OPMENU 4/5 = OP1/OP2, prefix opcode 1 = OP1 */
		data->opmenu[4] = data->rpmc.op1;
		data->opmenu[5] = data->rpmc.op2;
		write_opmenu(dev);
		wr(dev, PREOP_OPTYPE, 0x01040006U | (uint32_t)data->rpmc.op1 << 8);
		wr(dev, VSCC0, rd(dev, VSCC0) | 0x8000000U);
		break;
	}
}

static int spi_init(const struct device *dev)
{
	struct spi_data *data = dev->data;
	static const uint8_t opmenu[8] = {0x9f, 0x01, 0x05, 0, 0, 0, 0, 0x35};
	uint32_t flmap0 = 0, flcomp = 0, ptdata, v;
	bool pt_flag, desc_readable;
	int rc;

	k_mutex_init(&data->lock);
	memcpy(data->opmenu, opmenu, sizeof(opmenu));
	write_opmenu(dev);
	wr(dev, PREOP_OPTYPE, 0x40006U);

	for (unsigned int i = 0; i < NUM_REGIONS; i++) {
		v = rd(dev, FREG0 + 4U * i);
		data->freg[i][0] = (v & 0x7fffU) << 12;
		data->freg[i][1] = (v & 0x7fff0000U) >> 4;
		if ((v & 0x7fffU) == 0x7fffU && data->freg[i][1] == 0U) {
			data->freg[i][0] = UINT32_MAX;
		} else {
			data->freg[i][1] |= 0xfffU;
		}
	}
	/* the CSME must be able to read the descriptor (BRRA bit 0) */
	desc_readable = (rd(dev, FRAP) & 1U) != 0U;
	if (!desc_readable) {
		LOG_ERR("descriptor not readable");
		return -EACCES;
	}

	(void)hwseq(dev, OP_READ, 0x14, 4, &flmap0);
	(void)hwseq(dev, OP_READ, (flmap0 & 0xffU) << 4, 4, &flcomp);
	data->ncomp = ((flmap0 >> 8) & 3U) + 1U;
	if (data->ncomp > 2U) {
		LOG_ERR("%u flash components", data->ncomp);
		return -EIO;
	}

	v = rd(dev, PTINX);
	wr(dev, PTINX, (v & 0xffffc003U) | 0x2000U);
	ptdata = rd(dev, PTDATA);
	pt_flag = (((ptdata & 0x60000U) >> 17) - 1U) <= 1U;

	for (uint32_t c = 0; c < data->ncomp; c++) {
		uint32_t base = c == 0U ? 0U : 0x200U << (flcomp & 0xfU);
		uint32_t dens = c == 0U ? flcomp & 0xfU : (flcomp & 0xffU) >> 4;
		uint8_t id[3];

		if (dens > 7U) {
			LOG_ERR("component %u density %u", c, dens);
			return -EIO;
		}
		data->comp[c].size_kib = 0x200U << dens;
		v = rd(dev, c == 0U ? VSCC0 : VSCC1);
		if ((int32_t)v < 0) {
			v = rd(dev, PTINX);
			wr(dev, PTINX, (v & 0xffff3fffU) | (c & 3U) << 14);
		}
		(void)hwseq(dev, OP_READ_JEDEC, base << 10, 3, id);
		data->comp[c].jedec = id[0] | id[1] << 8 | (uint32_t)id[2] << 16;
	}
	if (data->ncomp == 2U && data->comp[0].size_kib > 0x4000U && !pt_flag) {
		LOG_ERR("component 0 too large without parameter tables");
		return -EIO;
	}

	rc = setup_vscc(dev, desc_readable);
	if (rc != 0) {
		return rc;
	}
	clear_block_protect(dev);
	find_rpmc(dev);
	/* drop the status-write opcode (index 1) */
	data->opmenu[1] = 0;
	write_opmenu(dev);

	data->layout.pages_size = PAGE_SIZE;
	data->layout.pages_count = flash_size(data) / PAGE_SIZE;
	return 0;
}

#define SPI_INIT(n)                                                                        \
	static const struct spi_config spi_config_##n = {                                  \
		.regs = DT_INST_REG_ADDR(n),                                               \
	};                                                                                 \
	static struct spi_data spi_data_##n;                                               \
	DEVICE_DT_INST_DEFINE(n, spi_init, NULL, &spi_data_##n, &spi_config_##n,           \
			      POST_KERNEL, CONFIG_FLASH_INIT_PRIORITY, &spi_api);

DT_INST_FOREACH_STATUS_OKAY(SPI_INIT)
