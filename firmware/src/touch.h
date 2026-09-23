#ifndef _TOUCH_H
#define _TOUCH_H

#include <stdint.h>

#define TOUCH_FINGERPRINT 0
#define TOUCH_CHOMPY      1
#define TOUCH_COUNT       2

void touch_init(void);

// Samples every pad; call once per main-loop tick. Returns a bitmask of the
// pads that saw a press edge (a tap) this tick, (1 << TOUCH_x).
uint8_t touch_tick(void);

// Bitmask of the pads currently held down (debounced).
uint8_t touch_held(void);

// Low bits of the latest raw readings, as entropy for the RNG seed.
uint32_t touch_noise(void);

#endif
