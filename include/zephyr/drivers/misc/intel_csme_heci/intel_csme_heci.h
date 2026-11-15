/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Intel CSME HECI functions, engine side: firmware status and CSR
 *
 * The host reads the CSME's state from the firmware status registers
 * (HFSTS1-6) of the HECI functions; the CSME writes them. The register
 * layout comes from reverse engineering the CSME firmware.
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MISC_INTEL_CSME_HECI_H_
#define ZEPHYR_INCLUDE_DRIVERS_MISC_INTEL_CSME_HECI_H_

#include <stdint.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Read firmware status register HFSTSn
 *
 * @param dev HECI device
 * @param n 1 to 6
 */
uint32_t intel_csme_heci_fwsts_read(const struct device *dev, unsigned int n);

/**
 * @brief Read-modify-write firmware status register HFSTSn
 *
 * @param dev HECI device
 * @param n 1 to 6
 * @param mask Bits to change
 * @param val New value of those bits
 * @return The value before the update
 */
uint32_t intel_csme_heci_fwsts_update(const struct device *dev, unsigned int n, uint32_t mask,
				      uint32_t val);

/** Write firmware status register HFSTSn (1 to 6) whole */
void intel_csme_heci_fwsts_write(const struct device *dev, unsigned int n, uint32_t val);

/**
 * @brief Read-modify-write the engine's control and status register (ME_CSR)
 *
 * @return The value before the update
 */
uint32_t intel_csme_heci_csr_update(const struct device *dev, uint32_t mask, uint32_t val);

/**
 * @brief Request host memory (UMA) for the CSME
 *
 * The BIOS reads the request from HECI1 during memory initialisation.
 *
 * @param dev HECI device
 * @param mib Size in MiB (0 to 127)
 */
void intel_csme_heci_uma_request(const struct device *dev, uint32_t mib);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MISC_INTEL_CSME_HECI_H_ */
