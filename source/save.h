// save.h -- the meta layer, persisted between sessions.
//
// WHAT IS SAVED
//
// Only the progress that outlives a run: the skill tree and its tokens, the
// field and its seeds, the banked cake ingredients, the day counter, which
// regions have been secured, and the two volume settings. A run itself -- the
// player's level, cards, hearts, position, the monsters -- is deliberately NOT
// saved. Dying is still a soft failure that costs you the expedition and
// nothing else, which is the shape the game already had; the difference is that
// the place you come back to is still there in the morning.
//
// WHY IT MATTERS
//
// The whole farm half of the loop is built on time passing: a crop needs two
// watered nights, the hot spring advances the calendar, and the field panel
// talks about which plots are waiting. Without a save all of that lived in RAM
// and died with the power switch, so the game promised a rhythm it could only
// deliver inside one sitting.
//
// HOW
//
// The cartridge's own save chip, through libnds' card EEPROM API. This is the
// same mechanism a commercial DS game uses to save, so it needs no DLDI patch
// and no filesystem -- which keeps the project's "copy the ROM onto any
// flashcard and play" property intact. The payload is kept under 64 bytes so it
// fits even the smallest chip the API can report (a 4Kbit / 512-byte EEPROM).
//
// Every entry point degrades quietly: on a cartridge with no save chip the
// functions report false and the game plays exactly as it did before.

#pragma once

#include <stdbool.h>

// Is there a usable save chip on this cartridge? Answered once at boot.
bool save_available(void);

// Was the save chip ever actually written to and read back successfully? This is
// what the title screen uses to decide whether to offer "continue", and it is
// deliberately separate from save_available(): an empty chip and an unreadable
// chip are different things and only one of them should offer a resume.
bool save_has_data(void);

// Copy the saved meta layer into the live globals. Returns true when a valid
// save was applied. Called after game_init(), and only when continuing.
bool save_load(void);

// Write the live globals to the save chip. Returns true on success. Cheap
// enough to call at every state transition that changes the meta layer.
bool save_write(void);

// Invalidate the save (a fresh start). The next save_write() rewrites it.
void save_clear(void);
