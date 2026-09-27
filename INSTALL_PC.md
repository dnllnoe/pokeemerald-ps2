# Instructions

These instructions explain how to set up the tools required to build **pokeemerald**, which assembles the source files into a binary.

## Windows 10/11 (WSL1)
Follow pret instructions on how to install WSL1 [here](INSTALL.md)

Install the following libraries

```
sudo apt install build-essential git make g++-mingw-w64-i686 g++-mingw-w64-x86-64 libpng-dev
```
Download SDL2 mingw libraries from [here](https://archive.org/download/sdl-2-2.0.16/SDL2-devel-2.0.16-mingw.tar.gz)

Extract the tar to the root of the project and rename the SDL2-2.0.16 folder to SDL2

Get the number of threads in your pc using the command `nproc`

Build the Windows version using the following command (replace the parentheses with the number you got from nproc)

```
make winwsl -j(nproc number here)
```

To build 32 bit version add `IS64BIT=0` to the end of the command above

You should get an executable named `pokeemerald(64 or 32).exe`

Download SDL2.dll [32 bit version](https://archive.org/download/sdl-2-2.0.16/SDL2-2.0.16-win32-x86.zip), [64 bit version](https://archive.org/download/sdl-2-2.0.16/SDL2-2.0.16-win32-x64.zip) and put it in the same place as the executable and you should be good to go

## Linux (Ubuntu)

Run the following command to install the required libraries
```
sudo apt install build-essential git make libpng-dev libsdl2-dev
```

Get the number of threads in your pc using the command `nproc`

Build the Linux version using the following command (replace the parentheses with the number you got from nproc)

```
make linux -j(nproc number here)
```

To build 32 bit version add `IS64BIT=0` to the end of the command above (32 bit version does not build on Ubuntu 25 and above)

You should get an executable named `pokeemerald(64 or 32)`

To build with clang instead of gcc, add `COMPILER=clang`.

## macOS

Install the Xcode command line tools with `xcode-select --install`, and [Homebrew](https://brew.sh). Then install the other tools and libraries:
```
brew install make pkg-config libpng sdl2 x86_64-elf-binutils
```

The game's data is assembled with the GNU assembler from `x86_64-elf-binutils`, because clang's assembler can't build it. Homebrew's `make` is a newer GNU make than the one macOS has, and it's run as `gmake`.

Build the macOS version with:
```
gmake macos -j$(sysctl -n hw.ncpu)
```

You should get an executable named `pokeemerald64`, which uses Homebrew's SDL2.

## GBA

Follow instructions in [INSTALL.md](INSTALL.md)