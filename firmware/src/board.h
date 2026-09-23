#ifndef _BOARD_H
#define _BOARD_H

// Pin map for the production (R1) PCB. See hardware/bsidesorl-v1.kicad_sch.

// SK6812MINI-E chain on PC6 (SPI1 MOSI, streamed by DMA1 channel 3). Chain
// order is D1 -> D2 -> D3.
#define RGB_HEADLIGHT_L 0    // D1, flying car headlight (left, facing the badge)
#define RGB_HEADLIGHT_R 1    // D2, flying car headlight (right)
#define RGB_SCANNER     2    // D3, the "SCANNING IN PROGRESS" indicator
#define RGB_COUNT       3

// Self-blinking 3mm LEDs. Each MCU pin sits in the LED's ground return path,
// so these are open drain: pull low to light, release to turn off.
#define PIN_SPIRE   PD4      // D5, breathing LED at the top of the tower spire
// D6-D8, fast-blink LEDs on the geodesic sphere:
#define PIN_SPHERE0 PC4      // D6
#define PIN_SPHERE1 PC5      // D7
#define PIN_SPHERE2 PC3      // D8

// Capacitive touch pads (ADC slope sensing).
#define TOUCH_FINGERPRINT_PIN 2    // PA2, ADC channel 0
#define TOUCH_FINGERPRINT_ADC 0
#define TOUCH_CHOMPY_PIN      1    // PA1, ADC channel 1 (Lil Chompy's head)
#define TOUCH_CHOMPY_ADC      1

// SAO connector:
//   PC1/PC2 -> SDA/SCL (badge is the SMBus master)
//   PD0 -> GPIO1, doubles as the console's UART TX (USART1 partial remap 1)
//   PD1 -> GPIO2, doubles as the console's UART RX -- and is SWIO (debug)

#endif
