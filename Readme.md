Raspberry Pi (Broadcom BCM283x) EDK2 Platform Support
=====================================================

# Summary

This is a port of 64-bit Tiano Core UEFI firmware for the Pi 3/3B+ platforms,
based on [Ard Bisheuvel's 64-bit](http://www.workofard.com/2017/02/uefi-on-the-pi/)
and [Microsoft's 32-bit](https://github.com/ms-iot/RPi-UEFI/tree/ms-iot/Pi3BoardPkg)
implementations, as maintained by [Andrei Warkentin](https://github.com/andreiw/RaspberryPiPkg).

This is meant as a generally useful 64-bit ATF + UEFI implementation for the Pi 3/3B+
which should be good enough for most kind of UEFI development, as well as for running
real operating systems.

Support for Raspberry Pi 2 B+, which is being worked on, may also be added later.

# Status

This firmware, that has been validated to compile against the current
[edk2](https://github.com/tianocore/edk2)/[edk2-platforms](https://github.com/tianocore/edk2-platforms),
should be able to boot Linux (SUSE, Ubuntu), NetBSD, FreeBSD as well as Windows 10 Arm64
(full GUI version).

It also provides support for ATF ([Arm Trusted Platform](https://github.com/ARM-software/arm-trusted-firmware)).

HDMI and the mini-UART serial port can be used for output devices, with mirrored output.
USB keyboards and the mini-UART serial port can be used as input.

The boot order is currently hardcoded, first to the USB ports and then to the uSD card.
If there no bootable media media is found, the UEFI Shell is launched.
<kbd>Esc</kbd> enters platform setup. <kbd>F1</kbd> boots the UEFI Shell.

# Building

(These instructions were validated against the latest edk2 / edk2-platforms /
edk2-non-osi as of 2018.11.01, on a Debian 9.5 x64 system).

You may need to install the relevant compilation tools. Especially you should have the
ACPI Source Language (ASL) compiler, `nasm` as well as a native compiler installed.
On a Debian system, you can get these prerequisites installed with:
```
sudo apt-get install build-essential acpica-tools nasm uuid-dev
```

**IMPORTANT:** Do not be tempted to install Debian's ARM64 GCC cross compiler package,
as this currently results in GCC 6.x being installed, which is known to create issues.
Instead, you should stick with GCC 5.5, such as the one provided by Linaro, as per the
instructions below.

You can then build the firmware as follows:

```
mkdir ~/workspace
cd ~/workspace
git clone https://github.com/tianocore/edk2.git
# The following is only needed once, after you cloned edk2
make -C edk2/BaseTools
git clone https://github.com/tianocore/edk2-platforms.git
git clone https://github.com/tianocore/edk2-non-osi.git
git clone https://github.com/pbatard/RaspberryPiPkg edk2-platforms/Platform/Broadcom/Bcm283x
# You *MUST* use a GCC5 toolchain (*NOT* GCC6 or later), such as the one from
# https://releases.linaro.org/components/toolchain/binaries/5.5-2017.10/aarch64-linux-gnu/
# GCC6 and later toolchains *WILL* result in Synchronous Exceptions. You have been warned!
wget https://releases.linaro.org/components/toolchain/binaries/5.5-2017.10/aarch64-linux-gnu/gcc-linaro-5.5.0-2017.10-x86_64_aarch64-linux-gnu.tar.xz
tar -xJvf gcc-linaro-5.5.0-2017.10-x86_64_aarch64-linux-gnu.tar.xz
# If you have multiple AARCH64 toolchains, make sure the GCC5 one comes first in your path
export PATH=$PWD/gcc-linaro-5.5.0-2017.10-x86_64_aarch64-linux-gnu/bin:$PATH
export GCC5_AARCH64_PREFIX=aarch64-linux-gnu-
export WORKSPACE=$PWD
export PACKAGES_PATH=$WORKSPACE/edk2:$WORKSPACE/edk2-platforms:$WORKSPACE/edk2-non-osi
. edk2/edksetup.sh
build -a AARCH64 -t GCC5 -p edk2-platforms/Platform/Broadcom/Bcm283x/RaspberryPiPkg.dsc -b RELEASE
```

# Booting the firmware

1. Format a uSD card as FAT32
2. Copy the generated `RPI_EFI.fd` firmware onto the partition
3. Download and copy the following files from https://github.com/raspberrypi/firmware/tree/master/boot
  - `bootcode.bin`
  - `fixup.dat`
  - `start.elf`
4. Create a `config.txt` with the following content:
  ```
  arm_control=0x200
  enable_uart=1
  armstub=RPI_EFI.fd
  disable_commandline_tags=1
  ```
5. Insert the uSD card and power up the Pi.

Note that if you have a model 3+ or a model 3 where you enabled USB boot through OTP
(see [here](https://www.raspberrypi.org/documentation/hardware/raspberrypi/bootmodes/msd.md))
you may also be able to boot from a FAT32 USB driver rather than uSD.

# Notes

## Custom Device Tree

The default Device Tree included in the firmware is the one for a Raspberry Pi 3 Model B (not B+).
If you want to use a different Device Tree, to boot a Pi 3 Model B+ for instance (for which a
DTB is also provided under `DeviceTree/`), you should copy the relevant `.dtb` into the root of
the SD or USB, and then edit your `config.txt` so that it looks like:

```
(...)
disable_commandline_tags=2
device_tree_address=0x8000
device_tree_end=0x10000
device_tree=bcm2710-rpi-3-b-plus.dtb
```

Note: the address range **must** be [0x8000:0x10000]. `dtoverlay` and `dtparam` parameters are also supported.

## Custom `bootargs`

This firmware will honor the command line passed by the GPU via `cmdline.txt`.

Note, that the ultimate contents of `/chosen/bootargs` are a combination of several pieces:
- Original `/chosen/bootargs` if using the internal DTB. Seems to be completely discarded by GPU when booting with a custom device tree.
- GPU-passed hardware configuration. This one is always present.
- Additional boot options passed via `cmdline.txt`.

# Tested Platforms

## Linux

Ubuntu 18.04 LTS has been tested and confirmed to work, including its installation process.

openSUSE Leap 42.3 has also been reported to work. Other ARM64 Linux releases, that support
UEFI boot are also expected to run, though their installation process might require some
cajoling.

## Windows

Windows 10 (for ARM64) build 17134 has been tested and confirmed to work.
Builds 17125-17133 and 17672 have also been reported to work.

You probably want to look at https://www.worproject.ml/ as well as the
[Windows thread in the original RaspberryPiPkg](https://github.com/andreiw/RaspberryPiPkg/issues/12)
for installation details.

## Other platforms

Details you may need to run other platforms, including FreeBSD, is provided in the
[Readme from the original RaspberryPiPkg](https://github.com/andreiw/RaspberryPiPkg).

# Limitations

## HDMI

The UEFI HDMI video support relies on the VC (that's the GPU)
firmware to correctly detect and configure the attached screen.
Some screens are slow, and this detection may not occur fast
enough. Finally, you may wish to be able to boot your Pi
headless, yet be able to attach a display to it later for
debugging.

To accommodate these issues, the following extra lines
are recommended for your `config.txt`:
- `hdmi_force_hotplug=1` to allow plugging in video after system is booted.
- `hdmi_group=1` and `hdmi_mode=4` to force a specific mode, both to accommodate
   late-plugged screens or buggy/slow screens. See [official documentation](https://www.raspberrypi.org/documentation/configuration/config-txt/video.md)
   to make sense of these parameters (example above sets up 720p 60Hz).

## NVRAM

The Raspberry Pi has no NVRAM.

NVRAM is emulated, with the non-volatile store backed by the UEFI image itself. This means
that any changes made in UEFI proper will be persisted, but changes made in HLOS will not.
It would be nice to implement ATF-assisted warm reboot, to allow persisting HLOS
NVRAM changes.

## RTC

The Rasberry Pi has no RTC.

`RtcEpochSeconds` NVRAM variable is used to store the boot time
This should allow you to set whatever date/time you
want using the Shell date and time commands. While in UEFI
or HLOS, the time will tick forward. `RtcEpochSeconds`
is not updated on reboots.

## uSD

UEFI supports both the Arasan SDHCI and the Broadcom SDHost controllers to access the uSD slot.
You can use either. The other controller gets routed to the SDIO card. The choice made will
impact ACPI OSes booted (e.g. Windows 10). Arasan, being an SDIO controller, is usually used
with the WiFi adapter where available. SDHost cannot be used with SDIO. In UEFI setup screen:
- go to `Device Manager`
- go to `Raspberry Pi Configuration`
- go to `Chipset`
- configure `Boot uSD Routing`

Known issues:
- Arasan HS/4bit support is missing.
- No 8 bit mode support for (e)MMC (irrelevant for the Pi 3).
- Hacky (e)MMC support (no HS).
- No card removal/replacement detection, tons of timeouts and slow down during boot without an uSD card present.

## USB

- USB1 BBB mass storage devices untested (USB2 and USB3 devices are fine).
- USB1 CBI mass storage devices don't work (e.g. HP FD-05PUB floppy).

## ACPI

Should match the MS-IoT one. Good enough to boot WinPE, but unclear how functional all of it is,
given current state of WoA on RPi3. Both Arasan and SDHost SD controllers are exposed.

## Missing Functionality

- Network booting via onboard NIC.
- Ability to switch UART use to PL011.

# Reporting Issues

This repository is mostly a repackaging from Andrey Warkentin's [original RaspberryPiPkg](https://github.com/andreiw/RaspberryPiPkg),
with the aim of submitting this work for integration into [edk2-platforms](https://github.com/tianocore/edk2-platforms).

As such, any issues with this firmware are better submitted to the
[original issue tracker from that project](https://github.com/andreiw/RaspberryPiPkg/issues).
