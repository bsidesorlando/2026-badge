// Tower spire + geodesic sphere LEDs (see beacons.h).

#include "ch32fun.h"

#include "beacons.h"
#include "board.h"
#include "util.h"

// How often the sphere hands off to a new LED, and how long the old one
// stays lit alongside it.
#define HANDOFF_PERIOD_TICKS  MS_TICKS(3000)
#define HANDOFF_OVERLAP_TICKS MS_TICKS(500)

#define SPHERE_COUNT 3

static const uint8_t sphere_pins[SPHERE_COUNT] = { PIN_SPHERE0, PIN_SPHERE1, PIN_SPHERE2 };

// Lit sphere LEDs, oldest first.
static uint8_t lit_queue[SPHERE_COUNT];
static uint8_t n_lit;
static uint8_t target_lit = 1;

static uint16_t period_ticks;
static uint16_t overlap_ticks;   // nonzero while a handoff is in flight
static uint8_t  forced;          // self-test: everything on, rotation paused
static uint8_t  spire_on = 1;

// Open drain, low = lit. Set the output latch before switching the pin to an
// output so nothing flashes on during init.
static void led_pin_init(uint8_t pin)
{
	funDigitalWrite(pin, FUN_HIGH);
	funPinMode(pin, GPIO_CFGLR_OUT_10Mhz_OD);
}

static void sphere_add_random(void)
{
	uint8_t mask = 0;

	if (n_lit >= SPHERE_COUNT)
		return;

	for (uint8_t i = 0; i < n_lit; i++)
		mask |= (uint8_t)(1u << lit_queue[i]);

	// Pick uniformly among the dark ones.
	uint8_t pick = (uint8_t)rand_below(SPHERE_COUNT - n_lit);
	for (uint8_t i = 0; i < SPHERE_COUNT; i++) {
		if (mask & (1u << i))
			continue;
		if (pick-- == 0) {
			funDigitalWrite(sphere_pins[i], FUN_LOW);
			lit_queue[n_lit++] = i;
			return;
		}
	}
}

static void sphere_drop_oldest(void)
{
	funDigitalWrite(sphere_pins[lit_queue[0]], FUN_HIGH);

	n_lit--;
	for (uint8_t i = 0; i < n_lit; i++)
		lit_queue[i] = lit_queue[i + 1];
}

void beacons_init(void)
{
	RCC->APB2PCENR |= RCC_APB2Periph_GPIOC | RCC_APB2Periph_GPIOD;

	led_pin_init(PIN_SPIRE);
	for (uint8_t i = 0; i < SPHERE_COUNT; i++)
		led_pin_init(sphere_pins[i]);

	funDigitalWrite(PIN_SPIRE, FUN_LOW);
}

void beacons_set_lit(uint8_t lit)
{
	if (lit < 1)
		lit = 1;
	if (lit > SPHERE_COUNT)
		lit = SPHERE_COUNT;
	target_lit = lit;
}

void beacons_set_spire(uint8_t on)
{
	spire_on = on;
}

void beacons_force_all(uint8_t on)
{
	forced = on;
	funDigitalWrite(PIN_SPIRE, (on || spire_on) ? FUN_LOW : FUN_HIGH);
	for (uint8_t i = 0; i < SPHERE_COUNT; i++)
		funDigitalWrite(sphere_pins[i], on ? FUN_LOW : FUN_HIGH);

	// Leaving: start over from all dark; the next tick lights the target.
	n_lit = 0;
	period_ticks = 0;
	overlap_ticks = 0;
}

void beacons_tick(void)
{
	if (forced)
		return;

	funDigitalWrite(PIN_SPIRE, spire_on ? FUN_LOW : FUN_HIGH);

	if (overlap_ticks) {
		if (--overlap_ticks == 0)
			sphere_drop_oldest();
	}
	else if (target_lit < SPHERE_COUNT && ++period_ticks >= HANDOFF_PERIOD_TICKS) {
		period_ticks = 0;
		sphere_add_random();
		overlap_ticks = HANDOFF_OVERLAP_TICKS;
	}

	// Converge on the target count (boot, or a newly solved challenge).
	// Mid-handoff there is one extra LED lit on purpose; leave it be.
	if (!overlap_ticks) {
		while (n_lit > target_lit)
			sphere_drop_oldest();
	}
	while (n_lit < target_lit)
		sphere_add_random();
}
