/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Intel CSME crypto engine: secure key storage (SKS)
 *
 * The engine's AES-P unit is a crypto API cipher (AES-ECB, synchronous).
 * A session's key is raw (CAP_RAW_KEY) or an SKS slot (CAP_OPAQUE_KEY_HNDL,
 * the slot number in cipher_ctx.key.handle, see INTEL_CSME_SKS_KEY()). The
 * functions below manage SKS slots, whose keys software never sees.
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_CRYPTO_INTEL_CSME_CRYPTO_H_
#define ZEPHYR_INCLUDE_DRIVERS_CRYPTO_INTEL_CSME_CRYPTO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The cipher_ctx.key.handle of SKS slot n */
#define INTEL_CSME_SKS_KEY(n) ((void *)(uintptr_t)(n))

/**
 * @brief Whether an SKS slot holds a key
 *
 * @param dev Crypto engine
 * @param slot SKS slot
 */
bool intel_csme_crypto_sks_valid(const struct device *dev, uint32_t slot);

/**
 * @brief Derive a key into an SKS slot: HMAC-SHA-256 of data under a slot
 *
 * The SHA unit takes the key from key_slot; its result goes to dst_slot,
 * which must be empty.
 *
 * @param dev Crypto engine
 * @param key_slot SKS slot of the HMAC key
 * @param data Message
 * @param len Message length in bytes
 * @param dst_slot SKS slot for the result
 *
 * @retval 0 Stored
 * @retval -ENOENT key_slot is empty
 * @retval -EEXIST dst_slot is not empty
 * @retval -EIO The SKS reported an error
 */
int intel_csme_crypto_sks_hmac_store(const struct device *dev, uint32_t key_slot,
				     const uint8_t *data, size_t len, uint32_t dst_slot);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_CRYPTO_INTEL_CSME_CRYPTO_H_ */
