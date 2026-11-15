/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Intel CSME address translation table (ATT): sideband windows
 *
 * The CSME reaches PCH agents on the IOSF sideband through ATT sideband
 * windows: a window maps an address range of the engine onto one agent
 * (port, opcodes and flags). There are fewer windows than agents, so the
 * driver maps agents on demand at the address the devicetree gives them and
 * evicts the least recently used window. Accesses go through the helpers
 * below, which map and access under one lock.
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MISC_INTEL_CSME_ATT_H_
#define ZEPHYR_INCLUDE_DRIVERS_MISC_INTEL_CSME_ATT_H_

#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A sideband agent as the engine maps it */
struct intel_csme_sb_target {
	/** Engine address the window maps */
	uintptr_t base;
	/** Window size in bytes */
	uint32_t size;
	/** Sideband port of the agent */
	uint8_t port;
	/** Sideband flags (FID/BAR selector) */
	uint8_t flags;
	/** Window attribute word */
	uint16_t attr;
};

/**
 * @brief Initializer for the sideband target of a devicetree node
 *
 * The node is a child of an "intel,csme-att" node with the properties of
 * intel,csme-sb-device.yaml.
 */
#define INTEL_CSME_SB_TARGET_DT(node_id)                                                   \
	{                                                                                  \
		.base = DT_REG_ADDR(node_id),                                              \
		.size = DT_REG_SIZE(node_id),                                              \
		.port = DT_PROP(node_id, intel_sb_port),                                   \
		.flags = DT_PROP_OR(node_id, intel_sb_flags, 0),                           \
		.attr = DT_PROP_OR(node_id, intel_sb_attr, 0),                             \
	}

/** The ATT device of a sideband agent's devicetree node */
#define INTEL_CSME_SB_ATT_DT(node_id) DEVICE_DT_GET(DT_PARENT(node_id))

/**
 * @brief Read a 32-bit agent register
 *
 * @param att ATT device
 * @param t Agent
 * @param off Register offset in the agent's window
 * @return Register value
 */
uint32_t intel_csme_att_sb_read32(const struct device *att, const struct intel_csme_sb_target *t,
				  uint32_t off);

/**
 * @brief Write a 32-bit agent register
 *
 * @param att ATT device
 * @param t Agent
 * @param off Register offset in the agent's window
 * @param val Value
 */
void intel_csme_att_sb_write32(const struct device *att, const struct intel_csme_sb_target *t,
			       uint32_t off, uint32_t val);

/**
 * @brief Read-modify-write a 32-bit agent register under one lock
 *
 * @param att ATT device
 * @param t Agent
 * @param off Register offset in the agent's window
 * @param mask Bits to change
 * @param val New value of those bits
 * @return The value before the update
 */
uint32_t intel_csme_att_sb_update32(const struct device *att,
				    const struct intel_csme_sb_target *t, uint32_t off,
				    uint32_t mask, uint32_t val);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MISC_INTEL_CSME_ATT_H_ */
