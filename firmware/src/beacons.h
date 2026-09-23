#ifndef _BEACONS_H
#define _BEACONS_H

#include <stdint.h>

// The four self-blinking LEDs: the tower spire, and the three on the
// geodesic sphere. The LEDs do their own blinking/breathing; firmware only
// decides which ones are powered.
//
// The spire is on unless switched off (while an SAO is attached, see
// main.c). The sphere keeps `lit` of its three LEDs on at a time, handing
// off to a new one every few seconds (the new one lights shortly before the
// old one goes dark). With all three lit there is nothing to hand off, so
// they simply stay on.

void beacons_init(void);

// Number of sphere LEDs to keep lit, 1..3.
void beacons_set_lit(uint8_t lit);

// Called every main-loop tick.
void beacons_tick(void);

// Spire on/off; applied on the next tick. Self-test overrides it.
void beacons_set_spire(uint8_t on);

// Self-test: all LEDs on (1), or back to normal rotation (0).
void beacons_force_all(uint8_t on);

#endif
