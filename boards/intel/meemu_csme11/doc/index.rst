.. zephyr:board:: meemu_csme11

Overview
********

The Intel Converged Security and Management Engine 11 (CSME 11, in the
Sunrise Point PCH) as emulated by meemu, a unicorn-based CSME emulator. The
CSME is a Lakemont ("minute IA") core with SRAM, a local APIC and the
engine's own devices.

This is a minimal port, the core platform only:

- Lakemont CPU, 32-bit, no MMU, no FPU;
- 1 MiB of SRAM from address 0 (page 0 is left unused);
- local APIC without an IO-APIC: device interrupts are MSIs to the local
  APIC (IRQ numbers above the LVT lines, 6 and up, are message-signalled);
- system timer: the CSME application timer (64-bit counter and comparator,
  MSI). Its rate is inferred, not confirmed: 1 GHz;
- console: an STH software channel of the trace hub (``f7012040``), one D8
  packet per character and a FLAG packet per line.

Booting
*******

A replacement boot ROM (``mefwtool-c/bootrom`` in the CSME reversing
repository) loads ``zephyr.elf`` from the start of the flash's ME region and
jumps to its entry point in protected mode. See its README for building the
flash image and running it:

.. code-block:: console

   meemu -q -R 0 bootrom.bin flash.bin

The console output appears in meemu's log as ``STH 1153: ...`` lines.
