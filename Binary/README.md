ARM Trusted Firmware for Raspberry Pi 3
=======================================

The `bl1` and `fip` ATF binaries found in this directory were built from 
a the [latest ATF](https://github.com/ARM-software/arm-trusted-firmware)
with the [following pull request](https://github.com/ARM-software/arm-trusted-firmware/pull/1685)
applied, using Linaro's GCC 5.5 compiler with:

```
make PLAT=rpi3 PRELOADED_BL33_BASE=0x30000 SUPPORT_VFP=1 RPI3_USE_UEFI_MAP=1 RPI3_PRELOADED_DTB_BASE=0x10000 fip all
```

This results in the following memory mapping being used:

```
    0x00000000 +-----------------+
               |       ROM       | BL1
    0x00010000 +-----------------+
               |       DTB       | (Loaded by the VideoCore)
    0x00020000 +-----------------+
               |       FIP       |
    0x00030000 +-----------------+
               |                 |
               |  UEFI PAYLOAD   |
               |                 |
    0x00200000 +-----------------+
               |   Secure SRAM   | BL2, BL31
    0x00300000 +-----------------+
               |   Secure DRAM   | BL32 (Secure payload)
    0x00400000 +-----------------+
               |                 |
               |                 |
               | Non-secure DRAM | BL33
               |                 |
               |                 |
    0x01000000 +-----------------+
               |                 |
               |       ...       |
               |                 |
    0x3F000000 +-----------------+
               |       I/O       |
```