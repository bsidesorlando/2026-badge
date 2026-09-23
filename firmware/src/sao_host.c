// SAOv3 host side: bus discovery, enumeration, and LED mirroring.
//
// The badge is the bus master on the SAO connector (SDA PC1 / SCL PC2, with
// on-board pull-ups). SAO GPIO1 (PD0) and GPIO2 (PD1) stay tristated, as the
// spec requires of a badge that has not negotiated an output mode -- PD1 is
// also SWIO, so leaving it alone is what keeps the debug link alive too.
//
// Discovery follows the saoh library's flow: a full reset scan shortly after
// boot (static SAO detection magic + ARP enumeration), a cheap scan_new
// every second to catch hot-plugged ARP devices, and a full ARP rescan every
// 15 s to notice removals. Static (non-ARP) SAOs have no removal event, so
// every enumerated SAO gets a periodic liveness check, and a slot whose
// transfers keep failing is dropped and a fresh full scan is scheduled.
//
// Enumeration records each SAO's VID/PID (so the app can spot particular
// SAOs, like HackUCF's) and whether it exposes the LED class interface. The
// badge does NOT take over SAO LEDs on its own: sao_host_set_led_takeover()
// turns that on, after which the task streams the current headlight colors
// across every LED-capable SAO's string.

#include "ch32fun.h"

#include "saoh_config.h"
#include "saoh/consts/err.h"
#include "saoh/consts/sao.h"
#include "saoh/discovery.h"
#include "saoh/itf.h"
#include "saoh/smbus.h"

#include "i2c_host.h"
#include "sao_host.h"

#include <string.h>

#define MAX_SAOS 4

// Consecutive failed transfers before a slot is declared unplugged.
#define SAO_MAX_FAILS 5

// Mirror at most this many LEDs per SAO: bounds both RAM and the bus time
// spent per update (16 * 3 + overhead ~ 55 bytes ~ 6 ms at 100 kHz).
#define MIRROR_MAX_LEDS 16

// Cadences, in ms.
#define BOOT_SCAN_DELAY_MS   500     // let SAO micros boot before the first scan
#define SCAN_NEW_PERIOD_MS   1000
#define RESCAN_PERIOD_MS     15000
#define MIRROR_PERIOD_MS     100
#define HEARTBEAT_PERIOD_MS  500     // one slot per beat, round robin
#define EMPTY_RESCAN_MS      2000    // full reset scan cadence while nothing is attached

enum slot_state {
	SLOT_EMPTY = 0,
	SLOT_PENDING,   // discovered, awaiting enumeration
	SLOT_READY,     // enumerated (led_base != 0 if it has the LED interface)
};

struct sao_slot {
	uint8_t state;
	uint8_t pec_addr;    // address with the PEC-capable flag in bit 7
	uint8_t led_base;    // LED class base command, 0 = no LED interface
	uint8_t led_mode;    // SAO_LEDITF_MODE_*
	uint16_t led_count;
	uint16_t vid;
	uint16_t pid;
	uint8_t fails;
};

static saoh_bus_t sao_bus;
static saoh_discovery_state_t sao_disc;
static struct sao_slot slots[MAX_SAOS];

static uint8_t headlights[2][3];

static uint32_t ms_now;
static uint32_t next_scan_ms;
static uint32_t next_rescan_ms;
static uint32_t next_mirror_ms;
static uint32_t next_heartbeat_ms;
static uint32_t next_empty_rescan_ms;
static uint8_t heartbeat_idx;
static uint8_t full_scan_needed = 1;   // boot starts with a full reset scan
static uint8_t led_takeover;

// ms deadline comparison that survives wrap (49 days -- but still).
static int time_after(uint32_t now, uint32_t deadline)
{
	return (int32_t)(now - deadline) >= 0;
}

// ---------- discovery callbacks ----------

static struct sao_slot *slot_by_addr(uint8_t addr)
{
	for (int i = 0; i < MAX_SAOS; i++) {
		if (slots[i].state != SLOT_EMPTY && (slots[i].pec_addr & SMBUS_ADDR_MASK) == addr)
			return &slots[i];
	}
	return 0;
}

static void sao_discovered_cb(void *arg, uint8_t pec_addr, const smbus_arp_udid_t *udid)
{
	(void)arg;
	(void)udid;   // VID/PID come from the common interface during enumeration

	// A full reset scan re-reports static SAOs we already track.
	struct sao_slot *s = slot_by_addr(pec_addr & SMBUS_ADDR_MASK);
	if (s) {
		s->pec_addr = pec_addr;
		s->fails = 0;
		return;
	}

	for (int i = 0; i < MAX_SAOS; i++) {
		if (slots[i].state == SLOT_EMPTY) {
			slots[i].state = SLOT_PENDING;
			slots[i].pec_addr = pec_addr;
			slots[i].led_base = 0;
			slots[i].fails = 0;
			return;
		}
	}
	// More SAOs than slots: ignored until something frees up.
}

static void sao_removed_cb(void *arg, uint8_t addr)
{
	(void)arg;

	struct sao_slot *s = slot_by_addr(addr);
	if (s)
		memset(s, 0, sizeof(*s));
}

// ---------- enumeration ----------

// Takes an SAO's LEDs over (or hands them back). The SAO stages commands
// while disabled, so order does not matter much, but enable-then-stream is
// cleanest.
static void sao_led_control(struct sao_slot *s, uint8_t enable)
{
	saoh_smbus_write_byte(&sao_bus, s->pec_addr, (uint8_t)(s->led_base + SAO_LEDITF_CMD_CONTROL_ENABLE), enable);
}

static void sao_enumerate(struct sao_slot *s)
{
	uint8_t buf[2 * 8];   // GPIO caps array / class interface pairs
	saoh_err_t ret;

	if (saoh_itf_check_valid(&sao_bus, s->pec_addr) != SAOH_ERR_OK) {
		// Not answering as an SAOv3 device (or gone already). Drop it; a
		// later scan will re-report it if it comes back.
		memset(s, 0, sizeof(*s));
		return;
	}

	// Identity. A failure leaves 0:0, which matches nothing we look for.
	s->vid = 0;
	s->pid = 0;
	saoh_itf_query_vidpid(&sao_bus, s->pec_addr, &s->vid, &s->pid, 0);

	// Per spec, write the default GPIO mode (first capability entry) during
	// enumeration to clear any state a previous host left behind. An empty
	// array means "mode 0 only"; a failure here is not fatal.
	ret = saoh_smbus_block_read_maxlen(&sao_bus, s->pec_addr, SAO_CMNITF_CMD_GET_GPIO_CAPABILITIES,
	                                   buf, sizeof(buf));
	if (ret >= 0)
		saoh_smbus_write_byte(&sao_bus, s->pec_addr, SAO_CMNITF_CMD_SET_GPIO_MODE,
		                      (ret > 0) ? buf[0] : SAO_CMNITF_IOMODE_DISABLED);

	// Class interfaces: (id, base command) pairs. We only consume LED.
	s->led_base = 0;
	ret = saoh_smbus_block_read_maxlen(&sao_bus, s->pec_addr, SAO_CMNITF_CMD_GET_CLASS_INTERFACES,
	                                   buf, sizeof(buf));
	if (ret > 0) {
		for (int i = 0; i + 1 < ret; i += 2) {
			if (buf[i] == SAO_CLASS_LED)
				s->led_base = buf[i + 1];
		}
	}

	if (s->led_base) {
		ret = saoh_smbus_block_read_maxlen(&sao_bus, s->pec_addr,
		                                   (uint8_t)(s->led_base + SAO_LEDITF_CMD_QUERY_CONFIG),
		                                   buf, sizeof(buf));
		if (ret >= 3) {
			s->led_mode = buf[0];
			s->led_count = (uint16_t)(buf[1] | ((uint16_t)buf[2] << 8));
			if (s->led_count > MIRROR_MAX_LEDS)
				s->led_count = MIRROR_MAX_LEDS;
		}
		else {
			s->led_base = 0;
		}
	}

	s->state = SLOT_READY;
	s->fails = 0;

	// Hot-plugged while a takeover is active: claim this one too.
	if (s->led_base && led_takeover)
		sao_led_control(s, 1);
}

// ---------- liveness / LED mirroring ----------

static void sao_slot_failed(struct sao_slot *s)
{
	if (++s->fails < SAO_MAX_FAILS)
		return;

	// Unplugged mid-conversation. ARP devices would eventually be reaped by
	// the periodic rescan, but static SAOs have no removal event -- schedule
	// a full reset scan so the maps match reality again.
	memset(s, 0, sizeof(*s));
	full_scan_needed = 1;
}

// Checks the next enumerated slot that mirroring is not already exercising.
static void sao_heartbeat(void)
{
	for (int n = 0; n < MAX_SAOS; n++) {
		struct sao_slot *s = &slots[heartbeat_idx];
		heartbeat_idx = (uint8_t)((heartbeat_idx + 1u) % MAX_SAOS);

		if (s->state != SLOT_READY || (led_takeover && s->led_base))
			continue;

		if (saoh_itf_check_valid(&sao_bus, s->pec_addr) != SAOH_ERR_OK)
			sao_slot_failed(s);
		else
			s->fails = 0;
		return;
	}
}

static void sao_mirror_leds(struct sao_slot *s)
{
	uint8_t cmd[MIRROR_MAX_LEDS * 3];
	uint16_t len = 0;
	saoh_err_t ret;

	for (uint16_t i = 0; i < s->led_count; i++) {
		// Alternate left/right headlight colors down the SAO's string.
		const uint8_t *px = headlights[i & 1];

		switch (s->led_mode) {
		case SAO_LEDITF_MODE_8B_RGB:
			cmd[len++] = px[0];
			cmd[len++] = px[1];
			cmd[len++] = px[2];
			break;
		case SAO_LEDITF_MODE_8B_GRB:
			cmd[len++] = px[1];
			cmd[len++] = px[0];
			cmd[len++] = px[2];
			break;
		case SAO_LEDITF_MODE_8B_MONO: {
			uint8_t v = px[0];
			if (px[1] > v) v = px[1];
			if (px[2] > v) v = px[2];
			cmd[len++] = v;
			break;
		}
		case SAO_LEDITF_MODE_1B_MONO:
		default:
			cmd[len++] = (px[0] | px[1] | px[2]) ? 1 : 0;
			break;
		}
	}

	ret = saoh_smbus_block_write(&sao_bus, s->pec_addr,
	                             (uint8_t)(s->led_base + SAO_LEDITF_CMD_LED_COMMAND),
	                             cmd, (uint8_t)len);
	if (ret != SAOH_ERR_OK)
		sao_slot_failed(s);
	else
		s->fails = 0;
}

// ---------- public API ----------

void sao_host_init(void)
{
	i2c_host_init();

	sao_bus.opaque = 0;
	memset(&sao_disc, 0, sizeof(sao_disc));
	sao_disc.bus = &sao_bus;
	sao_disc.discovery_cb = sao_discovered_cb;
	sao_disc.removal_cb = sao_removed_cb;
	sao_disc.rsvd_addrs = 0;
	sao_disc.rsvd_addrs_len = 0;

	next_scan_ms = BOOT_SCAN_DELAY_MS;
	next_rescan_ms = BOOT_SCAN_DELAY_MS + RESCAN_PERIOD_MS;
	next_mirror_ms = MIRROR_PERIOD_MS;
}

void sao_host_set_headlights(const uint8_t rgb[2][3])
{
	memcpy(headlights, rgb, sizeof(headlights));
}

void sao_host_set_led_takeover(uint8_t enable)
{
	enable = enable ? 1 : 0;
	if (enable == led_takeover)
		return;

	led_takeover = enable;
	for (int i = 0; i < MAX_SAOS; i++) {
		if (slots[i].state == SLOT_READY && slots[i].led_base)
			sao_led_control(&slots[i], enable);
	}
}

uint8_t sao_host_count(void)
{
	uint8_t n = 0;

	for (int i = 0; i < MAX_SAOS; i++)
		n += (slots[i].state != SLOT_EMPTY);
	return n;
}

uint8_t sao_host_find(uint16_t vid, uint16_t pid)
{
	for (int i = 0; i < MAX_SAOS; i++) {
		if (slots[i].state == SLOT_READY && slots[i].vid == vid && slots[i].pid == pid)
			return 1;
	}
	return 0;
}

void sao_host_task(uint32_t tick_ms)
{
	ms_now += tick_ms;

	// One bus-heavy action per tick, priority ordered, so the animation loop
	// never eats more than one scan/enumeration stall at a time.

	// Static (non-ARP) SAOs only turn up in a full reset scan; the periodic
	// ARP scans below never see a hot-plugged one. With nothing attached
	// there is nothing a reset scan can disturb, so keep running one.
	if (!full_scan_needed && sao_disc.bus_scanned && sao_host_count() == 0
	    && time_after(ms_now, next_empty_rescan_ms)) {
		next_empty_rescan_ms = ms_now + EMPTY_RESCAN_MS;
		full_scan_needed = 1;
	}

	if (full_scan_needed && time_after(ms_now, next_scan_ms)) {
		// discovery_reset does not fire removal callbacks: forget everything
		// first, discovery re-reports whatever is still attached.
		memset(slots, 0, sizeof(slots));
		full_scan_needed = 0;
		saoh_discovery_reset(&sao_disc);
		next_scan_ms = ms_now + SCAN_NEW_PERIOD_MS;
		next_rescan_ms = ms_now + RESCAN_PERIOD_MS;
		return;
	}

	if (!sao_disc.bus_scanned)
		return;

	for (int i = 0; i < MAX_SAOS; i++) {
		if (slots[i].state == SLOT_PENDING) {
			sao_enumerate(&slots[i]);
			return;
		}
	}

	if (time_after(ms_now, next_rescan_ms)) {
		next_rescan_ms = ms_now + RESCAN_PERIOD_MS;
		next_scan_ms = ms_now + SCAN_NEW_PERIOD_MS;
		saoh_discovery_rescan(&sao_disc);
		return;
	}

	if (time_after(ms_now, next_scan_ms)) {
		next_scan_ms = ms_now + SCAN_NEW_PERIOD_MS;
		saoh_discovery_scan_new(&sao_disc);
		return;
	}

	if (time_after(ms_now, next_heartbeat_ms)) {
		next_heartbeat_ms = ms_now + HEARTBEAT_PERIOD_MS;
		sao_heartbeat();
		return;
	}

	if (led_takeover && time_after(ms_now, next_mirror_ms)) {
		next_mirror_ms = ms_now + MIRROR_PERIOD_MS;
		for (int i = 0; i < MAX_SAOS; i++) {
			if (slots[i].state == SLOT_READY && slots[i].led_base)
				sao_mirror_leds(&slots[i]);
		}
	}
}
