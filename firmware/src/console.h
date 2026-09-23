#ifndef _CONSOLE_H
#define _CONSOLE_H

#include <stdint.h>

// Serial console on the SAO connector: 115200 8N1, badge TX on GPIO1 (PD0),
// RX on GPIO2 (PD1). Plug a 3.3V USB-serial adapter into the SAO header and
// press Enter.
//
// The SAO spec wants an un-negotiated host to drive nothing on the GPIOs,
// so TX stays tristated until the console wakes: that takes a clean CR or
// LF on RX while no SAO answers on I2C. It stays awake until an SAO shows
// up (or the badge resets).
//
// PD1 is also SWIO. RX only ever listens on it, so debugging keeps working
// -- expect the console to see noise while a probe is talking.

void con_init(void);

// Line editing and dispatch; call once per main-loop tick.
void con_tick(void);

uint8_t con_active(void);

// Output is dropped while the console is asleep. Newlines are expanded to
// CR LF. Writes block only when the TX buffer fills.
void con_putc(char c);
void con_puts(const char *s);
void con_putu(uint32_t v);
void con_puthex(uint32_t v, uint8_t digits);   // lowercase, zero-padded

// Finishes sending queued output, then resets the badge.
void con_reboot(void) __attribute__((noreturn));

#endif
