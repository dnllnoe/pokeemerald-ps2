#ifdef PORTABLE
#ifdef _WIN32
#include <windows.h>
#endif
#include <stdlib.h>
#include "global.h"
#include "platform/dma.h"
#include "m4a.h"

u16 INTR_CHECK;
void *INTR_VECTOR;
unsigned char REG_BASE[0x400] __attribute__ ((aligned (4)));
unsigned char PLTT[PLTT_SIZE] __attribute__ ((aligned (4)));
unsigned char VRAM_[VRAM_SIZE] __attribute__ ((aligned (4)));
unsigned char OAM[OAM_SIZE] __attribute__ ((aligned (4)));
unsigned char FLASH_BASE[131072] __attribute__ ((aligned (4)));
struct SoundInfo *SOUND_INFO_PTR;

extern void (*const gIntrTable[])(void);

void RunDMAsAndVBlank(void)
{
	REG_DISPSTAT |= INTR_FLAG_VBLANK;

	RunDMAs(DMA_HBLANK);

	if (REG_DISPSTAT & DISPSTAT_VBLANK_INTR)
		gIntrTable[4]();
	REG_DISPSTAT &= ~INTR_FLAG_VBLANK;
}

// All of the game's variables start out zeroed, and the Makefile moves them to
// a section of their own, gba_ram, which stands in for the GBA's EWRAM and IWRAM
static void GetGameRam(u8 **start, size_t *size)
{
#ifdef _WIN32
	const u8 *image = (const u8 *)GetModuleHandle(NULL);
	const IMAGE_NT_HEADERS *headers = (const void *)(image + ((const IMAGE_DOS_HEADER *)image)->e_lfanew);
	const IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(headers);

	*start = NULL;
	*size = 0;
	for (int i = 0; i < headers->FileHeader.NumberOfSections; i++, section++)
	{
		if (strncmp((const char *)section->Name, "gba_ram", IMAGE_SIZEOF_SHORT_NAME) == 0)
		{
			*start = (u8 *)image + section->VirtualAddress;
			*size = section->Misc.VirtualSize;
		}
	}
#else
	extern u8 __start_gba_ram[], __stop_gba_ram[];

	*start = __start_gba_ram;
	*size = __stop_gba_ram - __start_gba_ram;
#endif
}

// Byte by byte through a volatile pointer, which AddressSanitizer doesn't check:
// the section also holds the red zones it puts between variables
__attribute__((no_sanitize_address)) static void ClearGameRam(void)
{
	u8 *start;
	size_t size;

	GetGameRam(&start, &size);
	if (size == 0)
	{
		fputs("The gba_ram section is missing, so the game can't be reset\n", stderr);
		exit(1);
	}
	for (size_t i = 0; i < size; i++)
		((volatile u8 *)start)[i] = 0;
}

// Clears the RAM and registers that resetFlags selects, like the GBA BIOS.
// EWRAM and IWRAM are one section here, which gets cleared when both are
// selected. The only call that selects just one is in ReloadSave, after a link
// error, which can't happen on PC.
void RegisterRamReset(u32 resetFlags)
{
	if ((resetFlags & (RESET_EWRAM | RESET_IWRAM)) == (RESET_EWRAM | RESET_IWRAM))
		ClearGameRam();
	if (resetFlags & RESET_PALETTE)
		memset(PLTT, 0, sizeof(PLTT));
	if (resetFlags & RESET_VRAM)
		memset(VRAM_, 0, sizeof(VRAM_));
	if (resetFlags & RESET_OAM)
		memset(OAM, 0, sizeof(OAM));
	// The game always resets all of the registers together
	if (resetFlags & (RESET_SIO_REGS | RESET_SOUND_REGS | RESET_REGS))
		memset(REG_BASE, 0, sizeof(REG_BASE));
}

void AudioUpdate(void)
{
	if (gSoundInit == FALSE)
		return;

	m4aSoundMain();
	m4aSoundVSync();
}
#endif