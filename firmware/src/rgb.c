// SK6812 chain: headlight patterns and the fingerprint scanner indicator.

#include "ch32fun.h"

#include "board.h"
#include "ctf.h"
#include "rgb.h"
#include "sao_host.h"
#include "settings.h"
#include "util.h"

#include <string.h>

// A frame needs WS2812B_RESET_PERIOD + RGB_COUNT = 2 + 3 = 5 DMA slots and
// the default DMALEDS of 16 provides 8, so WS2812BDMAStart() fills the whole
// frame synchronously and the mid-frame refill ISR never has anything to do.
#define WS2812DMA_IMPLEMENTATION
#define DMALEDS 16

#include "ws2812b_dma_spi_led_driver.h"

// ---------- LED frame plumbing ----------
// The driver emits the 24-bit callback value MSB-first as bytes
// [23:16],[15:8],[7:0]. SK6812 takes them in G,R,B order, so pack G,R,B.
#define LED_GRB(r, g, b) (((uint32_t)(g) << 16) | ((uint32_t)(r) << 8) | (uint32_t)(b))

// Peak brightness, 0..255, applied to every frame (patterns, scanner,
// self-test, event flashes). Everything runs off one AAA cell through a
// boost converter, and the SK6812MINI-Es are plenty bright well below full
// scale: 96 read as too bright on the first assembled badges.
#define HEADLIGHT_BRIGHTNESS 48
#define SCANNER_BRIGHTNESS   48

// Current frame, RGB per LED, after brightness scaling.
static volatile uint8_t led_frame[RGB_COUNT][3];

uint32_t WS2812BLEDCallback(int ledno)
{
	const volatile uint8_t *px = led_frame[ledno];

	return LED_GRB(px[0], px[1], px[2]);
}

// Rounds rather than truncates: frames are scaled twice (color level, then
// the brightness cap), and at the dim end two truncations visibly shift
// both brightness and hue.
static uint32_t scale8(uint32_t component, uint32_t level)
{
	return (component * level + 128) >> 8;
}

// 16-bit phase -> 0..255..0 triangle.
static uint8_t tri8(uint16_t phase)
{
	uint16_t p = phase >> 7;

	return (uint8_t)(p < 256 ? p : 511 - p);
}

static void set_px(uint8_t px[3], uint8_t r, uint8_t g, uint8_t b)
{
	px[0] = r;
	px[1] = g;
	px[2] = b;
}

// ---------- HSV ----------
// The Adafruit NeoPixel formula: hue 0..65535, sat/val 0..255. All integer;
// the RV32EC has no hardware multiply but this only runs in the 10 ms tick.
static void color_hsv(uint16_t hue, uint8_t sat, uint8_t val, uint8_t px[3])
{
	uint32_t h = ((uint32_t)hue * 1530u + 32768u) >> 16;
	uint32_t r, g, b;

	if (h < 510) {
		b = 0;
		if (h < 255) { r = 255;     g = h; }
		else         { r = 510 - h; g = 255; }
	}
	else if (h < 1020) {
		r = 0;
		if (h < 765) { g = 255;      b = h - 510; }
		else         { g = 1020 - h; b = 255; }
	}
	else if (h < 1530) {
		g = 0;
		if (h < 1275) { r = h - 1020; b = 255; }
		else          { r = 255;      b = 1530 - h; }
	}
	else {
		r = 255; g = 0; b = 0;
	}

	uint32_t v1 = 1u + val;
	uint32_t s1 = 1u + sat;
	uint32_t s2 = 255u - sat;

	px[0] = (uint8_t)(((((r * s1) >> 8) + s2) * v1) >> 8);
	px[1] = (uint8_t)(((((g * s1) >> 8) + s2) * v1) >> 8);
	px[2] = (uint8_t)(((((b * s1) >> 8) + s2) * v1) >> 8);
}

// ---------- headlights ----------
// The two LEDs are a flying car's headlights. Tapping Lil Chompy's head
// cycles the modes (normal -> hazards -> rainbow, once unlocked); holding it
// toggles police lights. The badge always starts in normal mode.
//
// Normal mode is dim warm white, and every GAP (8-12 s) something happens:
// usually one headlight blinks orange as a turn signal for 5-10 s, sometimes
// both flash their high beams instead.
//
// Holding switches to police lights, and holding again returns to where the
// tap cycle was left (see tap_group_mode).
// Once a Hack@UCF SAO has ever been attached, a tap in police lights
// switches to a color crossfade and back: the headlights take turns, one
// fading in on a fresh random hue while the other fades out.
#define MODE_NORMAL     0
#define MODE_HAZARD     1
#define MODE_RAINBOW    2   // needs the adventure solved (rgb_rainbow_unlocked)
#define MODE_POLICE     3   // hold Chompy's head
#define MODE_HUE_FADE   4   // needs UNLOCK_HACKUCF_FADE

static uint8_t mode = MODE_NORMAL;

// The modes form two groups: taps cycle within one, a hold switches to the
// other. Each group remembers where it was left, so a hold returns to the
// mode last used there (police the first time).
static uint8_t tap_group_mode  = MODE_NORMAL;   // normal, hazards, rainbow
static uint8_t hold_group_mode = MODE_POLICE;   // police, color crossfade

// Warm yellow-orange headlights and signal amber -- tune on real LEDs, the
// SK6812MINI-E green channel runs hot. On the wire: low beams (8,4,1), high
// beams (48,24,3), signal amber (48,18,0).
#define WARM_R 255
#define WARM_G 128
#define WARM_B 16

#define AMBER_R 255
#define AMBER_G 96
#define AMBER_B 0

// Low beams: the warm white's brightest channel at about LOW_BEAM_OUT/255 on
// the wire, after HEADLIGHT_BRIGHTNESS. High beams are the same color at the
// full HEADLIGHT_BRIGHTNESS cap.
#define LOW_BEAM_OUT   8
#define LOW_BEAM_LEVEL (LOW_BEAM_OUT * 256 / HEADLIGHT_BRIGHTNESS)

// Normal-mode events.
#define EV_GAP      0   // steady low beams, waiting for the next event
#define EV_TURN     1   // one side blinking orange
#define EV_HIGHBEAM 2   // both flash high beams

#define GAP_MIN_TICKS      MS_TICKS(8000)
#define GAP_SPREAD_TICKS   MS_TICKS(4000)
#define TURN_MIN_TICKS     MS_TICKS(5000)
#define TURN_SPREAD_TICKS  MS_TICKS(5000)
#define HIGHBEAM_ONE_IN    4               // chance an event is high beams, 1 in N

static uint8_t  ev = EV_GAP;
static uint8_t  ev_left;      // EV_TURN: which side is signaling
static uint16_t ev_ticks;     // remaining in this gap/event

// hazard/turn: automotive flasher cadence, roughly 85 flashes per minute.
#define FLASH_HALF_TICKS MS_TICKS(350)
static uint8_t  flash_phase;   // 0/1, flips every FLASH_HALF_TICKS
static uint16_t flash_ticks;

// police/high beams: fixed-rate frame sequences, one bitmask per step:
// bit0 = left lit, bit1 = right lit.
#define POLICE_STEP_TICKS MS_TICKS(60)
static const uint8_t police_frames[] = { 1, 0, 1, 0, 2, 0, 2, 0 };

#define HIGHBEAM_STEP_TICKS MS_TICKS(150)
static const uint8_t highbeam_frames[] = { 3, 0, 3 };   // flash-to-pass, twice

static uint8_t  seq_idx;
static uint16_t seq_ticks;

// rainbow: full wheel in ~2.6 s.
#define RAINBOW_HUE_STEP 256
static uint16_t anim_hue;

// crossfade: fade_side fades in on fade_hue[fade_side] while the other side
// fades out, over FADE_TICKS; then they swap.
#define FADE_TICKS MS_TICKS(1000)
static uint8_t  fade_side;
static uint8_t  fade_first;    // first fade after entering: the other side is dark
static uint16_t fade_ticks;
static uint16_t fade_hue[2];

// Flash overlay (rgb_flash, rgb_signal): `count` flashes of one color on the
// sides in evflash_mask (bit i = frame[i]), each lit for evflash_half ticks
// and then dark for as long. The other side keeps rendering its mode.
#define EVFLASH_HALF_TICKS MS_TICKS(100)
static uint8_t evflash_rgb[3];
static uint8_t evflash_mask;
static uint8_t evflash_half;     // ticks per half-period
static uint8_t evflash_halves;   // remaining half-periods; even = lit
static uint8_t evflash_ticks;

uint8_t rgb_rainbow_unlocked(void)
{
	return (settings.unlocks & CTF_BIT(CTF_ADVENTURE)) != 0;
}

static uint8_t hue_fade_unlocked(void)
{
	return (settings.unlocks & UNLOCK_HACKUCF_FADE) != 0;
}

// Hand the fade-in to `side` with a fresh random hue, at least a sixth of
// the wheel away from the other side's, so the two never look alike.
static void fade_start(uint8_t side)
{
	fade_side = side;
	fade_ticks = 0;
	fade_hue[side] = (uint16_t)(fade_hue[side ^ 1] + 10923 + rand_below(65536 - 2 * 10923));
}

static void ev_start(uint8_t which)
{
	ev = which;
	seq_idx = 0;
	seq_ticks = 0;
	flash_phase = 1;   // a signal starts lit
	flash_ticks = 0;

	switch (which) {
	case EV_TURN:
		ev_left = (uint8_t)rand_below(2);
		ev_ticks = (uint16_t)(TURN_MIN_TICKS + rand_below(TURN_SPREAD_TICKS));
		break;
	case EV_HIGHBEAM:
		ev_ticks = (uint16_t)(sizeof(highbeam_frames) * HIGHBEAM_STEP_TICKS);
		break;
	default:
		ev_ticks = (uint16_t)(GAP_MIN_TICKS + rand_below(GAP_SPREAD_TICKS));
		break;
	}
}

static void mode_set(uint8_t m)
{
	mode = m;
	ev_start(EV_GAP);   // resets the flasher and sequences for every mode
	if (m == MODE_HUE_FADE) {
		fade_first = 1;
		fade_start(RGB_HEADLIGHT_L);
	}
}

void rgb_mode_next(void)
{
	switch (mode) {
	case MODE_NORMAL:
		mode_set(MODE_HAZARD);
		break;
	case MODE_HAZARD:
		mode_set(rgb_rainbow_unlocked() ? MODE_RAINBOW : MODE_NORMAL);
		break;
	case MODE_POLICE:
		mode_set(hue_fade_unlocked() ? MODE_HUE_FADE : MODE_NORMAL);
		break;
	case MODE_HUE_FADE:
		mode_set(MODE_POLICE);
		break;
	default:   // rainbow
		mode_set(MODE_NORMAL);
		break;
	}
}

void rgb_mode_hold(void)
{
	if (mode == MODE_POLICE || mode == MODE_HUE_FADE) {
		hold_group_mode = mode;
		mode_set(tap_group_mode);
	}
	else {
		tap_group_mode = mode;
		mode_set(hold_group_mode);
	}
}

static void seq_tick(uint16_t step_ticks, uint8_t len)
{
	if (++seq_ticks >= step_ticks) {
		seq_ticks = 0;
		seq_idx = (uint8_t)((seq_idx + 1u) % len);
	}
}

// Returns 1 when a blink turns on.
static uint8_t flash_tick(void)
{
	if (++flash_ticks < FLASH_HALF_TICKS)
		return 0;

	flash_ticks = 0;
	flash_phase ^= 1;
	return flash_phase;
}

static void headlights_anim_tick(void)
{
	if (evflash_halves && ++evflash_ticks >= evflash_half) {
		evflash_ticks = 0;
		evflash_halves--;
	}

	switch (mode) {
	case MODE_NORMAL:
		if (ev == EV_TURN)
			flash_tick();
		else if (ev == EV_HIGHBEAM)
			seq_tick(HIGHBEAM_STEP_TICKS, sizeof(highbeam_frames));

		if (--ev_ticks == 0) {
			if (ev != EV_GAP)
				ev_start(EV_GAP);
			else
				ev_start(rand_below(HIGHBEAM_ONE_IN) == 0 ? EV_HIGHBEAM : EV_TURN);
		}
		break;

	case MODE_HAZARD:
		flash_tick();
		break;

	case MODE_HUE_FADE:
		if (++fade_ticks >= FADE_TICKS) {
			fade_first = 0;
			fade_start(fade_side ^ 1);
		}
		break;

	case MODE_POLICE:
		seq_tick(POLICE_STEP_TICKS, sizeof(police_frames));
		break;

	case MODE_RAINBOW:
		anim_hue = (uint16_t)(anim_hue + RAINBOW_HUE_STEP);
		break;
	}
}

static void low_beam(uint8_t px[3])
{
	set_px(px, (uint8_t)scale8(WARM_R, LOW_BEAM_LEVEL), (uint8_t)scale8(WARM_G, LOW_BEAM_LEVEL),
	       (uint8_t)scale8(WARM_B, LOW_BEAM_LEVEL));
}

static void headlights_render(uint8_t frame[2][3])
{
	for (int i = 0; i < 2; i++) {
		int is_left = (i == RGB_HEADLIGHT_L);
		uint8_t *px = frame[i];

		set_px(px, 0, 0, 0);

		if (evflash_halves && (evflash_mask & (1u << i))) {
			if (!(evflash_halves & 1))
				memcpy(px, evflash_rgb, 3);
			continue;
		}

		switch (mode) {
		case MODE_NORMAL:
			if (ev == EV_TURN && is_left == ev_left) {
				if (flash_phase)
					set_px(px, AMBER_R, AMBER_G, AMBER_B);
			}
			else if (ev == EV_HIGHBEAM && highbeam_frames[seq_idx]) {
				set_px(px, WARM_R, WARM_G, WARM_B);
			}
			else {
				low_beam(px);
			}
			break;

		case MODE_HAZARD:
			if (flash_phase)
				set_px(px, AMBER_R, AMBER_G, AMBER_B);
			break;

		case MODE_HUE_FADE: {
			// One side ramps up while the other ramps down; squared, so the
			// fade looks even rather than rushing at the dim end.
			uint8_t t = (uint8_t)(fade_ticks * 255u / FADE_TICKS);
			if (i != fade_side)
				t = fade_first ? 0 : (uint8_t)(255 - t);
			color_hsv(fade_hue[i], 255, (uint8_t)scale8(t, t), px);
			break;
		}

		case MODE_POLICE:
			if (police_frames[seq_idx] & (is_left ? 1u : 2u)) {
				if (is_left) px[0] = 255;
				else         px[2] = 255;
			}
			break;

		case MODE_RAINBOW:
			color_hsv((uint16_t)(anim_hue + (is_left ? 0 : 32768)), 255, 255, px);
			break;
		}
	}
}

static void evflash_start(uint8_t r, uint8_t g, uint8_t b, uint8_t count, uint8_t mask, uint8_t half)
{
	set_px(evflash_rgb, r, g, b);
	evflash_mask = mask;
	evflash_half = half;
	evflash_halves = (uint8_t)(count * 2);
	evflash_ticks = 0;
}

void rgb_flash(uint8_t r, uint8_t g, uint8_t b, uint8_t count)
{
	evflash_start(r, g, b, count, 3, EVFLASH_HALF_TICKS);
}

void rgb_signal(uint8_t signal)
{
	switch (signal) {
	case RGB_SIGNAL_LEFT:
		evflash_start(AMBER_R, AMBER_G, AMBER_B, 2, 1u << RGB_HEADLIGHT_L, FLASH_HALF_TICKS);
		break;
	case RGB_SIGNAL_RIGHT:
		evflash_start(AMBER_R, AMBER_G, AMBER_B, 2, 1u << RGB_HEADLIGHT_R, FLASH_HALF_TICKS);
		break;
	case RGB_SIGNAL_HAZARDS:
		evflash_start(AMBER_R, AMBER_G, AMBER_B, 2, 3, FLASH_HALF_TICKS);
		break;
	case RGB_SIGNAL_BRIGHTS:
		evflash_start(WARM_R, WARM_G, WARM_B, 2, 3, HIGHBEAM_STEP_TICKS);
		break;
	}
}

void rgb_mode_rainbow(void)
{
	mode_set(MODE_RAINBOW);
}

// ---------- scanner ----------
// A scan runs only while the finger stays on the pad: it pulses blue for
// 5-10 s, then flashes green a few times (verified). Lifting the finger
// early fails the scan with a few red flashes instead. Either way the
// scanner then drops back to idle.

// Phase steps are 65536 / ticks-per-cycle.
#define SCAN_IDLE_STEP      (65536 / MS_TICKS(4000))
#define SCAN_BUSY_STEP      (65536 / MS_TICKS(400))

// Verified: SCAN_OK_FLASHES green flashes, SCAN_OK_HALF_TICKS on and off, at
// SCAN_OK_LEVEL before the brightness cap (128 -> ~24/255 on the wire).
#define SCAN_OK_FLASHES    3
#define SCAN_OK_HALF_TICKS MS_TICKS(200)
#define SCAN_OK_LEVEL      128

// Failure: SCAN_FAIL_FLASHES red flashes, SCAN_FAIL_HALF_TICKS on and off.
#define SCAN_FAIL_FLASHES    3
#define SCAN_FAIL_HALF_TICKS MS_TICKS(150)

#define SCAN_BUSY_MIN_TICKS    MS_TICKS(5000)
#define SCAN_BUSY_SPREAD_TICKS MS_TICKS(5000)


static uint8_t  scan_state;
static uint8_t  scan_gold;    // idle breathes gold instead of blue
static uint16_t scan_phase;
static uint16_t scan_ticks;   // remaining in BUSY / VERIFIED / FAILED

void rgb_scan_start(void)
{
	scan_state = SCAN_BUSY;
	scan_phase = 0;
	scan_ticks = (uint16_t)(SCAN_BUSY_MIN_TICKS + rand_below(SCAN_BUSY_SPREAD_TICKS));
}

void rgb_scan_finger(uint8_t down)
{
	if (scan_state != SCAN_BUSY || down)
		return;

	scan_state = SCAN_FAILED;
	scan_ticks = 2 * SCAN_FAIL_FLASHES * SCAN_FAIL_HALF_TICKS;
}

uint8_t rgb_scan_state(void)
{
	return scan_state;
}

void rgb_scan_gold(uint8_t on)
{
	scan_gold = on;
}

// Returns 1 on the tick a scan completes.
static uint8_t scanner_tick(void)
{
	static const uint16_t steps[] = { SCAN_IDLE_STEP, SCAN_BUSY_STEP, 0, 0 };

	scan_phase = (uint16_t)(scan_phase + steps[scan_state]);

	if (scan_state == SCAN_IDLE || --scan_ticks)
		return 0;

	if (scan_state == SCAN_BUSY) {
		scan_state = SCAN_VERIFIED;
		scan_ticks = 2 * SCAN_OK_FLASHES * SCAN_OK_HALF_TICKS;
		scan_phase = 0;
		return 1;
	}

	scan_state = SCAN_IDLE;
	scan_phase = 0;
	return 0;
}

// FAILED/VERIFIED: scan_ticks counts down from an even number of half
// periods; each flash is lit for its first half.
static uint8_t scan_flash_lit(uint16_t half_ticks)
{
	return (scan_ticks - 1) / half_ticks % 2;
}

static void scanner_render(uint8_t px[3])
{
	uint8_t t = tri8(scan_phase);

	switch (scan_state) {
	case SCAN_IDLE: {
		uint8_t v = (uint8_t)(8 + scale8(t, 40));
		if (scan_gold)
			set_px(px, v, (uint8_t)scale8(v, 150), 0);
		else
			set_px(px, 0, v / 4, v);
		break;
	}

	case SCAN_BUSY: {
		uint8_t v = (uint8_t)(16 + scale8(t, 239));
		set_px(px, 0, 0, v);
		break;
	}

	case SCAN_VERIFIED:
		// Pure green: any blue reads as teal.
		set_px(px, 0, scan_flash_lit(SCAN_OK_HALF_TICKS) ? SCAN_OK_LEVEL : 0, 0);
		break;

	case SCAN_FAILED:
		set_px(px, scan_flash_lit(SCAN_FAIL_HALF_TICKS) ? 255 : 0, 0, 0);
		break;
	}
}

// ---------- self-test ----------

#define SELFTEST_WHITE 64   // before brightness scaling

static uint8_t selftest_active;
static uint8_t selftest_scanner_red;
static uint8_t selftest_headlights_green;

void rgb_selftest(uint8_t active, uint8_t scanner_red, uint8_t headlights_green)
{
	selftest_active = active;
	selftest_scanner_red = scanner_red;
	selftest_headlights_green = headlights_green;
}

static void selftest_render(uint8_t frame[RGB_COUNT][3])
{
	for (int i = 0; i < RGB_COUNT; i++)
		set_px(frame[i], SELFTEST_WHITE, SELFTEST_WHITE, SELFTEST_WHITE);

	if (selftest_scanner_red)
		set_px(frame[RGB_SCANNER], 255, 0, 0);

	if (selftest_headlights_green) {
		set_px(frame[RGB_HEADLIGHT_L], 0, 255, 0);
		set_px(frame[RGB_HEADLIGHT_R], 0, 255, 0);
	}
}

// ---------- frame ----------

void rgb_init(void)
{
	mode_set(MODE_NORMAL);
	WS2812BDMAInit();
}

uint8_t rgb_tick(void)
{
	uint8_t frame[RGB_COUNT][3];
	uint8_t verified = 0;

	if (selftest_active) {
		selftest_render(frame);
	}
	else {
		headlights_anim_tick();
		verified = scanner_tick();

		headlights_render(frame);
		scanner_render(frame[RGB_SCANNER]);
	}

	for (int i = 0; i < RGB_COUNT; i++) {
		uint32_t level = (i == RGB_SCANNER) ? SCANNER_BRIGHTNESS : HEADLIGHT_BRIGHTNESS;
		for (int c = 0; c < 3; c++)
			frame[i][c] = (uint8_t)scale8(frame[i][c], level);
	}

	// A frame still in flight just means this one is skipped; the animation
	// state has already moved on either way.
	if (!WS2812BLEDInUse) {
		memcpy((void *)led_frame, frame, sizeof(frame));
		WS2812BDMAStart(RGB_COUNT);
	}

	sao_host_set_headlights((const uint8_t (*)[3])frame);

	return verified;
}
