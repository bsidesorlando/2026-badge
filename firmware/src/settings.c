// Settings in flash: one magic-checked 64-byte page, written via the fast
// page-program path.

#include "ch32fun.h"

#include "settings.h"
#include "util.h"

#include <string.h>

#define SETTINGS_PAGE_ADDR 0x08003FC0UL

// Marks a provisioned page; anything else (blank flash, or a page from a
// test build) loads as defaults.
static const char settings_magic[8] = { 'B', 'S', 'O', '_', '2', '0', '2', '6' };

#define SETTINGS_SAVE_DELAY_TICKS MS_TICKS(3000)

struct __attribute__((packed)) settings_page {
	char magic[8];
	struct settings s;
};

struct settings settings;

static uint16_t save_countdown;

static const struct settings_page *flash_page(void)
{
	return (const struct settings_page *)SETTINGS_PAGE_ADDR;
}

static uint8_t provisioned(void)
{
	return !memcmp(flash_page()->magic, settings_magic, sizeof(settings_magic));
}

void settings_load(void)
{
	if (provisioned())
		settings = flash_page()->s;
}

void settings_save(void)
{
	save_countdown = 0;

	if (provisioned() && !memcmp(&flash_page()->s, &settings, sizeof(settings)))
		return;   // nothing changed; spare the erase cycle

	// A union, not a cast: byte stores through a struct pointer aimed at a
	// word array is an aliasing violation LTO has been seen acting on.
	union {
		struct settings_page p;
		uint32_t words[16];
	} img;

	memset(&img, 0xFF, sizeof(img));
	memcpy(img.p.magic, settings_magic, sizeof(settings_magic));
	img.p.s = settings;

	volatile uint32_t *page = (volatile uint32_t *)SETTINGS_PAGE_ADDR;

	// Both locks: KEYR for flash generally, MODEKEYR for fast page mode.
	FLASH->KEYR = FLASH_KEY1;
	FLASH->KEYR = FLASH_KEY2;
	FLASH->MODEKEYR = FLASH_KEY1;
	FLASH->MODEKEYR = FLASH_KEY2;

	FLASH->CTLR = CR_PAGE_ER;
	FLASH->ADDR = (uint32_t)SETTINGS_PAGE_ADDR;
	FLASH->CTLR = CR_STRT_Set | CR_PAGE_ER;
	while (FLASH->STATR & FLASH_STATR_BSY) { }

	FLASH->CTLR = CR_PAGE_PG;
	FLASH->CTLR = CR_BUF_RST | CR_PAGE_PG;
	FLASH->ADDR = (uint32_t)SETTINGS_PAGE_ADDR;
	while (FLASH->STATR & FLASH_STATR_BSY) { }

	for (int i = 0; i < 16; i++) {
		page[i] = img.words[i];
		FLASH->CTLR = CR_PAGE_PG | FLASH_CTLR_BUF_LOAD;
		while (FLASH->STATR & FLASH_STATR_BSY) { }
	}

	FLASH->CTLR = CR_PAGE_PG | CR_STRT_Set;
	while (FLASH->STATR & FLASH_STATR_BSY) { }

	FLASH->CTLR = FLASH_CTLR_LOCK;
}

void settings_save_later(void)
{
	save_countdown = SETTINGS_SAVE_DELAY_TICKS;
}

void settings_tick(void)
{
	if (save_countdown && --save_countdown == 0)
		settings_save();
}
