#ifndef _SETTINGS_H
#define _SETTINGS_H

#include <stdint.h>

// Persistent state, kept in the last 64-byte page of the 16K flash. The
// linker script is trimmed so code can never land in that page (see
// scripts/gen_ldscript.py), and the programmer never erases it.
//
// Page layout (settings.c): the 8-byte magic "BSO_2026" (just a sentinel
// that the page has been provisioned), then this struct, then 0xFF.

struct __attribute__((packed)) settings {
	uint8_t unlocks;   // bits 0..CTF_COUNT-1: CTF solves (CTF_BIT); UNLOCK_* above
};

// A Hack@UCF SAO has been attached at least once: color crossfade mode and
// a gold fingerprint-scanner idle.
#define UNLOCK_HACKUCF_FADE 0x80

extern struct settings settings;

// Loads from flash, falling back to defaults (all zero) on a blank or stale
// page.
void settings_load(void);

// Writes now, if anything differs from what is in flash.
void settings_save(void);

// Writes a few seconds from now, restarting the countdown on every call --
// spares the flash page from a tap-happy thumb.
void settings_save_later(void);

// Called every main-loop tick; performs a deferred save when it comes due.
void settings_tick(void);

#endif
