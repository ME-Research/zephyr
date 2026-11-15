/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Intel PCH fuse controller, CSME view: extensions to the OTP API
 *
 * otp_read() reads the fuse array as the controller lays it out: the
 * shadow copy (what the hardware uses) at 0x000-0x3ff, 0x80 bytes per bank,
 * and the copy sensed from the fuses at 0x400-0x7ff. The controller is
 * taken for the duration of each call.
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_OTP_INTEL_PCH_FUSE_H_
#define ZEPHYR_INCLUDE_DRIVERS_OTP_INTEL_PCH_FUSE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Offset of the shadow copy of a fuse bank in the OTP address space */
#define INTEL_PCH_FUSE_SHADOW(bank) ((bank) * 0x80U)
/** Offset of the sensed copy of a fuse bank in the OTP address space */
#define INTEL_PCH_FUSE_SENSED(bank) (0x400U + (bank) * 0x80U)

/**
 * @brief Read a fuse bank sensed at a margin
 *
 * Senses the bank again at a shifted read threshold, to find weakly blown
 * fuses: the CSME firmware compares the normal read with margin 0 and 2.
 *
 * @param dev Fuse controller
 * @param margin Margin mode: 0 or 2 (others sense at the normal level)
 * @param bank Fuse bank
 * @param sel Sense selector
 * @param selftest Check a known fuse word before and after (some PCH
 *                 revisions)
 * @param words 32 words of sensed data
 *
 * @retval 0 Read
 * @retval -ETIMEDOUT The controller could not be taken
 * @retval -EIO The sense or the self test failed
 */
int intel_pch_fuse_margin_read(const struct device *dev, int margin, unsigned int bank,
			       unsigned int sel, bool selftest, uint32_t words[32]);

/**
 * @brief Write words of the shadow copy
 *
 * The hardware uses the shadow copy; the fuses themselves are unchanged.
 *
 * @param dev Fuse controller
 * @param offset Byte offset in the shadow (INTEL_PCH_FUSE_SHADOW()), aligned
 * @param words Data
 * @param n Number of words
 *
 * @retval 0 Written
 * @retval -ETIMEDOUT The controller could not be taken
 */
int intel_pch_fuse_shadow_write(const struct device *dev, uint32_t offset,
				const uint32_t *words, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_OTP_INTEL_PCH_FUSE_H_ */
