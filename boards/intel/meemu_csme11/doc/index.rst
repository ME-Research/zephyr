.. zephyr:board:: meemu_csme11

Overview
********

The Intel Converged Security and Management Engine 11 (CSME 11, in the
Sunrise Point PCH) as emulated by meemu, a unicorn-based CSME emulator. The
CSME is a Lakemont ("minute IA") core with SRAM, a local APIC and the
engine's own devices.

The core platform:

- Lakemont CPU, 32-bit, no MMU, no FPU;
- 1 MiB of SRAM from address 0 (page 0 is left unused);
- local APIC without an IO-APIC: device interrupts are MSIs to the local
  APIC (IRQ numbers above the LVT lines, 6 and up, are message-signalled);
- system timer: the CSME application timer (64-bit counter and comparator,
  MSI). Its rate is inferred, not confirmed: 1 GHz;
- console: an STH software channel of the trace hub (``f7012040``), one D8
  packet per character and a FLAG packet per line.

The engine's devices and the PCH devices it drives:

- the ATT's sideband windows, mapped on demand to the PCH's IOSF sideband
  agents (``intel,csme-att``);
- the DRNG (entropy), the AON agent's always-on SRAM (retained memory), the
  PRTC's always-on registers (BBRAM) and the fuse controller (OTP), all
  sideband agents;
- the SPI flash controller (flash, with extended operations for the
  descriptor regions, HMRFPO and RPMC);
- the PMC, the engine side of HECI1-3 and the crypto engine (AES-ECB with
  raw or SKS-held keys);
- reboot and power-off through the PMC, the way the CSME kernel does them:
  ``sys_reboot(SYS_REBOOT_WARM)`` resets the CSME, ``SYS_REBOOT_COLD`` the
  platform, ``sys_poweroff()`` powers it down.

Register layouts come from reverse engineering the CSME 11 firmware; none
of this has run on hardware.

Booting
*******

A replacement boot ROM (``mefwtool-c/bootrom`` in the CSME reversing
repository) does the platform set-up of Intel's ROM, loads ``zephyr.elf``
from the FTPR partition of the flash's ME region (or the start of the
region) and jumps to its entry point in protected mode. See its README for
building the flash image and running it:

.. code-block:: console

   meemu -q -R 0 bootrom.bin flash.bin

The console output appears in meemu's log as ``STH 1153: ...`` lines.
