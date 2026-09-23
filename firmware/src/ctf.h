#ifndef _CTF_H
#define _CTF_H

#include <stdint.h>

// CTF challenge bookkeeping. Each challenge owns one bit of settings.solved.
// Flags are submitted on the CTFd site, not to the badge; the badge only
// shows its own flag when it's earned, and rewards the solve with lights:
// another sphere LED lit, and rainbow headlights unlocked (and switched on).
//
// The adventure prints its flag, so the firmware holds an XOR-obfuscated copy
// (scripts/ctf_flag.py). That only keeps it out of `strings`: anyone who
// reverses the firmware can decode it. Enable flash read protection on
// production badges if that matters -- SWIO is on the SAO connector.

#define CTF_ADVENTURE 0   // beat the text adventure on the UART console
#define CTF_COUNT     1   // up to 7: bit 7 of settings.unlocks is taken

#define CTF_BIT(id) (1u << (id))

const char *ctf_name(uint8_t id);

uint8_t ctf_is_solved(uint8_t id);

// Records a solve: persists it, updates the sphere, flashes the headlights
// and switches them to rainbow. No-op if already solved.
void ctf_mark_solved(uint8_t id);

// Writes a challenge's decoded flag to the console.
void ctf_print_flag(uint8_t id);

// Applies saved solves to the sphere; call once after settings load.
void ctf_init(void);

// The PEEK-only flag's page (64 bytes, page-aligned); see ctf.c.
extern const char ctf_mirror_flag[64];

#endif
