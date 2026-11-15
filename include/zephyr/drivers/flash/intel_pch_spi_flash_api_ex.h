/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Intel PCH SPI flash controller (CSME view): extended operations
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_FLASH_INTEL_PCH_SPI_FLASH_API_EX_H_
#define ZEPHYR_INCLUDE_DRIVERS_FLASH_INTEL_PCH_SPI_FLASH_API_EX_H_

#include <stdint.h>
#include <zephyr/drivers/flash.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Extended operations of the Intel PCH SPI flash controller */
enum flash_intel_pch_spi_ex_ops {
	/**
	 * A flash descriptor region.
	 *
	 * @param in Region number (FREGn), 0 to 8
	 * @param out struct intel_pch_spi_region *
	 */
	FLASH_INTEL_PCH_SPI_EX_OP_REGION = FLASH_EX_OP_VENDOR_BASE,
	/**
	 * Hardware sequencing status (HSFSTS), e.g. the descriptor override
	 * strap (INTEL_PCH_SPI_HSFSTS_FDOPSS clear when asserted).
	 *
	 * @param out uint32_t *
	 */
	FLASH_INTEL_PCH_SPI_EX_OP_STATUS,
	/** Lock the controller's configuration down (FLOCKDN) */
	FLASH_INTEL_PCH_SPI_EX_OP_LOCKDOWN,
	/**
	 * Grant the host read/write access to the CSME's region (HMRFPO).
	 *
	 * @param in Bit 0 read, bit 1 write
	 */
	FLASH_INTEL_PCH_SPI_EX_OP_HOST_ME_ACCESS,
	/**
	 * The replay-protected monotonic counter (RPMC) a component offers,
	 * from its SFDP tables.
	 *
	 * @param out struct intel_pch_spi_rpmc_info *
	 */
	FLASH_INTEL_PCH_SPI_EX_OP_RPMC_INFO,
	/**
	 * One RPMC command: OP1 with the command, the busy poll through OP2
	 * (or none, with OP1 polling), and the OP2 response.
	 *
	 * @param in const struct intel_pch_spi_rpmc_cmd *
	 * @param out uint8_t[INTEL_PCH_SPI_RPMC_RESPONSE] response, byte 1 the status
	 */
	FLASH_INTEL_PCH_SPI_EX_OP_RPMC_TRANSACT,
};

/** HSFSTS: the descriptor override strap is not asserted */
#define INTEL_PCH_SPI_HSFSTS_FDOPSS BIT(13)
/** HSFSTS: configuration locked down */
#define INTEL_PCH_SPI_HSFSTS_FLOCKDN BIT(15)

/** A flash descriptor region; limit < base when the region is unused */
struct intel_pch_spi_region {
	uint32_t base;
	uint32_t limit;
};

/** RPMC parameters (JESD216 RPMC parameter table) */
struct intel_pch_spi_rpmc_info {
	bool present;
	uint8_t component;
	uint8_t op1;
	uint8_t op2;
	uint8_t counters;
	bool op1_polls;
	uint32_t update_rate;
	/** Flash address of the component */
	uint32_t base;
	/** Busy timeouts in microseconds: counter commands, the others */
	uint32_t timeout_counter_us;
	uint32_t timeout_other_us;
};

/** An RPMC command */
struct intel_pch_spi_rpmc_cmd {
	const uint8_t *data;
	uint8_t len;
};

#define INTEL_PCH_SPI_RPMC_RESPONSE 0x32U

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_FLASH_INTEL_PCH_SPI_FLASH_API_EX_H_ */
