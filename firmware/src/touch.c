// Capacitive touch pads: ADC-slope sensing via ch32fun's touch driver.
// TOUCH_SLOPE is 1, so a press reads *higher* than the baseline.
//
// One read is 3 iterations x 3 inner samples = 9 summed 10-bit conversions,
// so values sit in 0..~9200. The thresholds are guesses until they meet the
// real pads -- enable TOUCH_DEBUG and watch the SWIO console to tune them.
// The Chompy pad is much smaller than the fingerprint, so expect it to want
// a lower threshold.

#include "ch32fun.h"

#include "board.h"
#include "touch.h"

#include "ch32v00x_touch.h"

#define TOUCH_ITERATIONS 3
#define TOUCH_DEBOUNCE   3    // consecutive ticks to accept a state change
//#define TOUCH_DEBUG

struct pad {
	uint8_t pin;
	uint8_t adc;
	uint16_t threshold;
};

static const struct pad pads[TOUCH_COUNT] = {
	[TOUCH_FINGERPRINT] = { TOUCH_FINGERPRINT_PIN, TOUCH_FINGERPRINT_ADC, 300 },
	[TOUCH_CHOMPY]      = { TOUCH_CHOMPY_PIN,      TOUCH_CHOMPY_ADC,      200 },
};

static uint32_t baseline[TOUCH_COUNT];
static uint8_t  streak[TOUCH_COUNT];
static uint8_t  held;       // debounced state, one bit per pad
static uint32_t noise;

static uint32_t pad_read(int i)
{
	uint32_t v = ReadTouchPin(GPIOA, pads[i].pin, pads[i].adc, TOUCH_ITERATIONS);

	noise = (noise << 3) ^ v;
	return v;
}

void touch_init(void)
{
	RCC->APB2PCENR |= RCC_APB2Periph_GPIOA | RCC_APB2Periph_ADC1;
	InitTouchADC();

	// Settle, then average a handful of reads for the untouched baseline.
	// (If a pad is held during power-on, the drift tracker recovers once it
	// is released.)
	for (int i = 0; i < TOUCH_COUNT; i++) {
		uint32_t sum = 0;
		for (int n = 0; n < 8; n++) {
			uint32_t v = pad_read(i);
			if (n >= 4)
				sum += v;
		}
		baseline[i] = sum / 4;
	}
}

uint8_t touch_tick(void)
{
	uint8_t taps = 0;

#ifdef TOUCH_DEBUG
	static uint8_t dbg_div;
	uint8_t dbg = (++dbg_div >= 50);
	if (dbg)
		dbg_div = 0;
#endif

	for (int i = 0; i < TOUCH_COUNT; i++) {
		uint8_t bit = (uint8_t)(1u << i);
		uint32_t v = pad_read(i);
		uint8_t raw = (v > baseline[i] + pads[i].threshold) ? bit : 0;

#ifdef TOUCH_DEBUG
		if (dbg)
			printf("touch%d %d base %d\n", i, (int)v, (int)baseline[i]);
#endif

		// Track slow drift (temperature, humidity) while untouched: glide
		// the baseline toward the reading by 1/64 of the difference.
		if (!raw && !(held & bit))
			baseline[i] += ((int32_t)v - (int32_t)baseline[i]) / 64;

		if (raw == (held & bit)) {
			streak[i] = 0;
			continue;
		}

		if (++streak[i] < TOUCH_DEBOUNCE)
			continue;

		streak[i] = 0;
		held ^= bit;
		taps |= raw;   // only on the press edge
	}

	return taps;
}

uint8_t touch_held(void)
{
	return held;
}

uint32_t touch_noise(void)
{
	return noise;
}
