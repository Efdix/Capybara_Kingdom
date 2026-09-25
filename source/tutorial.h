// tutorial.h -- the first-run guidance chain.
//
// One bitmask of one-shot beats, saved per file: the seven hub steps that walk
// the seed-to-cake chain (gate -> seed -> plant -> water -> sleep -> harvest ->
// oven), the field beats that teach combat's first minute at the moment it
// happens, and the two acknowledged-once modals (duel card, prologue). One
// hint shows at a time (in the hub's proximity line), each completes itself
// the moment the player performs the action, and a save that already shows
// progress marks the whole chain done -- a veteran never sees a hint, and a
// newcomer is never modal-blocked by one. See game.h for the step/event enums.

#pragma once

#include "game.h"
