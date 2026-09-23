#ifndef _RGB_H
#define _RGB_H

#include <stdint.h>

// The SK6812 chain: the flying car's two headlights and the fingerprint
// scanner indicator (see board.h for which is which).

void rgb_init(void);

// Advances every animation and streams a frame out; call once per tick.
// Returns 1 on the tick a fingerprint scan completes (turns green).
uint8_t rgb_tick(void);

// ---------- headlights ----------

// Chompy tapped: normal -> hazards -> rainbow (once unlocked) -> normal.
// In police lights: toggles to the color crossfade and back once that is
// unlocked (UNLOCK_HACKUCF_FADE), else back to normal.
void rgb_mode_next(void);

// Chompy held: switches between the tap modes (normal, hazards, rainbow)
// and the hold modes (police, color crossfade), returning to whichever
// mode was last used on that side -- police the first time.
void rgb_mode_hold(void);

// Rainbow mode is unlocked by solving the adventure.
uint8_t rgb_rainbow_unlocked(void);

// Switch straight to rainbow mode (the moment it's unlocked).
void rgb_mode_rainbow(void);

// Blink both headlights `count` times in the given color, overriding the
// mode -- a quick acknowledgement for events (SAO plugged in, challenge
// solved, ...).
void rgb_flash(uint8_t r, uint8_t g, uint8_t b, uint8_t count);

// A brief driving signal over the current mode, for moves in the adventure:
// two blinks of one turn signal or of the hazards, or a double high-beam
// flash. Left/right as seen looking at the badge.
#define RGB_SIGNAL_LEFT    0
#define RGB_SIGNAL_RIGHT   1
#define RGB_SIGNAL_BRIGHTS 2
#define RGB_SIGNAL_HAZARDS 3
void rgb_signal(uint8_t signal);

// Self-test frame instead of the normal animations: everything dim white,
// the scanner red and/or the headlights green on request. active = 0 goes
// back to normal; animations resume where they left off.
void rgb_selftest(uint8_t active, uint8_t scanner_red, uint8_t headlights_green);

// ---------- scanner ----------

#define SCAN_IDLE     0   // dim, slow blue breathing
#define SCAN_BUSY     1   // fast blue pulsing, 5-10 s, while the finger stays down
#define SCAN_VERIFIED 2   // scan passed: a few green flashes, then idle
#define SCAN_FAILED   3   // finger lifted mid-scan: a few red flashes, then idle

// Finger placed on the pad (press edge): start a scan.
void rgb_scan_start(void);

// Current finger state, every tick: lifting it mid-scan fails the scan.
void rgb_scan_finger(uint8_t down);

// Idle breathing in gold (1) instead of blue (0): the Hack@UCF unlock.
void rgb_scan_gold(uint8_t on);
uint8_t rgb_scan_state(void);

#endif
