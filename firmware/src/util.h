#ifndef _UTIL_H
#define _UTIL_H

#include <stdint.h>

// Everything ticks at the main loop's 10 ms cadence.
#define TICK_MS 10
#define MS_TICKS(ms) ((ms) / TICK_MS)

// xorshift32. Seeded from the chip's unique ID plus touch-pad noise, so
// badges do not all run the same "random" sequence in lockstep.
void rand_seed(uint32_t seed);
uint32_t rand_u32(void);

// Uniform-enough value in [0, n) for small n.
static inline uint32_t rand_below(uint32_t n)
{
	return rand_u32() % n;
}

#endif
