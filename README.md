Firmware
========

This repository contains the microcontroller firmware and desktop application
for the Lumentum Project.

Microcontroller firmware is under the "fw" directory and the desktop
application will be under the "app" directory.

The "tempBuild" directory allows for development of microcontroller firmware
without the STM32F446 development board.

After cloning this git repository, please initialize and update git submodules
with `git submodule init && git submodule update`. Then, you may build firmware
using Make in the "fw" directory.

The "firmware" git submodule will allow acquiring hardware pin definitions.

Building
--------

After installing an arm-none-eabi toolchain for your system (see
[this](libopencm3/README.md)), in the fw directory, run `make`. The built
firmware ELF can be flashed normally, and be debugged with OpenOCD and GDB.

The "tempBuild" directory can be build using the
[PlatformIO](https://platformio.org) toolchain.
