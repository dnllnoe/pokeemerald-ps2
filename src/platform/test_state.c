#ifdef PORTABLE
#include <stdio.h>
#include "global.h"
#include "global.fieldmap.h"
#include "fieldmap.h"
#include "main.h"
#include "m4a.h"
#include "overworld.h"
#include "palette.h"
#include "script.h"
#include "constants/songs.h"
#include "platform/test_state.h"

// The song the BGM player is playing, or -1
static int GetBgmSong(void)
{
    if (gMPlayInfo_BGM.songHeader == NULL || (gMPlayInfo_BGM.status & MUSICPLAYER_STATUS_PAUSE)
     || !(gMPlayInfo_BGM.status & MUSICPLAYER_STATUS_TRACK))
        return -1;
    // PH_NURSE_SOLO is the last song in the table
    for (int i = 0; i <= PH_NURSE_SOLO; i++)
    {
        if (gSongTable[i].header == gMPlayInfo_BGM.songHeader)
            return i;
    }
    return -1;
}

// What the headless tests' driver needs to know to play: where the player is,
// whether they can move, and what's going on, as one line of JSON
void WriteTestState(FILE *file, u32 frame)
{
    const struct ObjectEvent *player = &gObjectEvents[gPlayerAvatar.objectEventId];
    bool32 first = TRUE;
    // There's no save block until a game is started or continued
    s8 mapGroup = gSaveBlock1Ptr != NULL ? gSaveBlock1Ptr->location.mapGroup : -1;
    s8 mapNum = gSaveBlock1Ptr != NULL ? gSaveBlock1Ptr->location.mapNum : -1;

    fprintf(file, "{\"frame\": %u, \"map\": [%d, %d], \"pos\": [%d, %d], \"facing\": %d, \"moving\": %d, "
                  "\"overworld\": %d, \"locked\": %d, \"script\": %d, \"battle\": %d, \"fade\": %d, \"bgm\": %d, \"objects\": [",
            frame, mapGroup, mapNum,
            player->currentCoords.x - MAP_OFFSET, player->currentCoords.y - MAP_OFFSET, player->facingDirection,
            gPlayerAvatar.tileTransitionState, gMain.callback2 == CB2_Overworld, ArePlayerFieldControlsLocked(),
            ScriptContext_IsEnabled(), gMain.inBattle, gPaletteFade.active, GetBgmSong());

    // Where the other people and things are, since they block the way
    for (int i = 0; i < OBJECT_EVENTS_COUNT; i++)
    {
        const struct ObjectEvent *object = &gObjectEvents[i];

        if (!object->active || object->isPlayer || object->invisible)
            continue;
        fprintf(file, "%s[%d, %d]", first ? "" : ", ", object->currentCoords.x - MAP_OFFSET,
                object->currentCoords.y - MAP_OFFSET);
        first = FALSE;
    }
    fprintf(file, "]}\n");
    fflush(file);
}
#endif // PORTABLE
