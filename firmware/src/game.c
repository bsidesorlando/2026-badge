// Lil Chompy and the World's Fair: a tiny retro-futurist text adventure on
// the SAO console, and the badge's CTF challenge (CTF_ADVENTURE).
//
// The walkthrough, for the people who write the game rather than play it:
// from visitor parking go north to the fairgrounds gate, hold the badge's
// fingerprint pad (the kiosk only hints at it), go north. Decode the station
// plaque (Caesar shift 7, from the stuck LINE 7 board). Tap Lil Chompy's
// head on the badge at the aerocar lot; he has lost his flight passkey, so
// walk back out to visitor parking and LOOK -- only an explicit LOOK after
// he's asked turns it up. Scan in again at the gate (it re-locks behind
// anyone arriving from parking), return to the lot, FLY to the Sky Spike,
// SAY SEE YOU LATER ALLIGATOR to the robot butler, go UP.
//
// It is deliberately small and hand-coded -- room logic lives in the verb
// handlers rather than a data-driven engine, which is the cheaper trade on
// a 16K part. All prose lives in game_text.txt, compressed at build time
// (scripts/gen_text.py); say(T_...) prints a message. To grow the game: add
// a room (enum, @ROOM_ message in the same position, exits row), then
// special-case whatever it needs in the handlers.

#include "ch32fun.h"

#include "console.h"
#include "ctf.h"
#include "game.h"
#include "rgb.h"
#include "settings.h"

#define GAME_TEXT_DATA
#include "game_text.h"

#include <string.h>

// ---------- text ----------

static void text_emit(uint8_t c)
{
	if (c < TEXT_FIRST_PAIR) {
		con_putc((char)c);
		return;
	}

	const uint8_t *pair = text_pairs[c - TEXT_FIRST_PAIR];
	text_emit(pair[0]);
	text_emit(pair[1]);
}

static void say(uint8_t id)
{
	const uint8_t *p = text_blob;

	while (id--) {
		while (*p++) { }
	}
	while (*p)
		text_emit(*p++);
}

// ---------- world ----------

enum room {
	R_PARKING,   // start
	R_GATE,
	R_AVENUE,
	R_STATION,
	R_LOT,
	R_SKYPORT,
	R_DECK,
	ROOM_COUNT,
};

_Static_assert(T_ROOM_DECK - T_ROOM_PARKING == R_DECK,
               "@ROOM_ messages in game_text.txt must match enum room");

enum dir { D_N, D_E, D_S, D_W, D_U, D_D, DIR_COUNT };

#define NO_EXIT 0xFF

static const uint8_t exits[ROOM_COUNT][DIR_COUNT] = {
	//               N          E          S          W          U          D
	[R_PARKING] = { R_GATE,    NO_EXIT,   NO_EXIT,   NO_EXIT,   NO_EXIT,   NO_EXIT },
	[R_GATE]    = { R_AVENUE,  NO_EXIT,   R_PARKING, NO_EXIT,   NO_EXIT,   NO_EXIT },
	[R_AVENUE]  = { NO_EXIT,   R_STATION, R_GATE,    R_LOT,     NO_EXIT,   NO_EXIT },
	[R_STATION] = { NO_EXIT,   NO_EXIT,   NO_EXIT,   R_AVENUE,  NO_EXIT,   NO_EXIT },
	[R_LOT]     = { NO_EXIT,   R_AVENUE,  NO_EXIT,   NO_EXIT,   NO_EXIT,   NO_EXIT },
	[R_SKYPORT] = { NO_EXIT,   NO_EXIT,   NO_EXIT,   NO_EXIT,   R_DECK,    NO_EXIT },
	[R_DECK]    = { NO_EXIT,   NO_EXIT,   NO_EXIT,   NO_EXIT,   NO_EXIT,   R_SKYPORT },
};

// ---------- state ----------

#define G_VERIFIED 0x01   // fingerprint scanned at the gate since arriving from parking
#define G_AWAKE    0x02   // Lil Chompy is awake (and has lost his passkey)
#define G_DOOR     0x04   // Sky Spike door open
#define G_PASSKEY  0x08   // carrying Chompy's flight passkey

static uint8_t room = R_PARKING;
static uint8_t gflags;
static uint8_t facing = D_N;   // the way the player last moved (enum dir, N/E/S/W)

// Room description plus whatever is going on there. `searching` is an
// explicit LOOK rather than walking in: only that turns up the passkey,
// once Chompy has asked the player to look around for it.
static void look(uint8_t searching)
{
	say((uint8_t)(T_ROOM_PARKING + room));

	switch (room) {
	case R_PARKING:
		if (searching && (gflags & (G_AWAKE | G_PASSKEY)) == G_AWAKE) {
			gflags |= G_PASSKEY;
			say(T_FOUND_PASSKEY);
		}
		break;

	case R_GATE:
		if (gflags & G_VERIFIED)
			say(T_TURNSTILES_OPEN);
		break;

	case R_LOT:
		say(!(gflags & G_AWAKE)    ? T_CHOMPY_ASLEEP
		    : (gflags & G_PASSKEY) ? T_CHOMPY_READY
		                           : T_CHOMPY_WAITING);
		break;

	case R_DECK:
		con_puts("  ");
		ctf_print_flag(CTF_ADVENTURE);
		con_puts("\n");
		if (!ctf_is_solved(CTF_ADVENTURE)) {
			ctf_mark_solved(CTF_ADVENTURE);
			say(T_SOLVED);
		}
		break;
	}
}

// ---------- parsing ----------

static char *skip_spaces(char *s)
{
	while (*s == ' ')
		s++;
	return s;
}

// Keeps only letters, lowercased: "See you later, Alligator!" ->
// "seeyoulateralligator".
static void letters_only(char *s)
{
	char *d = s;

	for (; *s; s++) {
		char c = (char)(*s | 0x20);   // ASCII letters: set bit 5 to lowercase
		if (c >= 'a' && c <= 'z')
			*d++ = c;
	}
	*d = 0;
}

// Vocabulary. A flat table of fixed-width words is much smaller than a
// strcmp() per verb in the dispatcher, and needs no pointer per entry.
enum word {
	W_UNKNOWN,
	W_N, W_E, W_S, W_W, W_U, W_D,   // same order as enum dir
	W_GO, W_HELP, W_LOOK, W_TAP, W_FLY, W_SAY,
	W_STATUS, W_RESTART, W_RESET, W_CHOMP, W_XYZZY, W_PEEK,
	// nouns
	W_PLAQUE, W_BOARD, W_ROBOT, W_KIOSK, W_CHOMPY,
};

#define WORD_MAX 7

static const struct {
	char text[WORD_MAX] __attribute__((nonstring));   // no NUL at full width
	uint8_t id;
} vocab[] = {
	{ "n", W_N }, { "north", W_N }, { "e", W_E }, { "east", W_E },
	{ "s", W_S }, { "south", W_S }, { "w", W_W }, { "west", W_W },
	{ "u", W_U }, { "up", W_U }, { "d", W_D }, { "down", W_D },
	{ "go", W_GO }, { "walk", W_GO },
	{ "help", W_HELP }, { "?", W_HELP },
	{ "look", W_LOOK }, { "l", W_LOOK },
	{ "tap", W_TAP }, { "wake", W_TAP }, { "pet", W_TAP },
	{ "fly", W_FLY }, { "drive", W_FLY }, { "ride", W_FLY },
	{ "say", W_SAY }, { "speak", W_SAY },
	{ "status", W_STATUS }, { "restart", W_RESTART }, { "reset", W_RESET },
	{ "chomp", W_CHOMP }, { "xyzzy", W_XYZZY }, { "peek", W_PEEK },
	{ "plaque", W_PLAQUE }, { "board", W_BOARD },
	{ "robot", W_ROBOT }, { "butler", W_ROBOT }, { "rb-7", W_ROBOT }, { "rb7", W_ROBOT },
	{ "kiosk", W_KIOSK }, { "scanner", W_KIOSK }, { "chompy", W_CHOMPY },
};

// Case-insensitive, leaving the word itself untouched. Only the first
// WORD_MAX letters count, Infocom-style.
static uint8_t lookup(const char *w)
{
	char buf[WORD_MAX];
	uint8_t n;

	for (n = 0; w[n] && n < WORD_MAX; n++) {
		char c = w[n];
		buf[n] = (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
	}
	if (n < WORD_MAX)
		buf[n] = 0;

	for (uint8_t i = 0; i < sizeof(vocab) / sizeof(vocab[0]); i++) {
		if (!strncmp(vocab[i].text, buf, WORD_MAX))
			return vocab[i].id;
	}
	return W_UNKNOWN;
}

// ---------- verbs ----------

static void go(uint8_t d)
{
	uint8_t to = exits[room][d];

	if (room == R_GATE && d == D_N && !(gflags & G_VERIFIED)) {
		say(T_NO_CLEARANCE);
		return;
	}
	if (room == R_SKYPORT && d == D_U && !(gflags & G_DOOR)) {
		say(T_DOOR_SEALED);
		return;
	}
	if (to == NO_EXIT) {
		say(T_NO_EXIT);
		return;
	}

	// The flying car signals each move relative to the way the player was
	// facing: turn signal for a turn, brights straight ahead, hazards for
	// doubling back. Up and down don't change the facing.
	if (d <= D_W) {
		static const uint8_t signals[4] = {
			RGB_SIGNAL_BRIGHTS, RGB_SIGNAL_RIGHT, RGB_SIGNAL_HAZARDS, RGB_SIGNAL_LEFT,
		};
		rgb_signal(signals[(d - facing) & 3]);   // enum dir runs clockwise
		facing = d;
	}

	// The turnstiles re-lock behind anyone coming in from the parking lot.
	if (to == R_GATE && room == R_PARKING)
		gflags &= (uint8_t)~G_VERIFIED;

	room = to;
	look(0);
}

static void examine(uint8_t what)
{
	uint8_t msg = T_NOTHING_SPECIAL;

	switch (room) {
	case R_STATION:
		if (what == W_PLAQUE)
			msg = T_PLAQUE;
		else if (what == W_BOARD)
			msg = T_BOARD;
		break;

	case R_SKYPORT:
		if (what == W_ROBOT)
			msg = T_ROBOT;
		break;

	case R_GATE:
		if (what == W_KIOSK)
			msg = T_KIOSK;
		break;

	case R_LOT:
		if (what == W_CHOMPY)
			msg = (gflags & G_AWAKE) ? T_CHOMPY_GRIN : T_CHOMPY_TAPPABLE;
		break;

	case R_DECK:
		if (what == W_PLAQUE) {
			look(0);   // the time capsule plaque is the flag
			return;
		}
		break;
	}

	say(msg);
}

static void fly(void)
{
	if (room == R_LOT && (gflags & G_PASSKEY)) {
		say(T_FLY_UP);
		room = R_SKYPORT;
	}
	else if (room == R_SKYPORT) {
		say(T_FLY_DOWN);
		room = R_LOT;
	}
	else {
		say(room != R_LOT           ? T_FLAP
		    : (gflags & G_AWAKE)    ? T_NO_PASSKEY
		                            : T_NEED_DRIVER);
		return;
	}

	con_puts("\n");
	look(0);
}

static void speak(char *words)
{
	if (room != R_SKYPORT) {
		say(T_NOBODY);
		return;
	}

	letters_only(words);
	if (!strcmp(words, "seeyoulateralligator")) {
		say(T_PASSPHRASE_OK);
		gflags |= G_DOOR;
	}
	else {
		say(T_ACCESS_DENIED);
	}
}

static void status(void)
{
	for (uint8_t id = 0; id < CTF_COUNT; id++) {
		con_puts(ctf_is_solved(id) ? "  [x] " : "  [ ] ");
		con_puts(ctf_name(id));
		con_puts("\n");
	}

	say(T_STATUS_MODES);
	say(rgb_rainbow_unlocked() ? T_STATUS_RAINBOW_ON : T_STATUS_RAINBOW_OFF);
	if (settings.unlocks & UNLOCK_HACKUCF_FADE)
		say(T_STATUS_HACKUCF);
}

// ---------- PEEK ----------
// Unlocked by beating the adventure: read one 32-bit word of memory per call,
// so a player who finds the flash's address in the CH32V003 datasheet can
// dump the firmware. No guard rails: a read from an unimplemented address
// faults, and the fault handler (console.c) prints a crash report and
// reboots the badge -- players learn the hard way. The one exception is the page holding ctf_mirror_flag, and
// only at its low boot-mirror address -- the real flash address at
// 0x08000000 + offset reads fine, which is the challenge.

#ifdef GAME_HOST_BUILD
uint32_t game_host_peek32(uint32_t addr);   // test/host/stubs.c
#define PEEK32(addr) game_host_peek32(addr)
#else
#define PEEK32(addr) (*(const volatile uint32_t *)(uintptr_t)(addr))
#endif

// Hex address, "0x" optional. Returns 0 on anything that isn't one.
static uint8_t parse_hex(const char *s, uint32_t *out)
{
	uint32_t v = 0;
	uint8_t digits = 0;

	if (s[0] == '0' && (s[1] | 0x20) == 'x')
		s += 2;

	for (; *s; s++, digits++) {
		char c = (char)(*s | 0x20);   // lowercase letters; digits unaffected
		uint8_t d;
		if (*s >= '0' && *s <= '9')
			d = (uint8_t)(*s - '0');
		else if (c >= 'a' && c <= 'f')
			d = (uint8_t)(c - 'a' + 10);
		else
			return 0;
		if (digits == 8)
			return 0;
		v = (v << 4) | d;
	}

	*out = v;
	return digits != 0;
}

static void peek(const char *arg)
{
	uint32_t addr;
	uint32_t mirror = (uint32_t)(uintptr_t)ctf_mirror_flag;

	if (!parse_hex(arg, &addr)) {
		say(T_PEEK_USAGE);
		return;
	}

	con_puts("0x");
	con_puthex(addr, 8);
	con_puts(": ");
	// Refuse any 4-byte read that overlaps the page, not just ones starting
	// in it: addresses aren't forced to alignment, so a word starting up to
	// 3 bytes before the page would otherwise carry flag bytes.
	if (addr + 3 - mirror < sizeof(ctf_mirror_flag) + 3) {
		say(T_PEEK_DENIED);
		return;
	}
	con_puts("0x");
	con_puthex(PEEK32(addr), 8);
	con_putc('\n');
}

// ---------- entry points ----------

void game_banner(void)
{
	say(T_BANNER);
	con_puts("\n");
	look(0);
}

void game_command(char *line)
{
	char *verb = skip_spaces(line);
	char *arg = verb;

	while (*arg && *arg != ' ')
		arg++;
	if (*arg)
		*arg++ = 0;
	arg = skip_spaces(arg);

	// Trim trailing spaces.
	for (char *e = arg + strlen(arg); e > arg && e[-1] == ' '; )
		*--e = 0;

	// "LOOK AT THING" reads as "LOOK THING".
	if ((arg[0] | 0x20) == 'a' && (arg[1] | 0x20) == 't' && arg[2] == ' ')
		arg = skip_spaces(arg + 3);

	uint8_t v = lookup(verb);
	uint8_t noun = lookup(arg);

	if (v == W_GO)
		v = noun;

	switch (v) {
	case W_N: case W_E: case W_S: case W_W: case W_U: case W_D:
		go((uint8_t)(v - W_N));
		break;

	case W_HELP:
		say(T_HELP);
		if (ctf_is_solved(CTF_ADVENTURE))
			say(T_HELP_PEEK);
		break;

	case W_LOOK:
		if (*arg)
			examine(noun);
		else
			look(1);
		break;

	case W_TAP:
		say(room == R_LOT && !(gflags & G_AWAKE) ? T_TAP_CHOMPY : T_NOTHING_HAPPENS);
		break;

	case W_FLY:
		fly();
		break;

	case W_SAY:
		speak(arg);
		break;

	case W_STATUS:
		status();
		break;

	case W_PEEK:
		if (ctf_is_solved(CTF_ADVENTURE))
			peek(arg);
		else
			say(T_HUH);   // a secret until earned
		break;

	case W_RESTART:
		room = R_PARKING;
		gflags = 0;
		facing = D_N;
		look(0);
		break;

	case W_RESET:
		// Wipe every solve and unlock, then reboot so the lights, headlight
		// modes and PEEK all come back up from the clean slate.
		settings.unlocks = 0;
		settings_save();
		say(T_RESET_DONE);
		con_reboot();

	case W_CHOMP:
		say(T_CHOMP);
		rgb_flash(32, 200, 16, 2);
		break;

	case W_XYZZY:
		say(T_XYZZY);
		break;

	default:
		if (*verb)
			say(T_HUH);
		break;
	}
}

// Out-of-turn narration: break away from the prompt, then put it back.
static void interject(uint8_t id)
{
	con_puts("\n");
	say(id);
	con_puts("> ");
}

// Only the gate kiosk reads fingerprints.
void game_on_scan_verified(void)
{
	if (room != R_GATE || (gflags & G_VERIFIED))
		return;

	gflags |= G_VERIFIED;
	interject(T_KIOSK_CHIMES);
}

void game_on_chompy_tap(void)
{
	if (room != R_LOT || (gflags & G_AWAKE))
		return;

	gflags |= G_AWAKE;
	interject(T_CHOMPY_WAKES);
}
