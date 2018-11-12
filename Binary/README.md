ARM Trusted Firmware for Raspberry Pi 3
=======================================

The `bl1` and `fip` ATF binaries found in this directory were built from 
a the [latest ATF](https://github.com/ARM-software/arm-trusted-firmware)
with the [following pull request](https://github.com/ARM-software/arm-trusted-firmware/pull/1678)
applied, using Linaro's GCC 5.5 compiler with:

```
make PLAT=rpi3 PRELOADED_BL33_BASE=0x30000 SUPPORT_VFP=1 CONSOLE_FLUSH_ON_POWEROFF=0 RPI3_PRELOADED_DTB_BASE=0x10000 fip all
```
