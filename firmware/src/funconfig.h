#ifndef _FUNCONFIG_H
#define _FUNCONFIG_H

// Place configuration items here, you can see a full list in ch32fun/ch32fun.h
// Defaults: internal RC at 48MHz, printf over the SWIO debug link.

// The fatal-error dump costs ~170 bytes of a nearly full 16K part and nobody
// watches the SWIO console on a badge in the wild. Flip back to 1 on the bench
// when chasing a crash.
#define FUNCONF_DEBUG_HARDFAULT 0

#endif
