// BSides Orlando 2026 badge -- CH32V003 firmware for the production (R1) PCB.
//
// What the badge does (pin map in board.h):
//   - Tower spire LED on, except while an SAO is attached (it sits right by
//     the SAO connector); the geodesic sphere keeps one of its three
//     LEDs lit, handing off every few seconds -- more of them for every CTF
//     challenge solved (beacons.c, ctf.c).
//   - Fingerprint pad: hold a finger on it and the scanner LED pulses blue
//     through a 5-10 s "scan", then flashes green: clearance verified.
//     Lifting early fails the scan with red flashes (rgb.c).
//   - Lil Chompy's head: tap to cycle the flying car's headlight modes
//     (normal, hazards, and rainbow once a challenge is solved), hold for
//     police lights (rgb.c). A Hack@UCF SAO unlocks a color crossfade.
//   - SAO connector: SAOv3 host on I2C (sao_host.c), plus a serial console on
//     the GPIO pins running a text adventure CTF (console.c, game.c).
//   - Post-flash self-test, triggered over the console RX pin (selftest.h).

#include "ch32fun.h"

#include "beacons.h"
#include "console.h"
#include "ctf.h"
#include "game.h"
#include "rgb.h"
#include "sao_host.h"
#include "selftest.h"
#include "settings.h"
#include "touch.h"
#include "util.h"

// ---------- RNG ----------

static uint32_t rng_state = 1;

void rand_seed(uint32_t seed)
{
	rng_state = seed ? seed : 1;   // xorshift has a fixed point at zero
}

uint32_t rand_u32(void)
{
	uint32_t x = rng_state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	rng_state = x;
	return x;
}

// ---------- HackUCF SAO ----------
// When a Hack@UCF SAO shows up: a gold flash of the headlights, and a
// permanent unlock of the color crossfade and a gold fingerprint-scanner
// idle.
static void hackucf_tick(void)
{
	static uint8_t present;
	uint8_t now = sao_host_find(SAO_VID_HACKUCF, SAO_PID_HACKUCF);

	if (now && !present) {
		rgb_flash(255, 170, 0, 3);
		if (!(settings.unlocks & UNLOCK_HACKUCF_FADE)) {
			settings.unlocks |= UNLOCK_HACKUCF_FADE;
			settings_save();
		}
	}

	rgb_scan_gold(settings.unlocks & UNLOCK_HACKUCF_FADE);
	present = now;
}

// ---------- Lil Chompy's head ----------
// A tap acts on release, so the start of a hold doesn't also count as a tap;
// a hold acts once, when it reaches CHOMPY_HOLD_TICKS.

#define CHOMPY_HOLD_TICKS MS_TICKS(1000)

// Ticks the pad has been held, 0 when released. Parked past the threshold
// when a press must be ignored until it lets go (see selftest_tick).
static uint16_t chompy_ticks;

static void chompy_tick(uint8_t taps)
{
	// The adventure's "wake Chompy" reacts to any touch, right away.
	if (taps & (1u << TOUCH_CHOMPY))
		game_on_chompy_tap();

	if (touch_held() & (1u << TOUCH_CHOMPY)) {
		if (chompy_ticks <= CHOMPY_HOLD_TICKS && ++chompy_ticks == CHOMPY_HOLD_TICKS)
			rgb_mode_hold();
		return;
	}

	if (chompy_ticks && chompy_ticks < CHOMPY_HOLD_TICKS)
		rgb_mode_next();
	chompy_ticks = 0;
}

// ---------- self-test ----------

#define PADS_BOTH ((1u << TOUCH_FINGERPRINT) | (1u << TOUCH_CHOMPY))

static uint8_t selftest;

void selftest_start(void)
{
	selftest = 1;
	beacons_force_all(1);
	rgb_selftest(1, 0, 0);
}

// Runs instead of the normal touch handling while in self-test, so no press
// made here starts a scan or changes the headlight mode.
static void selftest_tick(void)
{
	uint8_t held = touch_held();

	if (held == PADS_BOTH) {
		selftest = 0;
		beacons_force_all(0);
		rgb_selftest(0, 0, 0);
		// The exit press is still down; don't let its release count as a tap.
		chompy_ticks = CHOMPY_HOLD_TICKS + 1;
		return;
	}

	rgb_selftest(1, held & (1u << TOUCH_FINGERPRINT), held & (1u << TOUCH_CHOMPY));
}

// ---------- main ----------

int main(void)
{
	SystemInit();

	settings_load();

	beacons_init();
	touch_init();
	rand_seed(ESIG->UNIID1 ^ ESIG->UNIID2 ^ ESIG->UNIID3 ^ touch_noise());
	ctf_init();
	rgb_init();
	sao_host_init();
	con_init();

	while (1) {
		uint8_t taps = touch_tick();

		if (selftest) {
			selftest_tick();
		}
		else {
			if (taps & (1u << TOUCH_FINGERPRINT))
				rgb_scan_start();
			rgb_scan_finger(touch_held() & (1u << TOUCH_FINGERPRINT));

			chompy_tick(taps);
		}

		if (rgb_tick())
			game_on_scan_verified();

		beacons_set_spire(sao_host_count() == 0);
		beacons_tick();

		// Discovery scans, liveness checks, and (when taken over) SAO LED
		// mirroring; rate-limited internally.
		sao_host_task(TICK_MS);
		hackucf_tick();

		con_tick();
		settings_tick();

		Delay_Ms(TICK_MS);
	}
}
