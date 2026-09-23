// Host build of the text adventure: the real game.c and ctf.c on a desktop
// terminal, with the badge hardware stubbed out. See run.sh.
//
// Lines starting with '!' simulate badge events instead of being typed:
//   !scan    the fingerprint scan completes (turns green)
//   !chompy  Lil Chompy's head is tapped

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "beacons.h"
#include "console.h"
#include "game.h"
#include "rgb.h"
#include "settings.h"

struct settings settings;

void con_putc(char c) { putchar(c); }
void con_puts(const char *s) { fputs(s, stdout); }
void con_putu(uint32_t v) { printf("%u", (unsigned)v); }
uint8_t con_active(void) { return 1; }
void con_reboot(void) { printf("[reboot]\n"); exit(0); }
void con_puthex(uint32_t v, uint8_t digits)
{
	while (digits--)
		putchar("0123456789abcdef"[(v >> (4 * digits)) & 0xF]);
}

// Fake memory for PEEK: every word reads as its own address, so output is
// easy to check by eye.
uint32_t game_host_peek32(uint32_t addr) { return addr; }

void settings_save(void) { printf("[settings saved: unlocks=0x%02x]\n", settings.unlocks); }
void beacons_set_lit(uint8_t lit) { printf("[sphere: %u lit]\n", lit); }

void rgb_flash(uint8_t r, uint8_t g, uint8_t b, uint8_t count)
{
	printf("[headlights flash #%02x%02x%02x x%u]\n", r, g, b, count);
}

uint8_t rgb_rainbow_unlocked(void) { return settings.unlocks & 1; }
void rgb_mode_rainbow(void) { printf("[headlights: rainbow mode]\n"); }

void rgb_signal(uint8_t signal)
{
	static const char *const names[] = { "LEFT turn signal", "RIGHT turn signal", "brights", "hazards" };
	printf("[headlights: %s]\n", names[signal]);
}

int main(void)
{
	char line[128];

	game_banner();
	fputs("> ", stdout);
	while (fgets(line, sizeof(line), stdin)) {
		line[strcspn(line, "\r\n")] = 0;
		printf("%s\n", line);   // echo, so piped transcripts read naturally

		if (!strcmp(line, "!scan"))
			game_on_scan_verified();
		else if (!strcmp(line, "!chompy"))
			game_on_chompy_tap();
		else {
			line[48] = 0;   // the badge's LINE_MAX
			game_command(line);
			fputs("> ", stdout);
		}
	}
	putchar('\n');
	return 0;
}
