# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

zephyr_include_directories(.)

zephyr_cc_option(-march=pentium)

set(SOC_LINKER_SCRIPT ${ZEPHYR_BASE}/soc/intel/lakemont/linker.ld CACHE INTERNAL "")
