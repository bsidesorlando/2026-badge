#ifndef _SELFTEST_H
#define _SELFTEST_H

// Post-flash self-test, triggered over the console's RX pin (PD1 -- the
// same pin the programmer flashes through), so it works with a programmer
// that has nothing else connected. Either trigger works, console awake or
// not:
//   - the single byte SELFTEST_BYTE, or
//   - the line "selftest" (CR and/or LF terminated).
//
// In self-test every self-blinking LED is on and the SK6812s are dim white.
// Holding the fingerprint pad turns the scanner red; holding Chompy's head
// turns the headlights green. Holding both at once exits to normal
// operation.

#define SELFTEST_BYTE 0xB5

void selftest_start(void);

#endif
