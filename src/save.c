#include "global.h"
#include "platform.h"
#include "agb_flash.h"
#include "gba/flash_internal.h"
#include "fieldmap.h"
#include "save.h"
#include "task.h"
#include "decompress.h"
#include "load_save.h"
#include "overworld.h"
#include "pokemon_storage_system.h"
#include "trainer_hill.h"
#include "link.h"
#include "constants/characters.h"
#include "constants/game_stat.h"

static u16 CalculateChecksum(void *, u16);
static bool8 ReadFlashSector(u8, struct SaveSector *);
static u8 GetSaveValidStatus(const struct SaveSectorLocation *);
static u8 CopySaveSlotData(u16, struct SaveSectorLocation *);
static u8 TryWriteSector(u8, u8 *);
static u8 HandleWriteSector(u16, const struct SaveSectorLocation *);
static u8 HandleReplaceSector(u16, const struct SaveSectorLocation *);

// Divide save blocks into individual chunks to be written to flash sectors

/*
 * Sector Layout:
 *
 * Sectors 0 - 13:      Save Slot 1
 * Sectors 14 - 27:     Save Slot 2
 * Sectors 28 - 29:     Hall of Fame
 * Sector 30:           Trainer Hill
 * Sector 31:           Recorded Battle
 *
 * There are two save slots for saving the player's game data. We alternate between
 * them each time the game is saved, so that if the current save slot is corrupt,
 * we can load the previous one. We also rotate the sectors in each save slot
 * so that the same data is not always being written to the same sector. This
 * might be done to reduce wear on the flash memory, but I'm not sure, since all
 * 14 sectors get written anyway.
 *
 * See SECTOR_ID_* constants in save.h
 */

#define SAVEBLOCK_CHUNK_OF_SIZE(size, chunkNum)                      \
{                                                                    \
    chunkNum * SECTOR_DATA_SIZE,                                     \
    (size) >= chunkNum * SECTOR_DATA_SIZE ?                          \
    min((size) - chunkNum * SECTOR_DATA_SIZE, SECTOR_DATA_SIZE) : 0  \
}
#define SAVEBLOCK_CHUNK(structure, chunkNum) SAVEBLOCK_CHUNK_OF_SIZE(sizeof(structure), chunkNum)

#ifdef VER_64BIT
/*
 * On 64-bit builds SaveBlock1 is bigger in memory than on the GBA, because it
 * contains pointers: the script of each object event template and the two
 * description lines of the Enigma Berry. The save file keeps the GBA layout,
 * with 4 bytes for each of those pointers, so saves are interchangeable with
 * the GBA and 32-bit builds. Its sectors are written from and read into
 * sSaveBlock1Saved, which is converted from/to gSaveBlock1Ptr.
 *
 * The pointers are saved as 0 and loaded as NULL. They are never used after
 * loading: CB2_ContinueSavedGame restores the object event scripts from the
 * map, and an Enigma Berry's descriptions only pointed into the e-Reader data
 * of the game that saved it (its checksum then fails, so it isn't used).
 *
 * Any other pointer added to SaveBlock1 has to be converted here as well.
 */

// struct ObjectEventTemplate as stored in the save
struct SavedObjectEventTemplate
{
    u8 localId;
    u8 graphicsId;
    u8 kind;
    s16 x;
    s16 y;
    u8 elevation;
    u8 movementType;
    u16 movementRangeX:4;
    u16 movementRangeY:4;
    u16 trainerType;
    u16 trainerRange_berryTreeId;
    u32 script;
    u16 flagId;
};

// struct Berry2 as stored in the save
struct SavedBerry2
{
    u8 name[BERRY_NAME_LENGTH + 1];
    u8 firmness;
    u16 size;
    u8 maxYield;
    u8 minYield;
    u32 description1;
    u32 description2;
    u8 stageDuration;
    u8 spicy;
    u8 dry;
    u8 sweet;
    u8 bitter;
    u8 sour;
    u8 smoothness;
};

// struct EnigmaBerry as stored in the save
struct SavedEnigmaBerry
{
    struct SavedBerry2 berry;
    u8 itemEffect[BERRY_ITEM_EFFECT_COUNT];
    u8 holdEffect;
    u8 holdEffectParam;
    u32 checksum;
};

STATIC_ASSERT(sizeof(struct SavedObjectEventTemplate) == 0x18, SavedObjectEventTemplateSize);
STATIC_ASSERT(sizeof(struct SavedEnigmaBerry) == 0x34, SavedEnigmaBerrySize);

// Where the converted parts are in memory...
#define SB1_TEMPLATES_OFFSET  offsetof(struct SaveBlock1, objectEventTemplates)
#define SB1_TEMPLATES_END     (SB1_TEMPLATES_OFFSET + OBJECT_EVENT_TEMPLATES_COUNT * sizeof(struct ObjectEventTemplate))
#define SB1_ENIGMA_OFFSET     offsetof(struct SaveBlock1, enigmaBerry)
#define SB1_ENIGMA_END        (SB1_ENIGMA_OFFSET + sizeof(struct EnigmaBerry))
#define SB1_END               (offsetof(struct SaveBlock1, waldaPhrase) + sizeof(struct WaldaPhrase))

// ...and in the save. Everything in between has the same layout in both.
#define SAVED_TEMPLATES_END   (SB1_TEMPLATES_OFFSET + OBJECT_EVENT_TEMPLATES_COUNT * sizeof(struct SavedObjectEventTemplate))
#define SAVED_ENIGMA_OFFSET   (SAVED_TEMPLATES_END + SB1_ENIGMA_OFFSET - SB1_TEMPLATES_END)
#define SAVED_ENIGMA_END      (SAVED_ENIGMA_OFFSET + sizeof(struct SavedEnigmaBerry))
#define SAVEBLOCK1_SAVE_SIZE  (SAVED_ENIGMA_END + SB1_END - SB1_ENIGMA_END)

STATIC_ASSERT(SB1_TEMPLATES_END <= SB1_ENIGMA_OFFSET, SaveBlock1ConvertedPartsInOrder);

EWRAM_DATA static u8 sSaveBlock1Saved[SAVEBLOCK1_SAVE_SIZE] ALIGNED(4) = {0};

/*
 * Before saves used the GBA layout, 64-bit builds saved SaveBlock1 as it is in
 * memory, with only 10 secret bases so that it fit. Such saves are migrated
 * when loaded (see IsOld64BitSaveBlock1).
 */
#define OLD_64BIT_SECRET_BASES_COUNT 10
#define OLD_64BIT_SAVEBLOCK1_SIZE (sizeof(struct SaveBlock1) - (SECRET_BASES_COUNT - OLD_64BIT_SECRET_BASES_COUNT) * sizeof(struct SecretBase))

STATIC_ASSERT(OLD_64BIT_SAVEBLOCK1_SIZE > (SECTOR_ID_SAVEBLOCK1_END - SECTOR_ID_SAVEBLOCK1_START) * SECTOR_DATA_SIZE
           && OLD_64BIT_SAVEBLOCK1_SIZE < SAVEBLOCK1_SAVE_SIZE, Old64BitSaveBlock1EndsInLastSector);

static void SaveBlock1ToSaved(void);
static void SaveBlock1FromSaved(void);
static bool32 IsOld64BitSaveBlock1(const struct SaveSectorLocation *locations);
static void SaveBlock1FromOld64Bit(void);

// Called right before a sector's data is copied out to be written. SaveBlock1
// is converted as late as possible, since the game copies things like the
// party into it after UpdateSaveAddresses.
static void PrepareSectorData(u16 sectorId)
{
    if (sectorId >= SECTOR_ID_SAVEBLOCK1_START && sectorId <= SECTOR_ID_SAVEBLOCK1_END)
        SaveBlock1ToSaved();
}
#else
#define SAVEBLOCK1_SAVE_SIZE sizeof(struct SaveBlock1)
#define PrepareSectorData(sectorId)
#endif

struct
{
    u16 offset;
    u16 size;
} static const sSaveSlotLayout[NUM_SECTORS_PER_SLOT] =
{
    SAVEBLOCK_CHUNK(struct SaveBlock2, 0), // SECTOR_ID_SAVEBLOCK2

    SAVEBLOCK_CHUNK_OF_SIZE(SAVEBLOCK1_SAVE_SIZE, 0), // SECTOR_ID_SAVEBLOCK1_START
    SAVEBLOCK_CHUNK_OF_SIZE(SAVEBLOCK1_SAVE_SIZE, 1),
    SAVEBLOCK_CHUNK_OF_SIZE(SAVEBLOCK1_SAVE_SIZE, 2),
    SAVEBLOCK_CHUNK_OF_SIZE(SAVEBLOCK1_SAVE_SIZE, 3), // SECTOR_ID_SAVEBLOCK1_END

    SAVEBLOCK_CHUNK(struct PokemonStorage, 0), // SECTOR_ID_PKMN_STORAGE_START
    SAVEBLOCK_CHUNK(struct PokemonStorage, 1),
    SAVEBLOCK_CHUNK(struct PokemonStorage, 2),
    SAVEBLOCK_CHUNK(struct PokemonStorage, 3),
    SAVEBLOCK_CHUNK(struct PokemonStorage, 4),
    SAVEBLOCK_CHUNK(struct PokemonStorage, 5),
    SAVEBLOCK_CHUNK(struct PokemonStorage, 6),
    SAVEBLOCK_CHUNK(struct PokemonStorage, 7),
    SAVEBLOCK_CHUNK(struct PokemonStorage, 8), // SECTOR_ID_PKMN_STORAGE_END
};

// These will produce an error if a save struct is larger than the space
// alloted for it in the flash.
STATIC_ASSERT(sizeof(struct SaveBlock2) <= SECTOR_DATA_SIZE, SaveBlock2FreeSpace);
STATIC_ASSERT(SAVEBLOCK1_SAVE_SIZE <= SECTOR_DATA_SIZE * (SECTOR_ID_SAVEBLOCK1_END - SECTOR_ID_SAVEBLOCK1_START + 1), SaveBlock1FreeSpace);
STATIC_ASSERT(sizeof(struct PokemonStorage) <= SECTOR_DATA_SIZE * (SECTOR_ID_PKMN_STORAGE_END - SECTOR_ID_PKMN_STORAGE_START + 1), PokemonStorageFreeSpace);

COMMON_DATA u16 gLastWrittenSector = 0;
COMMON_DATA u32 gLastSaveCounter = 0;
COMMON_DATA u16 gLastKnownGoodSector = 0;
COMMON_DATA u32 gDamagedSaveSectors = 0;
COMMON_DATA u32 gSaveCounter = 0;
COMMON_DATA struct SaveSector *gReadWriteSector = NULL; // Pointer to a buffer for reading/writing a sector
COMMON_DATA u16 gIncrementalSectorId = 0;
COMMON_DATA u16 gSaveUnusedVar = 0;
COMMON_DATA u16 gSaveFileStatus = 0;
COMMON_DATA MainCallback gGameContinueCallback = NULL;
COMMON_DATA struct SaveSectorLocation gRamSaveSectorLocations[NUM_SECTORS_PER_SLOT] = {0};
COMMON_DATA u16 gSaveUnusedVar2 = 0;
COMMON_DATA u16 gSaveAttemptStatus = 0;

EWRAM_DATA struct SaveSector gSaveDataBuffer = {0}; // Buffer used for reading/writing sectors
EWRAM_DATA static u8 sUnusedVar = 0;

void ClearSaveData(void)
{
    u16 i;

    // Clear the full save two sectors at a time
    for (i = 0; i < SECTORS_COUNT / 2; i++)
    {
        EraseFlashSector(i);
        EraseFlashSector(i + SECTORS_COUNT / 2);
    }
}

void Save_ResetSaveCounters(void)
{
    gSaveCounter = 0;
    gLastWrittenSector = 0;
    gDamagedSaveSectors = 0;
}

static bool32 SetDamagedSectorBits(u8 op, u8 sectorId)
{
    bool32 retVal = FALSE;

    switch (op)
    {
    case ENABLE:
        gDamagedSaveSectors |= (1 << sectorId);
        break;
    case DISABLE:
        gDamagedSaveSectors &= ~(1 << sectorId);
        break;
    case CHECK: // unused
        if (gDamagedSaveSectors & (1 << sectorId))
            retVal = TRUE;
        break;
    }

    return retVal;
}

static u8 WriteSaveSectorOrSlot(u16 sectorId, const struct SaveSectorLocation *locations)
{
    u32 status;
    u16 i;

    gReadWriteSector = &gSaveDataBuffer;

    if (sectorId != FULL_SAVE_SLOT)
    {
        // A sector was specified, just write that sector.
        // This is never reached, FULL_SAVE_SLOT is always used instead.
        status = HandleWriteSector(sectorId, locations);
    }
    else
    {
        // No sector was specified, write full save slot.
        gLastKnownGoodSector = gLastWrittenSector; // backup the current written sector before attempting to write.
        gLastSaveCounter = gSaveCounter;
        gLastWrittenSector++;
        gLastWrittenSector = gLastWrittenSector % NUM_SECTORS_PER_SLOT;
        gSaveCounter++;
        status = SAVE_STATUS_OK;

        for (i = 0; i < NUM_SECTORS_PER_SLOT; i++)
            HandleWriteSector(i, locations);

        if (gDamagedSaveSectors)
        {
            // At least one sector save failed
            status = SAVE_STATUS_ERROR;
            gLastWrittenSector = gLastKnownGoodSector;
            gSaveCounter = gLastSaveCounter;
        }
    }

    return status;
}

static u8 HandleWriteSector(u16 sectorId, const struct SaveSectorLocation *locations)
{
    u16 i;
    u16 sector;
    u8 *data;
    u16 size;

    // Adjust sector id for current save slot
    sector = sectorId + gLastWrittenSector;
    sector %= NUM_SECTORS_PER_SLOT;
    sector += NUM_SECTORS_PER_SLOT * (gSaveCounter % NUM_SAVE_SLOTS);

    // Get current save data
    PrepareSectorData(sectorId);
    data = locations[sectorId].data;
    size = locations[sectorId].size;

    // Clear temp save sector
    for (i = 0; i < SECTOR_SIZE; i++)
        ((u8 *)gReadWriteSector)[i] = 0;

    // Set footer data
    gReadWriteSector->id = sectorId;
    gReadWriteSector->signature = SECTOR_SIGNATURE;
    gReadWriteSector->counter = gSaveCounter;

    // Copy current data to temp buffer for writing
    for (i = 0; i < size; i++)
        gReadWriteSector->data[i] = data[i];

    gReadWriteSector->checksum = CalculateChecksum(data, size);

    return TryWriteSector(sector, gReadWriteSector->data);
}

static u8 HandleWriteSectorNBytes(u8 sectorId, u8 *data, u16 size)
{
    u16 i;
    struct SaveSector *sector = &gSaveDataBuffer;

    // Clear temp save sector
    for (i = 0; i < SECTOR_SIZE; i++)
        ((u8 *)sector)[i] = 0;

    sector->signature = SECTOR_SIGNATURE;

    // Copy data to temp buffer for writing
    for (i = 0; i < size; i++)
        sector->data[i] = data[i];

    sector->id = CalculateChecksum(data, size); // though this appears to be incorrect, it might be some sector checksum instead of a whole save checksum and only appears to be relevent to HOF data, if used.
    return TryWriteSector(sectorId, sector->data);
}

static u8 TryWriteSector(u8 sector, u8 *data)
{
    if (ProgramFlashSectorAndVerify(sector, data)) // is damaged?
    {
        // Failed
        SetDamagedSectorBits(ENABLE, sector);
        return SAVE_STATUS_ERROR;
    }
    else
    {
        // Succeeded
        SetDamagedSectorBits(DISABLE, sector);
        return SAVE_STATUS_OK;
    }
}

static u32 RestoreSaveBackupVarsAndIncrement(const struct SaveSectorLocation *locations)
{
    gReadWriteSector = &gSaveDataBuffer;
    gLastKnownGoodSector = gLastWrittenSector;
    gLastSaveCounter = gSaveCounter;
    gLastWrittenSector++;
    gLastWrittenSector %= NUM_SECTORS_PER_SLOT;
    gSaveCounter++;
    gIncrementalSectorId = 0;
    gDamagedSaveSectors = 0;
    return 0;
}

static u32 RestoreSaveBackupVars(const struct SaveSectorLocation *locations)
{
    gReadWriteSector = &gSaveDataBuffer;
    gLastKnownGoodSector = gLastWrittenSector;
    gLastSaveCounter = gSaveCounter;
    gIncrementalSectorId = 0;
    gDamagedSaveSectors = 0;
    return 0;
}

static u8 HandleWriteIncrementalSector(u16 numSectors, const struct SaveSectorLocation *locations)
{
    u8 status;

    if (gIncrementalSectorId < numSectors - 1)
    {
        status = SAVE_STATUS_OK;
        HandleWriteSector(gIncrementalSectorId, locations);
        gIncrementalSectorId++;
        if (gDamagedSaveSectors)
        {
            status = SAVE_STATUS_ERROR;
            gLastWrittenSector = gLastKnownGoodSector;
            gSaveCounter = gLastSaveCounter;
        }
    }
    else
    {
        // Exceeded max sector, finished
        status = SAVE_STATUS_ERROR;
    }

    return status;
}

static u8 HandleReplaceSectorAndVerify(u16 sectorId, const struct SaveSectorLocation *locations)
{
    u8 status = SAVE_STATUS_OK;

    HandleReplaceSector(sectorId - 1, locations);

    if (gDamagedSaveSectors)
    {
        status = SAVE_STATUS_ERROR;
        gLastWrittenSector = gLastKnownGoodSector;
        gSaveCounter = gLastSaveCounter;
    }
    return status;
}

// Similar to HandleWriteSector, but fully erases the sector first, and skips writing the first signature byte
static u8 HandleReplaceSector(u16 sectorId, const struct SaveSectorLocation *locations)
{
    u16 i;
    u16 sector;
    u8 *data;
    u16 size;
    u8 status;

    // Adjust sector id for current save slot
    sector = sectorId + gLastWrittenSector;
    sector %= NUM_SECTORS_PER_SLOT;
    sector += NUM_SECTORS_PER_SLOT * (gSaveCounter % NUM_SAVE_SLOTS);

    // Get current save data
    PrepareSectorData(sectorId);
    data = locations[sectorId].data;
    size = locations[sectorId].size;

    // Clear temp save sector.
    for (i = 0; i < SECTOR_SIZE; i++)
        ((u8 *)gReadWriteSector)[i] = 0;

    // Set footer data
    gReadWriteSector->id = sectorId;
    gReadWriteSector->signature = SECTOR_SIGNATURE;
    gReadWriteSector->counter = gSaveCounter;

    // Copy current data to temp buffer for writing
    for (i = 0; i < size; i++)
        gReadWriteSector->data[i] = data[i];

    gReadWriteSector->checksum = CalculateChecksum(data, size);

    // Erase old save data
    EraseFlashSector(sector);

    status = SAVE_STATUS_OK;

    // Write new save data up to signature field
    for (i = 0; i < SECTOR_SIGNATURE_OFFSET; i++)
    {
        if (ProgramFlashByte(sector, i, ((u8 *)gReadWriteSector)[i]))
        {
            status = SAVE_STATUS_ERROR;
            break;
        }
    }

    if (status == SAVE_STATUS_ERROR)
    {
        // Writing save data failed
        SetDamagedSectorBits(ENABLE, sector);
        return SAVE_STATUS_ERROR;
    }
    else
    {
        // Writing save data succeeded, write signature and counter
        status = SAVE_STATUS_OK;

        // Write signature (skipping the first byte) and counter fields.
        // The byte of signature that is skipped is instead written by WriteSectorSignatureByte or WriteSectorSignatureByte_NoOffset
        for (i = 0; i < SECTOR_SIZE - (SECTOR_SIGNATURE_OFFSET + 1); i++)
        {
            if (ProgramFlashByte(sector, SECTOR_SIGNATURE_OFFSET + 1 + i, ((u8 *)gReadWriteSector)[SECTOR_SIGNATURE_OFFSET + 1 + i]))
            {
                status = SAVE_STATUS_ERROR;
                break;
            }
        }

        if (status == SAVE_STATUS_ERROR)
        {
            // Writing signature/counter failed
            SetDamagedSectorBits(ENABLE, sector);
            return SAVE_STATUS_ERROR;
        }
        else
        {
            // Succeeded
            SetDamagedSectorBits(DISABLE, sector);
            return SAVE_STATUS_OK;
        }
    }
}

static u8 WriteSectorSignatureByte_NoOffset(u16 sectorId, const struct SaveSectorLocation *locations)
{
    // Adjust sector id for current save slot
    // This first line lacking -1 is the only difference from WriteSectorSignatureByte
    u16 sector = sectorId + gLastWrittenSector;
    sector %= NUM_SECTORS_PER_SLOT;
    sector += NUM_SECTORS_PER_SLOT * (gSaveCounter % NUM_SAVE_SLOTS);

    // Write just the first byte of the signature field, which was skipped by HandleReplaceSector
    if (ProgramFlashByte(sector, SECTOR_SIGNATURE_OFFSET, SECTOR_SIGNATURE & 0xFF))
    {
        // Sector is damaged, so enable the bit in gDamagedSaveSectors and restore the last written sector and save counter.
        SetDamagedSectorBits(ENABLE, sector);
        gLastWrittenSector = gLastKnownGoodSector;
        gSaveCounter = gLastSaveCounter;
        return SAVE_STATUS_ERROR;
    }
    else
    {
        // Succeeded
        SetDamagedSectorBits(DISABLE, sector);
        return SAVE_STATUS_OK;
    }
}

static u8 CopySectorSignatureByte(u16 sectorId, const struct SaveSectorLocation *locations)
{
    // Adjust sector id for current save slot
    u16 sector = sectorId + gLastWrittenSector - 1;
    sector %= NUM_SECTORS_PER_SLOT;
    sector += NUM_SECTORS_PER_SLOT * (gSaveCounter % NUM_SAVE_SLOTS);

    // Copy just the first byte of the signature field from the read/write buffer
    if (ProgramFlashByte(sector, SECTOR_SIGNATURE_OFFSET, ((u8 *)gReadWriteSector)[SECTOR_SIGNATURE_OFFSET]))
    {
        // Sector is damaged, so enable the bit in gDamagedSaveSectors and restore the last written sector and save counter.
        SetDamagedSectorBits(ENABLE, sector);
        gLastWrittenSector = gLastKnownGoodSector;
        gSaveCounter = gLastSaveCounter;
        return SAVE_STATUS_ERROR;
    }
    else
    {
        // Succeeded
        SetDamagedSectorBits(DISABLE, sector);
        return SAVE_STATUS_OK;
    }
}

static u8 WriteSectorSignatureByte(u16 sectorId, const struct SaveSectorLocation *locations)
{
    // Adjust sector id for current save slot
    u16 sector = sectorId + gLastWrittenSector - 1;
    sector %= NUM_SECTORS_PER_SLOT;
    sector += NUM_SECTORS_PER_SLOT * (gSaveCounter % NUM_SAVE_SLOTS);

    // Write just the first byte of the signature field, which was skipped by HandleReplaceSector
    if (ProgramFlashByte(sector, SECTOR_SIGNATURE_OFFSET, SECTOR_SIGNATURE & 0xFF))
    {
        // Sector is damaged, so enable the bit in gDamagedSaveSectors and restore the last written sector and save counter.
        SetDamagedSectorBits(ENABLE, sector);
        gLastWrittenSector = gLastKnownGoodSector;
        gSaveCounter = gLastSaveCounter;
        return SAVE_STATUS_ERROR;
    }
    else
    {
        // Succeeded
        SetDamagedSectorBits(DISABLE, sector);
        return SAVE_STATUS_OK;
    }
}

static u8 TryLoadSaveSlot(u16 sectorId, struct SaveSectorLocation *locations)
{
    u8 status;
    gReadWriteSector = &gSaveDataBuffer;
    if (sectorId != FULL_SAVE_SLOT)
    {
        // This function may not be used with a specific sector id
        status = SAVE_STATUS_ERROR;
    }
    else
    {
        status = GetSaveValidStatus(locations);
        CopySaveSlotData(FULL_SAVE_SLOT, locations);
    }

    return status;
}

// sectorId arg is ignored, this always reads the full save slot
static u8 CopySaveSlotData(u16 sectorId, struct SaveSectorLocation *locations)
{
    u16 i;
    u16 checksum;
    u16 slotOffset = NUM_SECTORS_PER_SLOT * (gSaveCounter % NUM_SAVE_SLOTS);
    u16 id;

    for (i = 0; i < NUM_SECTORS_PER_SLOT; i++)
    {
        ReadFlashSector(i + slotOffset, gReadWriteSector);

        id = gReadWriteSector->id;
        if (id == 0)
            gLastWrittenSector = i;

        checksum = CalculateChecksum(gReadWriteSector->data, locations[id].size);

        // Only copy data for sectors whose signature and checksum fields are correct
        if (gReadWriteSector->signature == SECTOR_SIGNATURE && gReadWriteSector->checksum == checksum)
        {
            u16 j;
            for (j = 0; j < locations[id].size; j++)
                ((u8 *)locations[id].data)[j] = gReadWriteSector->data[j];
        }
    }

    return SAVE_STATUS_OK;
}

static u8 GetSaveValidStatus(const struct SaveSectorLocation *locations)
{
    u16 i;
    u16 checksum;
    u32 saveSlot1Counter = 0;
    u32 saveSlot2Counter = 0;
    u32 validSectorFlags = 0;
    bool8 signatureValid = FALSE;
    u8 saveSlot1Status;
    u8 saveSlot2Status;

    // Check save slot 1
    for (i = 0; i < NUM_SECTORS_PER_SLOT; i++)
    {
        ReadFlashSector(i, gReadWriteSector);
        if (gReadWriteSector->signature == SECTOR_SIGNATURE)
        {
            signatureValid = TRUE;
            checksum = CalculateChecksum(gReadWriteSector->data, locations[gReadWriteSector->id].size);
            if (gReadWriteSector->checksum == checksum)
            {
                saveSlot1Counter = gReadWriteSector->counter;
                validSectorFlags |= 1 << gReadWriteSector->id;
            }
        }
    }

    if (signatureValid)
    {
        if (validSectorFlags == (1 << NUM_SECTORS_PER_SLOT) - 1)
            saveSlot1Status = SAVE_STATUS_OK;
        else
            saveSlot1Status = SAVE_STATUS_ERROR;
    }
    else
    {
        // No sectors in slot 1 have the correct signature, treat it as empty
        saveSlot1Status = SAVE_STATUS_EMPTY;
    }

    validSectorFlags = 0;
    signatureValid = FALSE;

    // Check save slot 2
    for (i = 0; i < NUM_SECTORS_PER_SLOT; i++)
    {
        ReadFlashSector(i + NUM_SECTORS_PER_SLOT, gReadWriteSector);
        if (gReadWriteSector->signature == SECTOR_SIGNATURE)
        {
            signatureValid = TRUE;
            checksum = CalculateChecksum(gReadWriteSector->data, locations[gReadWriteSector->id].size);
            if (gReadWriteSector->checksum == checksum)
            {
                saveSlot2Counter = gReadWriteSector->counter;
                validSectorFlags |= 1 << gReadWriteSector->id;
            }
        }
    }

    if (signatureValid)
    {
        if (validSectorFlags == (1 << NUM_SECTORS_PER_SLOT) - 1)
            saveSlot2Status = SAVE_STATUS_OK;
        else
            saveSlot2Status = SAVE_STATUS_ERROR;
    }
    else
    {
        // No sectors in slot 2 have the correct signature, treat it as empty.
        saveSlot2Status = SAVE_STATUS_EMPTY;
    }

    if (saveSlot1Status == SAVE_STATUS_OK && saveSlot2Status == SAVE_STATUS_OK)
    {
        if ((saveSlot1Counter == -1 && saveSlot2Counter ==  0)
         || (saveSlot1Counter ==  0 && saveSlot2Counter == -1))
        {
            if ((unsigned)(saveSlot1Counter + 1) < (unsigned)(saveSlot2Counter + 1))
                gSaveCounter = saveSlot2Counter;
            else
                gSaveCounter = saveSlot1Counter;
        }
        else
        {
            if (saveSlot1Counter < saveSlot2Counter)
                gSaveCounter = saveSlot2Counter;
            else
                gSaveCounter = saveSlot1Counter;
        }
        return SAVE_STATUS_OK;
    }

    // One or both save slots are not OK

    if (saveSlot1Status == SAVE_STATUS_OK)
    {
        gSaveCounter = saveSlot1Counter;
        if (saveSlot2Status == SAVE_STATUS_ERROR)
            return SAVE_STATUS_ERROR; // Slot 2 errored
        return SAVE_STATUS_OK; // Slot 1 is OK, slot 2 is empty
    }

    if (saveSlot2Status == SAVE_STATUS_OK)
    {
        gSaveCounter = saveSlot2Counter;
        if (saveSlot1Status == SAVE_STATUS_ERROR)
            return SAVE_STATUS_ERROR; // Slot 1 errored
        return SAVE_STATUS_OK; // Slot 2 is OK, slot 1 is empty
    }

    // Neither slot is OK, check if both are empty
    if (saveSlot1Status == SAVE_STATUS_EMPTY
     && saveSlot2Status == SAVE_STATUS_EMPTY)
    {
        gSaveCounter = 0;
        gLastWrittenSector = 0;
        return SAVE_STATUS_EMPTY;
    }

    // Both slots errored
    gSaveCounter = 0;
    gLastWrittenSector = 0;
    return SAVE_STATUS_CORRUPT;
}

static u8 TryLoadSaveSector(u8 sectorId, u8 *data, u16 size)
{
    u16 i;
    struct SaveSector *sector = &gSaveDataBuffer;
    ReadFlashSector(sectorId, sector);
    if (sector->signature == SECTOR_SIGNATURE)
    {
        u16 checksum = CalculateChecksum(sector->data, size);
        if (sector->id == checksum)
        {
            // Signature and checksum are correct, copy data
            for (i = 0; i < size; i++)
                data[i] = sector->data[i];
            return SAVE_STATUS_OK;
        }
        else
        {
            // Incorrect checksum
            return SAVE_STATUS_CORRUPT;
        }
    }
    else
    {
        // Incorrect signature value
        return SAVE_STATUS_EMPTY;
    }
}

// Return value always ignored
static bool8 ReadFlashSector(u8 sectorId, struct SaveSector *sector)
{
    ReadFlash(sectorId, 0, sector->data, SECTOR_SIZE);
    return TRUE;
}

static u16 CalculateChecksum(void *data, u16 size)
{
    u16 i;
    u32 checksum = 0;

    for (i = 0; i < (size / 4); i++)
    {
        checksum += *((u32 *)data);
        data += sizeof(u32);
    }

    return ((checksum >> 16) + checksum);
}

#ifdef VER_64BIT
static void SaveBlock1ToSaved(void)
{
    const u8 *block = (const u8 *)gSaveBlock1Ptr;
    struct SavedObjectEventTemplate *savedTemplates = (void *)&sSaveBlock1Saved[SB1_TEMPLATES_OFFSET];
    struct SavedEnigmaBerry *savedBerry = (void *)&sSaveBlock1Saved[SAVED_ENIGMA_OFFSET];
    const struct EnigmaBerry *berry;
    s32 i;

    if (gSaveBlock1Ptr == NULL)
        return;

    // Padding is saved as 0, and so are the pointers
    memset(sSaveBlock1Saved, 0, sizeof(sSaveBlock1Saved));

    memcpy(sSaveBlock1Saved, block, SB1_TEMPLATES_OFFSET);
    for (i = 0; i < OBJECT_EVENT_TEMPLATES_COUNT; i++)
    {
        const struct ObjectEventTemplate *template = &gSaveBlock1Ptr->objectEventTemplates[i];

        savedTemplates[i].localId = template->localId;
        savedTemplates[i].graphicsId = template->graphicsId;
        savedTemplates[i].kind = template->kind;
        savedTemplates[i].x = template->x;
        savedTemplates[i].y = template->y;
        savedTemplates[i].elevation = template->elevation;
        savedTemplates[i].movementType = template->movementType;
        savedTemplates[i].movementRangeX = template->movementRangeX;
        savedTemplates[i].movementRangeY = template->movementRangeY;
        savedTemplates[i].trainerType = template->trainerType;
        savedTemplates[i].trainerRange_berryTreeId = template->trainerRange_berryTreeId;
        savedTemplates[i].flagId = template->flagId;
    }

    memcpy(&sSaveBlock1Saved[SAVED_TEMPLATES_END], &block[SB1_TEMPLATES_END], SB1_ENIGMA_OFFSET - SB1_TEMPLATES_END);

    berry = &gSaveBlock1Ptr->enigmaBerry;
    memcpy(savedBerry->berry.name, berry->berry.name, sizeof(savedBerry->berry.name));
    savedBerry->berry.firmness = berry->berry.firmness;
    savedBerry->berry.size = berry->berry.size;
    savedBerry->berry.maxYield = berry->berry.maxYield;
    savedBerry->berry.minYield = berry->berry.minYield;
    savedBerry->berry.stageDuration = berry->berry.stageDuration;
    savedBerry->berry.spicy = berry->berry.spicy;
    savedBerry->berry.dry = berry->berry.dry;
    savedBerry->berry.sweet = berry->berry.sweet;
    savedBerry->berry.bitter = berry->berry.bitter;
    savedBerry->berry.sour = berry->berry.sour;
    savedBerry->berry.smoothness = berry->berry.smoothness;
    memcpy(savedBerry->itemEffect, berry->itemEffect, sizeof(savedBerry->itemEffect));
    savedBerry->holdEffect = berry->holdEffect;
    savedBerry->holdEffectParam = berry->holdEffectParam;
    savedBerry->checksum = berry->checksum;

    memcpy(&sSaveBlock1Saved[SAVED_ENIGMA_END], &block[SB1_ENIGMA_END], SB1_END - SB1_ENIGMA_END);
}

static void SaveBlock1FromSaved(void)
{
    u8 *block = (u8 *)gSaveBlock1Ptr;
    const struct SavedObjectEventTemplate *savedTemplates = (const void *)&sSaveBlock1Saved[SB1_TEMPLATES_OFFSET];
    const struct SavedEnigmaBerry *savedBerry = (const void *)&sSaveBlock1Saved[SAVED_ENIGMA_OFFSET];
    struct EnigmaBerry *berry;
    s32 i;

    memcpy(block, sSaveBlock1Saved, SB1_TEMPLATES_OFFSET);
    for (i = 0; i < OBJECT_EVENT_TEMPLATES_COUNT; i++)
    {
        struct ObjectEventTemplate *template = &gSaveBlock1Ptr->objectEventTemplates[i];

        memset(template, 0, sizeof(*template));
        template->localId = savedTemplates[i].localId;
        template->graphicsId = savedTemplates[i].graphicsId;
        template->kind = savedTemplates[i].kind;
        template->x = savedTemplates[i].x;
        template->y = savedTemplates[i].y;
        template->elevation = savedTemplates[i].elevation;
        template->movementType = savedTemplates[i].movementType;
        template->movementRangeX = savedTemplates[i].movementRangeX;
        template->movementRangeY = savedTemplates[i].movementRangeY;
        template->trainerType = savedTemplates[i].trainerType;
        template->trainerRange_berryTreeId = savedTemplates[i].trainerRange_berryTreeId;
        template->script = NULL;
        template->flagId = savedTemplates[i].flagId;
    }

    memcpy(&block[SB1_TEMPLATES_END], &sSaveBlock1Saved[SAVED_TEMPLATES_END], SB1_ENIGMA_OFFSET - SB1_TEMPLATES_END);

    // Cleared first so the padding doesn't affect the berry's checksum
    berry = &gSaveBlock1Ptr->enigmaBerry;
    memset(berry, 0, sizeof(*berry));
    memcpy(berry->berry.name, savedBerry->berry.name, sizeof(berry->berry.name));
    berry->berry.firmness = savedBerry->berry.firmness;
    berry->berry.size = savedBerry->berry.size;
    berry->berry.maxYield = savedBerry->berry.maxYield;
    berry->berry.minYield = savedBerry->berry.minYield;
    berry->berry.description1 = NULL;
    berry->berry.description2 = NULL;
    berry->berry.stageDuration = savedBerry->berry.stageDuration;
    berry->berry.spicy = savedBerry->berry.spicy;
    berry->berry.dry = savedBerry->berry.dry;
    berry->berry.sweet = savedBerry->berry.sweet;
    berry->berry.bitter = savedBerry->berry.bitter;
    berry->berry.sour = savedBerry->berry.sour;
    berry->berry.smoothness = savedBerry->berry.smoothness;
    memcpy(berry->itemEffect, savedBerry->itemEffect, sizeof(berry->itemEffect));
    berry->holdEffect = savedBerry->holdEffect;
    berry->holdEffectParam = savedBerry->holdEffectParam;
    berry->checksum = savedBerry->checksum;

    memcpy(&block[SB1_ENIGMA_END], &sSaveBlock1Saved[SAVED_ENIGMA_END], SB1_END - SB1_ENIGMA_END);
}

// An old 64-bit SaveBlock1 is shorter than the GBA layout, so the end of its
// last sector was left empty. In the GBA layout that part always has data,
// e.g. the Union Room phrases that get set when starting a new game.
static bool32 IsOld64BitSaveBlock1(const struct SaveSectorLocation *locations)
{
    u16 slotOffset = NUM_SECTORS_PER_SLOT * (gSaveCounter % NUM_SAVE_SLOTS);
    u16 emptyStart = OLD_64BIT_SAVEBLOCK1_SIZE - sSaveSlotLayout[SECTOR_ID_SAVEBLOCK1_END].offset;
    u16 i, j;

    // Look at the last SaveBlock1 sector of the slot that was loaded
    for (i = 0; i < NUM_SECTORS_PER_SLOT; i++)
    {
        ReadFlashSector(i + slotOffset, gReadWriteSector);
        if (gReadWriteSector->id != SECTOR_ID_SAVEBLOCK1_END)
            continue;

        if (gReadWriteSector->signature != SECTOR_SIGNATURE
         || gReadWriteSector->checksum != CalculateChecksum(gReadWriteSector->data, locations[SECTOR_ID_SAVEBLOCK1_END].size))
            return FALSE;

        for (j = emptyStart; j < locations[SECTOR_ID_SAVEBLOCK1_END].size; j++)
        {
            if (gReadWriteSector->data[j] != 0)
                return FALSE;
        }
        return TRUE;
    }
    return FALSE;
}

// The old layout is the in-memory one, with fewer secret bases
static void SaveBlock1FromOld64Bit(void)
{
    u8 *block = (u8 *)gSaveBlock1Ptr;
    size_t basesOffset = offsetof(struct SaveBlock1, secretBases);
    size_t oldBasesEnd = basesOffset + OLD_64BIT_SECRET_BASES_COUNT * sizeof(struct SecretBase);
    size_t basesEnd = basesOffset + SECRET_BASES_COUNT * sizeof(struct SecretBase);
    s32 i;

    memcpy(block, sSaveBlock1Saved, oldBasesEnd);
    memcpy(&block[basesEnd], &sSaveBlock1Saved[oldBasesEnd], OLD_64BIT_SAVEBLOCK1_SIZE - oldBasesEnd);

    // Empty the added secret bases, like ClearSecretBase
    for (i = OLD_64BIT_SECRET_BASES_COUNT; i < SECRET_BASES_COUNT; i++)
    {
        memset(&gSaveBlock1Ptr->secretBases[i], 0, sizeof(struct SecretBase));
        memset(gSaveBlock1Ptr->secretBases[i].trainerName, EOS, PLAYER_NAME_LENGTH);
    }

    // These were saved as pointers into the build that saved them
    for (i = 0; i < OBJECT_EVENT_TEMPLATES_COUNT; i++)
        gSaveBlock1Ptr->objectEventTemplates[i].script = NULL;
    gSaveBlock1Ptr->enigmaBerry.berry.description1 = NULL;
    gSaveBlock1Ptr->enigmaBerry.berry.description2 = NULL;
}
#endif

static void UpdateSaveAddresses(void)
{
    int i = SECTOR_ID_SAVEBLOCK2;
    gRamSaveSectorLocations[i].data = (void *)(gSaveBlock2Ptr) + sSaveSlotLayout[i].offset;
    gRamSaveSectorLocations[i].size = sSaveSlotLayout[i].size;

    for (i = SECTOR_ID_SAVEBLOCK1_START; i <= SECTOR_ID_SAVEBLOCK1_END; i++)
    {
#ifdef VER_64BIT
        gRamSaveSectorLocations[i].data = sSaveBlock1Saved + sSaveSlotLayout[i].offset;
#else
        gRamSaveSectorLocations[i].data = (void *)(gSaveBlock1Ptr) + sSaveSlotLayout[i].offset;
#endif
        gRamSaveSectorLocations[i].size = sSaveSlotLayout[i].size;
    }

    for (; i <= SECTOR_ID_PKMN_STORAGE_END; i++) //setting i to SECTOR_ID_PKMN_STORAGE_START does not match
    {
        gRamSaveSectorLocations[i].data = (void *)(gPokemonStoragePtr) + sSaveSlotLayout[i].offset;
        gRamSaveSectorLocations[i].size = sSaveSlotLayout[i].size;
    }
}

u8 HandleSavingData(u8 saveType)
{
    u8 i;
    u32 *backupVar = gTrainerHillVBlankCounter;
    u8 *tempAddr;

    gTrainerHillVBlankCounter = NULL;
    UpdateSaveAddresses();
    switch (saveType)
    {
    case SAVE_HALL_OF_FAME_ERASE_BEFORE:
        // Unused. Erases the special save sectors (HOF, Trainer Hill, Recorded Battle)
        // before overwriting HOF.
        for (i = SECTOR_ID_HOF_1; i < SECTORS_COUNT; i++)
            EraseFlashSector(i);
        // fallthrough
    case SAVE_HALL_OF_FAME:
        if (GetGameStat(GAME_STAT_ENTERED_HOF) < 999)
            IncrementGameStat(GAME_STAT_ENTERED_HOF);

        // Write the full save slot first
        CopyPartyAndObjectsToSave();
        WriteSaveSectorOrSlot(FULL_SAVE_SLOT, gRamSaveSectorLocations);

        // Save the Hall of Fame
        tempAddr = gDecompressionBuffer;
        HandleWriteSectorNBytes(SECTOR_ID_HOF_1, tempAddr, SECTOR_DATA_SIZE);
        HandleWriteSectorNBytes(SECTOR_ID_HOF_2, tempAddr + SECTOR_DATA_SIZE, SECTOR_DATA_SIZE);
        break;
    case SAVE_NORMAL:
    default:
        CopyPartyAndObjectsToSave();
        WriteSaveSectorOrSlot(FULL_SAVE_SLOT, gRamSaveSectorLocations);
        break;
    case SAVE_LINK:
    case SAVE_EREADER: // Dummied, now duplicate of SAVE_LINK
        // Used by link / Battle Frontier
        // Write only SaveBlocks 1 and 2 (skips the PC)
        CopyPartyAndObjectsToSave();
        for(i = SECTOR_ID_SAVEBLOCK2; i <= SECTOR_ID_SAVEBLOCK1_END; i++)
            HandleReplaceSector(i, gRamSaveSectorLocations);
        for(i = SECTOR_ID_SAVEBLOCK2; i <= SECTOR_ID_SAVEBLOCK1_END; i++)
            WriteSectorSignatureByte_NoOffset(i, gRamSaveSectorLocations);
        break;
    case SAVE_OVERWRITE_DIFFERENT_FILE:
        // Erase Hall of Fame
        for (i = SECTOR_ID_HOF_1; i < SECTORS_COUNT; i++)
            EraseFlashSector(i);

        // Overwrite save slot
        CopyPartyAndObjectsToSave();
        WriteSaveSectorOrSlot(FULL_SAVE_SLOT, gRamSaveSectorLocations);
        break;
    }

#ifdef PORTABLE
	Platform_StoreSaveFile();
#endif

    gTrainerHillVBlankCounter = backupVar;
    return 0;
}

u8 TrySavingData(u8 saveType)
{
    if (gFlashMemoryPresent != TRUE)
    {
        gSaveAttemptStatus = SAVE_STATUS_ERROR;
        return SAVE_STATUS_ERROR;
    }

    HandleSavingData(saveType);
    if (!gDamagedSaveSectors)
    {
#ifdef PORTABLE
		Platform_StoreSaveFile();
#endif
        gSaveAttemptStatus = SAVE_STATUS_OK;
        return SAVE_STATUS_OK;
    }
    else
    {
        DoSaveFailedScreen(saveType);
        gSaveAttemptStatus = SAVE_STATUS_ERROR;
        return SAVE_STATUS_ERROR;
    }
}

bool8 LinkFullSave_Init(void)
{
    if (gFlashMemoryPresent != TRUE)
        return TRUE;
    UpdateSaveAddresses();
    CopyPartyAndObjectsToSave();
    RestoreSaveBackupVarsAndIncrement(gRamSaveSectorLocations);
    return FALSE;
}

bool8 LinkFullSave_WriteSector(void)
{
    u8 status = HandleWriteIncrementalSector(NUM_SECTORS_PER_SLOT, gRamSaveSectorLocations);
    if (gDamagedSaveSectors)
        DoSaveFailedScreen(SAVE_NORMAL);

    // In this case "error" either means that an actual error was encountered
    // or that the given max sector has been reached (meaning it has finished successfully).
    // If there was an actual error the save failed screen above will also be shown.
    if (status == SAVE_STATUS_ERROR)
        return TRUE;
    else
        return FALSE;
}

bool8 LinkFullSave_ReplaceLastSector(void)
{
    HandleReplaceSectorAndVerify(NUM_SECTORS_PER_SLOT, gRamSaveSectorLocations);
    if (gDamagedSaveSectors)
        DoSaveFailedScreen(SAVE_NORMAL);
    return FALSE;
}

bool8 LinkFullSave_SetLastSectorSignature(void)
{
    CopySectorSignatureByte(NUM_SECTORS_PER_SLOT, gRamSaveSectorLocations);
    if (gDamagedSaveSectors)
        DoSaveFailedScreen(SAVE_NORMAL);
    return FALSE;
}

bool8 WriteSaveBlock2(void)
{
    if (gFlashMemoryPresent != TRUE)
        return TRUE;

    UpdateSaveAddresses();
    CopyPartyAndObjectsToSave();
    RestoreSaveBackupVars(gRamSaveSectorLocations);

    // Because RestoreSaveBackupVars is called immediately prior, gIncrementalSectorId will always be 0 below,
    // so this function only saves the first sector (SECTOR_ID_SAVEBLOCK2)
    HandleReplaceSectorAndVerify(gIncrementalSectorId + 1, gRamSaveSectorLocations);
    return FALSE;
}

// Used in conjunction with WriteSaveBlock2 to write both for certain link saves.
// This will be called repeatedly in a task, writing each sector of SaveBlock1 incrementally.
// It returns TRUE when finished.
bool8 WriteSaveBlock1Sector(void)
{
    u8 finished = FALSE;
    u16 sectorId = ++gIncrementalSectorId; // Because WriteSaveBlock2 will have been called prior, this will be SECTOR_ID_SAVEBLOCK1_START
    if (sectorId <= SECTOR_ID_SAVEBLOCK1_END)
    {
        // Write a single sector of SaveBlock1
        HandleReplaceSectorAndVerify(gIncrementalSectorId + 1, gRamSaveSectorLocations);
        WriteSectorSignatureByte(sectorId, gRamSaveSectorLocations);
    }
    else
    {
        // Beyond SaveBlock1, don't write the sector.
        // Does write 1 byte of the next sector's signature field, but as these
        // are the same for all valid sectors it doesn't matter.
        WriteSectorSignatureByte(sectorId, gRamSaveSectorLocations);
        finished = TRUE;
    }

    if (gDamagedSaveSectors)
        DoSaveFailedScreen(SAVE_LINK);

    return finished;
}

u8 LoadGameSave(u8 saveType)
{
    u8 status;

    if (gFlashMemoryPresent != TRUE)
    {
        gSaveFileStatus = SAVE_STATUS_NO_FLASH;
        return SAVE_STATUS_ERROR;
    }

    UpdateSaveAddresses();
    switch (saveType)
    {
    case SAVE_NORMAL:
    default:
#ifdef VER_64BIT
        // Sectors that fail to load keep the current data, like on the GBA
        SaveBlock1ToSaved();
#endif
        status = TryLoadSaveSlot(FULL_SAVE_SLOT, gRamSaveSectorLocations);
#ifdef VER_64BIT
        if (IsOld64BitSaveBlock1(gRamSaveSectorLocations))
            SaveBlock1FromOld64Bit();
        else
            SaveBlock1FromSaved();
#endif
        CopyPartyAndObjectsFromSave();
        gSaveFileStatus = status;
        gGameContinueCallback = NULL;
        break;
    case SAVE_HALL_OF_FAME:
        status = TryLoadSaveSector(SECTOR_ID_HOF_1, gDecompressionBuffer, SECTOR_DATA_SIZE);
        if (status == SAVE_STATUS_OK)
            status = TryLoadSaveSector(SECTOR_ID_HOF_2, &gDecompressionBuffer[SECTOR_DATA_SIZE], SECTOR_DATA_SIZE);
        break;
    }

    return status;
}

u16 GetSaveBlocksPointersBaseOffset(void)
{
    u16 i, slotOffset;
    struct SaveSector *sector;

    sector = gReadWriteSector = &gSaveDataBuffer;
    if (gFlashMemoryPresent != TRUE)
        return 0;
    UpdateSaveAddresses();
    GetSaveValidStatus(gRamSaveSectorLocations);
    slotOffset = NUM_SECTORS_PER_SLOT * (gSaveCounter % NUM_SAVE_SLOTS);
    for (i = 0; i < NUM_SECTORS_PER_SLOT; i++)
    {
        ReadFlashSector(i + slotOffset, gReadWriteSector);

        // Base offset for SaveBlock2 is calculated using the trainer id
        if (gReadWriteSector->id == SECTOR_ID_SAVEBLOCK2)
            return sector->data[offsetof(struct SaveBlock2, playerTrainerId[0])] +
                   sector->data[offsetof(struct SaveBlock2, playerTrainerId[1])] +
                   sector->data[offsetof(struct SaveBlock2, playerTrainerId[2])] +
                   sector->data[offsetof(struct SaveBlock2, playerTrainerId[3])];
    }
    return 0;
}

u32 TryReadSpecialSaveSector(u8 sector, u8 *dst)
{
    s32 i;
    s32 size;
    u8 *savData;

    if (sector != SECTOR_ID_TRAINER_HILL && sector != SECTOR_ID_RECORDED_BATTLE)
        return SAVE_STATUS_ERROR;

    ReadFlash(sector, 0, (u8 *)&gSaveDataBuffer, SECTOR_SIZE);
    if (*(u32 *)(&gSaveDataBuffer.data[0]) != SPECIAL_SECTOR_SENTINEL)
        return SAVE_STATUS_ERROR;

    // Copies whole save sector except u32 counter
    i = 0;
    size = SECTOR_COUNTER_OFFSET - 1;
    savData = &gSaveDataBuffer.data[4]; // data[4] to skip past SPECIAL_SECTOR_SENTINEL
    for (; i <= size; i++)
        dst[i] = savData[i];
    return SAVE_STATUS_OK;
}

u32 TryWriteSpecialSaveSector(u8 sector, u8 *src)
{
    s32 i;
    s32 size;
    u8 *savData;
    void *savDataBuffer;

    if (sector != SECTOR_ID_TRAINER_HILL && sector != SECTOR_ID_RECORDED_BATTLE)
        return SAVE_STATUS_ERROR;

    savDataBuffer = &gSaveDataBuffer;
    *(u32 *)(savDataBuffer) = SPECIAL_SECTOR_SENTINEL;

    // Copies whole save sector except u32 counter
    i = 0;
    size = SECTOR_COUNTER_OFFSET - 1;
    savData = &gSaveDataBuffer.data[4]; // data[4] to skip past SPECIAL_SECTOR_SENTINEL
    for (; i <= size; i++)
        savData[i] = src[i];
    if (ProgramFlashSectorAndVerify(sector, savDataBuffer) != 0)
        return SAVE_STATUS_ERROR;
    return SAVE_STATUS_OK;
}

#define tState         data[0]
#define tTimer         data[1]
#define tInBattleTower data[2]

// Note that this is very different from TrySavingData(SAVE_LINK).
// Most notably it does save the PC data.
void Task_LinkFullSave(u8 taskId)
{
    s16 *data = gTasks[taskId].data;

    switch (tState)
    {
    case 0:
        gSoftResetDisabled = TRUE;
        tState = 1;
        break;
    case 1:
        SetLinkStandbyCallback();
        tState = 2;
        break;
    case 2:
        if (IsLinkTaskFinished())
        {
            if (!tInBattleTower)
                SaveMapView();
            tState = 3;
        }
        break;
    case 3:
        if (!tInBattleTower)
            SetContinueGameWarpStatusToDynamicWarp();
        LinkFullSave_Init();
        tState = 4;
        break;
    case 4:
        if (++tTimer == 5)
        {
            tTimer = 0;
            tState = 5;
        }
        break;
    case 5:
        if (LinkFullSave_WriteSector())
            tState = 6;
        else
            tState = 4; // Not finished, delay again
        break;
    case 6:
        LinkFullSave_ReplaceLastSector();
        tState = 7;
        break;
    case 7:
        if (!tInBattleTower)
            ClearContinueGameWarpStatus2();
        SetLinkStandbyCallback();
        tState = 8;
        break;
    case 8:
        if (IsLinkTaskFinished())
        {
            LinkFullSave_SetLastSectorSignature();
            tState = 9;
        }
        break;
    case 9:
        SetLinkStandbyCallback();
        tState = 10;
        break;
    case 10:
        if (IsLinkTaskFinished())
            tState++;
        break;
    case 11:
        if (++tTimer > 5)
        {
            gSoftResetDisabled = FALSE;
            DestroyTask(taskId);
        }
        break;
    }
}
