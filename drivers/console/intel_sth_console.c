/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Console on an Intel trace hub STH software channel: each character is a
 * D8 data packet (a byte write to Dn), each line ends with a FLAG packet.
 * A trace decoder on the host side (DCI, or an emulator) shows the text.
 */

#define DT_DRV_COMPAT intel_npk_sth

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/printk-hooks.h>
#include <zephyr/sys/libc-hooks.h>

#define STH_CHANNEL DT_INST_REG_ADDR(0)
#define STH_DN      (STH_CHANNEL + 0x00)
#define STH_FLAG    (STH_CHANNEL + 0x30)

static int sth_console_out(int c)
{
	if (c == '\n') {
		sys_write32(0, STH_FLAG);
	} else {
		sys_write8((uint8_t)c, STH_DN);
	}
	return c;
}

static int sth_console_init(void)
{
#if defined(CONFIG_STDOUT_CONSOLE)
	__stdout_hook_install(sth_console_out);
#endif
#if defined(CONFIG_PRINTK)
	__printk_hook_install(sth_console_out);
#endif
	return 0;
}

SYS_INIT(sth_console_init,
#if defined(CONFIG_EARLY_CONSOLE)
	 PRE_KERNEL_1,
#else
	 POST_KERNEL,
#endif
	 CONFIG_CONSOLE_INIT_PRIORITY);
