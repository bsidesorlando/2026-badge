#ifndef _SAO_HOST_H
#define _SAO_HOST_H

#include <stdint.h>

// SAOv3 host: discovery (static magic + SMBus ARP), enumeration, and --
// on request -- LED mirroring onto attached SAOs that expose the LED class
// interface.
//
// Everything runs from the main loop -- sao_host_task() is called once per
// tick and internally rate-limits bus traffic. Individual SMBus transfers
// block for their duration (bounded by the I2C layer's timeouts), which at
// 100 kHz is at most a few ms -- short enough that the LED animation tick
// never visibly stalls.

void sao_host_init(void);

// Called every main-loop tick with the tick period in ms.
void sao_host_task(uint32_t tick_ms);

// Latest headlight colors, RGB per LED, already brightness-scaled. While a
// takeover is active, the task mirrors these onto SAO LEDs at its own
// (slower) cadence.
void sao_host_set_headlights(const uint8_t rgb[2][3]);

// Take control of (1) or hand back (0) the LEDs of every LED-capable SAO,
// including ones plugged in later. Off at boot.
void sao_host_set_led_takeover(uint8_t enable);

// Number of SAOs currently known (enumerated or about to be).
uint8_t sao_host_count(void);

// 1 if an enumerated SAO reports this VID/PID.
uint8_t sao_host_find(uint16_t vid, uint16_t pid);

// HackUCF SAO (~/Projects/Embedded/Badges/HackUCF_SAO, src/saod_config.c).
#define SAO_VID_HACKUCF 0x0102
#define SAO_PID_HACKUCF 0x0001

#endif
