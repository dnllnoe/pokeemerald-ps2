#ifndef GUARD_PLATFORM_GBA_RAM_H
#define GUARD_PLATFORM_GBA_RAM_H

// Included first in every game source file when building with clang: the
// game's variables that start out zeroed go in a section of their own, which a
// soft reset clears (see GetGameRam in src/platform/system.c). With gcc,
// objcopy renames .bss after compiling instead.
#ifdef __APPLE__
#pragma clang section bss="__DATA,__gba_ram,zerofill"
#else
#pragma clang section bss="gba_ram"
#endif

#endif // GUARD_PLATFORM_GBA_RAM_H
