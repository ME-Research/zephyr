/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __SOC_H_
#define __SOC_H_

#ifndef _ASMLANGUAGE

/**
 * @brief Request a platform power down through the PMC
 *
 * Like sys_poweroff(), but returns when the PMC ignores the request (the
 * first request while its firmware patch interface is unlocked).
 *
 * @retval 0 The PMC ignored it
 * @retval 1 The PMC powers the platform down once the engine is idle
 */
int intel_csme11_power_down(void);

#endif /* !_ASMLANGUAGE */

#endif /* __SOC_H_ */
