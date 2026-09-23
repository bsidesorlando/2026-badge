// CTF challenge table, flag printing, and solve bookkeeping.

#include "ch32fun.h"

#include "beacons.h"
#include "console.h"
#include "ctf.h"
#include "rgb.h"
#include "settings.h"

// The adventure's flag, XOR-obfuscated. Regenerate with
// scripts/ctf_flag.py <id> '<flag>'.
static const uint8_t flag0[39] = { 0x07, 0x0A, 0xD8, 0xB2, 0x5F, 0x32, 0x26, 0xC1, 0xC3, 0x30, 0xAD, 0x0E, 0xB7, 0x78, 0xC6, 0xEA, 0xA2, 0x63, 0xFF, 0xE8, 0x16, 0xF4, 0x45, 0xDE, 0xD8, 0xA5, 0x5B, 0x3E, 0x62, 0xEF, 0xDD, 0xB6, 0x7F, 0xE6, 0x38, 0xDA, 0xBC, 0x1C, 0x97 };

// A second flag, for players who dump the firmware with PEEK: it sits alone
// in its own flash page, and PEEK refuses that page at its low boot-mirror
// address (0x0000xxxx) -- but not at the real flash address (0x0800xxxx).
// Stored in the clear on purpose: reading raw memory is the challenge.
const char ctf_mirror_flag[64] __attribute__((aligned(64))) = "sun{l00k_1n_th3_m1rr0r}";

struct challenge {
	const char *name;
	const uint8_t *flag;
	uint8_t flag_len;
};

static const struct challenge challenges[CTF_COUNT] = {
	[CTF_ADVENTURE] = { "Clearance", flag0, sizeof(flag0) },
};

// Must match scripts/ctf_flag.py.
static uint8_t ctf_keystream(uint8_t *x)
{
	*x = (uint8_t)(*x * 5u + 0x3Bu);
	return *x;
}

static uint8_t ctf_key_seed(uint8_t id)
{
	return (uint8_t)(0xA5u ^ (id * 0x3Bu));
}

const char *ctf_name(uint8_t id)
{
	return challenges[id].name;
}

uint8_t ctf_is_solved(uint8_t id)
{
	return (settings.unlocks & CTF_BIT(id)) != 0;
}

// The sphere keeps one LED lit, plus one per solved challenge. With two of
// three lit the handoff keeps rotating which one is dark, which reads more
// fluidly than all three sitting on.
static void ctf_apply(void)
{
	uint8_t n = 0;

	for (uint8_t id = 0; id < CTF_COUNT; id++)
		n += ctf_is_solved(id);

	beacons_set_lit((uint8_t)(1 + n));
}

void ctf_init(void)
{
	ctf_apply();
}

void ctf_mark_solved(uint8_t id)
{
	if (ctf_is_solved(id))
		return;

	settings.unlocks |= (uint8_t)CTF_BIT(id);
	settings_save();
	ctf_apply();
	rgb_flash(0, 255, 64, 4);
	if (rgb_rainbow_unlocked())
		rgb_mode_rainbow();
}

void ctf_print_flag(uint8_t id)
{
	const struct challenge *c = &challenges[id];
	uint8_t x = ctf_key_seed(id);

	for (uint8_t i = 0; i < c->flag_len; i++)
		con_putc((char)(c->flag[i] ^ ctf_keystream(&x)));
}
