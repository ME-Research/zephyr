/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Intel PCH power management controller (PMC), as the CSME sees it
 *
 * The CSME drives platform power through the PMC: power-event status, the
 * host-boot handshake, IPC messages, retention SRAM, and the requests that
 * reset or power down the CSME and the platform. Register meanings come
 * from reverse engineering the CSME firmware; most bits have no public
 * documentation, so firmware policy reads and writes them by name through
 * the accessors below.
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MISC_INTEL_PCH_PMC_H_
#define ZEPHYR_INCLUDE_DRIVERS_MISC_INTEL_PCH_PMC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/sys/util.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @name PMC registers (BAR0)
 * @{
 */
/** Reset and RTC status */
#define INTEL_PCH_PMC_RST_STS       0x010U
/** Event status, write 1 to clear (host held in reset, host out of reset, ...) */
#define INTEL_PCH_PMC_EVT_STS       0x018U
/** PCH device ID and revision */
#define INTEL_PCH_PMC_PCH_ID        0x020U
/** Reset cause */
#define INTEL_PCH_PMC_RST_CAUSE     0x040U
/** CSME control: host release, reset and power-down requests */
#define INTEL_PCH_PMC_CTL           0x200U
/** PMC state: ready, host powered */
#define INTEL_PCH_PMC_STATE         0x204U
/** CSME-PMC handshake */
#define INTEL_PCH_PMC_HANDSHAKE     0x210U
/** Power-event status (suspend-well power lost, ...) */
#define INTEL_PCH_PMC_PWR_EVT_STS   0x218U
/** DeepSx and related configuration */
#define INTEL_PCH_PMC_DSX_CFG       0x250U
#define INTEL_PCH_PMC_DSX_CFG2      0x254U
/** Reset type and DeepSx */
#define INTEL_PCH_PMC_RST_TYPE      0x260U
#define INTEL_PCH_PMC_CFG_270       0x270U
/** IPC control: bit 0 power-event acknowledge, bit 1 send */
#define INTEL_PCH_PMC_IPC_CTL       0x300U
/** Power event: bit 0 pending, bits 16-19 the event code */
#define INTEL_PCH_PMC_PWR_EVENT     0x308U
#define INTEL_PCH_PMC_CFG_310       0x310U
/** Host-boot bits */
#define INTEL_PCH_PMC_BOOT          0x330U
/** CPUID of the host processor (valid with EVT_STS bit 25) */
#define INTEL_PCH_PMC_CPUID         0x334U
#define INTEL_PCH_PMC_CFG_340       0x340U
/** IPC message and reply */
#define INTEL_PCH_PMC_IPC_MSG       0x348U
#define INTEL_PCH_PMC_IPC_REPLY     0x34cU
/** @} */

/**
 * @name PMC configuration space
 * @{
 */
/** Firmware patch area offset in the SRAM (bits 0-13), bit 31 not allowed */
#define INTEL_PCH_PMC_CFG_PATCH_AREA 0x090U
/** Firmware patch control: bit 0 apply, bit 3 no patch, bit 31 lock */
#define INTEL_PCH_PMC_CFG_PATCH_CTL  0x094U
/** PMC version: bits 8-15 major, 0-7 minor */
#define INTEL_PCH_PMC_CFG_VERSION    0x098U
/** Offset of the CPU descriptor area in the SRAM */
#define INTEL_PCH_PMC_CFG_CPU_DESC   0x0a0U
/** @} */

/**
 * @brief Program the PMC's BARs and memory decoding again
 *
 * The CSME kernel does this before every power transition.
 */
void intel_pch_pmc_enable(const struct device *dev);

/** Read a PMC register */
uint32_t intel_pch_pmc_read(const struct device *dev, uint32_t reg);

/** Write a PMC register */
void intel_pch_pmc_write(const struct device *dev, uint32_t reg, uint32_t val);

/**
 * @brief Read-modify-write a PMC register
 *
 * @return The value before the update
 */
uint32_t intel_pch_pmc_update(const struct device *dev, uint32_t reg, uint32_t mask,
			      uint32_t val);

/** Read a PMC configuration space register */
uint32_t intel_pch_pmc_cfg_read(const struct device *dev, uint32_t reg);

/** Write a PMC configuration space register */
void intel_pch_pmc_cfg_write(const struct device *dev, uint32_t reg, uint32_t val);

/**
 * @brief Send an IPC message
 *
 * The message goes to IPC_MSG and IPC_CTL bit 1 starts it.
 *
 * @param dev PMC device
 * @param msg Message: bits 0-7 the command
 * @param wait Wait until the PMC has taken it (IPC_CTL bit 1 clears);
 *             without waiting, IPC_CTL is written with only the send bit
 */
void intel_pch_pmc_ipc_send(const struct device *dev, uint32_t msg, bool wait);

/** The reply to the last IPC message */
uint32_t intel_pch_pmc_ipc_reply(const struct device *dev);

/**
 * @brief Write the PMC's retention SRAM (IPC command 3)
 *
 * The PMC keeps it across CSME resets; the CSME kernel stores its S3 resume
 * keys there. One message per 16-bit word.
 *
 * @param dev PMC device
 * @param off Byte offset, even
 * @param buf Data
 * @param len Length in bytes, even
 */
void intel_pch_pmc_ipc_retention_write(const struct device *dev, uint8_t off,
				       const uint8_t *buf, size_t len);

/**
 * @brief Write the PMC's SRAM (BAR1): firmware patches, CPU descriptors
 *
 * 32-bit writes for whole aligned words, byte writes for the rest.
 *
 * @param dev PMC device
 * @param off Byte offset
 * @param buf Data
 * @param len Length in bytes
 */
void intel_pch_pmc_sram_write(const struct device *dev, uint32_t off, const void *buf,
			      size_t len);

/**
 * @brief Read the PMC's SRAM (BAR1)
 *
 * @param dev PMC device
 * @param off Byte offset
 * @param buf Data
 * @param len Length in bytes
 */
void intel_pch_pmc_sram_read(const struct device *dev, uint32_t off, void *buf, size_t len);

/**
 * @brief Request a CSME reset
 *
 * Sets the reset type, acknowledges a pending power event the PMC holds the
 * reset for, and returns: the PMC resets the CSME once the CSME is idle.
 */
void intel_pch_pmc_csme_reset(const struct device *dev);

/**
 * @brief Request a power down of the CSME and the platform
 *
 * @retval 0 The PMC did not act on it: while its firmware patch interface
 *           is unlocked, the first request only returns
 * @retval 1 The request is in; the PMC powers down once the CSME is idle
 */
int intel_pch_pmc_power_down(const struct device *dev);

/** Request a platform (global) reset */
void intel_pch_pmc_platform_reset(const struct device *dev);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MISC_INTEL_PCH_PMC_H_ */
