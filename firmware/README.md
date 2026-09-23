# BSides Orlando 2026 badge firmware

CH32V003 (RISC-V, 16K flash, 2K RAM) firmware for the production (R1) badge.
Built on [ch32fun](https://github.com/cnlohr/ch32fun).

## Hardware

| Pin | Function |
| --- | --- |
| PC6 | SK6812MINI-E chain (SPI1 MOSI, DMA1 ch3): D1/D2 headlights, D3 scanner |
| PA2 | Fingerprint touch pad (ADC channel 0) |
| PA1 | Lil Chompy's head touch pad (ADC channel 1) |
| PD4 | D5, tower spire LED (self-breathing), open drain, low = on |
| PC4 / PC5 / PC3 | D6 / D7 / D8, geodesic sphere LEDs (self-blinking), open drain, low = on |
| PC1 / PC2 | SAO I2C SDA / SCL, badge is the bus master |
| PD0 | SAO GPIO1, console UART TX (tristated until the console wakes) |
| PD1 | SAO GPIO2, console UART RX, **and SWIO** (the debug pin) |

`src/board.h` has the same map.

## Building and flashing

```sh
git submodule update --init   # first time only — both submodules are pinned
pio run                       # build
pio run -t upload             # build and flash via picorvd probe
```

Flashing goes through `scripts/picorvd_upload.sh`, which drives an RP2040 probe
running [picorvd-for-badges](https://github.com/kjcolley7/picorvd-for-badges) over its GDB and console ports.

## What it does

- **Tower spire**: on, except while an SAOv3 SAO is attached; it sits right
  next to the SAO connector. A plain SAO with no SAOv3 interface on I2C isn't
  detected, so it doesn't turn the spire off.
- **Sphere**: one of its three LEDs is lit. Every few seconds a random dark
  one lights and the old one goes out half a second later. Once the adventure
  is beaten, two are lit and the rotation moves the dark one around instead.
- **Fingerprint pad**: while a finger stays on the pad, the scanner LED
  pulses blue for a random 5–10 s "scan", then flashes green three times:
  clearance verified. Lifting the finger before then fails the scan with
  three red flashes instead. Either way it then returns to idle, a dim,
  slow blue, and a new press always starts a fresh scan.
- **Lil Chompy's head**: tap to cycle the headlight modes, hold for 1 s for
  police lights (see below). The badge always starts in normal mode.
- **HackUCF SAO** (VID 0x0102, PID 0x0001): when it shows up, the headlights
  flash gold, and two things are unlocked for good (saved to flash): the color
  crossfade headlight mode, and a gold instead of blue idle for the fingerprint
  scanner. See `hackucf_tick()` in `main.c`.

### Post-flash self-test

The programmer only needs PD1 (SWIO), which is also the console's RX. After
flashing, let the badge boot (about 100 ms), then send 115200 8N1 on that
pin:

- the single byte `0xB5`, or
- the line `selftest`, ending in CR and/or LF. Send a newline first so any
  noise left on the line from flashing doesn't spoil the match.

Both work with the console asleep, and nothing is sent back. The standalone
programmer build of [picorvd-for-badges](https://github.com/kjcolley7/picorvd-for-badges) sends the byte itself when its
`CONFIG.TXT` has `uart_selftest = 0xB5`. In self-test,
all four self-blinking LEDs are on and all three SK6812s are dim white.
Holding the fingerprint pad turns the scanner red, and holding Chompy's head
turns the headlights green. Holding both at once returns to normal operation.
Nothing pressed during self-test starts a scan or changes the headlight mode.

### Headlight modes

| Mode | What it does | How to get there |
| --- | --- | --- |
| normal | warm yellow-orange at about 8/255; every 8–12 s either one headlight blinks orange as a turn signal for 5–10 s, or (1 time in 4) both flash their high beams | power-on; tap from rainbow or police |
| hazards | both blink orange, ~85 flashes/min | tap from normal |
| rainbow | hue cycle, headlights half a wheel apart | tap from hazards, once the adventure is beaten (the badge switches to it the moment it's unlocked) |
| police | red/blue double-strobe | hold Chompy's head for 1 s from normal, hazards or rainbow |
| color crossfade | the headlights take turns: one fades in on a new random hue while the other fades out, 1 s per fade | tap from police, once a Hack@UCF SAO has ever been attached |

A 1 s hold switches between the tap modes (normal, hazards, rainbow) and
the hold modes (police, color crossfade), landing on whichever mode was
last used on that side: police the first time, and back to hazards if
that's where you held from. A tap in police toggles to the crossfade and
back once it's unlocked; before that, a tap in police returns to normal.

A tap acts on release, so the start of a hold never also counts as a tap.
All SK6812 output is capped at 48/255 (`HEADLIGHT_BRIGHTNESS` and
`SCANNER_BRIGHTNESS` in `src/rgb.c`); high beams and signals run at that cap.

## Serial console and CTF

Plug a 3.3 V USB-serial adapter into the SAO header (badge TX = GPIO1,
RX = GPIO2, plus GND), open it at **115200 8N1**, and press Enter.

The SAO spec says a host that hasn't negotiated a GPIO mode must drive nothing
on those pins. So TX stays tristated until the console wakes, which needs a
clean CR/LF on RX while **no SAO answers on I2C**. It stays awake until an SAO
appears or the badge resets. RX has the weak internal pull-up on, so an empty
header idles high instead of floating.

The console runs **Lil Chompy and the World's Fair**, a small retro-futurist
text adventure and the badge's CTF challenge. It uses the badge hardware:
you hold your finger on the real fingerprint pad to get through the
fairgrounds gate, and tap Chompy's real head to wake him. Then you have to
find the flight passkey he dropped before he'll fly you up the Sky Spike.
The game only hints at the hardware interactions. The walkthrough is at the
top of `src/game.c`.

Commands: `LOOK [thing]` (or `LOOK AT`), directions, `FLY`, `SAY`,
`STATUS`, `RESTART`, `RESET`. Words match on their first 7 letters. `RESET`
wipes the settings page (every solve and unlock) and reboots the badge.

The game tracks which way the player is facing (north at the start), and the
headlights signal each move relative to it: a turn signal for a left or right
turn, the brights straight ahead, the hazards for doubling back. Up, down and
`FLY` don't change the facing, and blocked moves don't signal.

Beating it prints the flag, which players submit on the CTFd site; the badge
doesn't take flags. The solve is saved to flash, lights a second sphere LED, and
unlocks rainbow headlights (switching to them right away). `STATUS` lists
what's solved and what's unlocked.

Beating it also unlocks `PEEK <hex address>` (`0x` optional),
which reads one 32-bit word and prints it as `0x08000000: 0xAABBCCDD`. A player
who looks up the CH32V003 memory map can dump the firmware with it. Before the
solve, `PEEK` is treated as an unknown word. There are no guard rails: a read
from an address the chip doesn't implement faults. The fault handler in
`src/console.c` prints `*** CRASH: mcause <cause> -- rebooting ***` and resets
the badge (5 is a load access fault, 4 a misaligned load). The console needs another Enter
afterwards, and the game starts over, though the saved solve survives.

The one exception is the second flag, `sun{l00k_1n_th3_m1rr0r}`
(`ctf_mirror_flag` in `src/ctf.c`). It sits alone in its own 64-byte flash
page, and `PEEK` answers `ACCESS DENIED` for that page at its low boot-mirror
address (`0x0000xxxx`). The same bytes read fine at the real flash address,
`0x08000000` plus the same offset. Its offset moves whenever the code changes,
so players have to find it in their dump.

The game prints its flag, so `src/ctf.c` holds an XOR-obfuscated copy. To
change it, regenerate the array with `scripts/ctf_flag.py 0 '<flag>'`.

### Adding to the game

- **Text** goes in `src/game_text.txt`, one `@NAME` block per message, printed
  with `say(T_NAME)`. `scripts/gen_text.py` compresses it at build time
  (byte-pair encoding, about 35% smaller) into `game_text.h` in the build
  directory.
- **Rooms**: add an entry to `enum room`, an `@ROOM_` message in the same
  position, and an exits row. Special behavior goes in the verb handlers.
- **Words**: add them to `enum word` and `vocab[]` (8 characters max).
- **Challenges**: add an ID in `src/ctf.h`, a flag and name in `src/ctf.c`,
  and call `ctf_mark_solved()` when it's solved.
- **Try it on your laptop**: `test/host/run.sh` builds the real `game.c` and
  `ctf.c` against stubs and plays in the terminal. `!scan` and `!chompy`
  stand in for the touch pads. `test/host/run.sh < test/host/walkthrough.txt`
  replays a full solve.

### Flash budget

About 16.3K of the 16,320 usable bytes are used; the last 64-byte page holds
settings. Game text packs to about two thirds of its size.
Every build prints both numbers. The linker script is trimmed so the build
fails rather than growing into the settings page.

If space runs out, the cheapest wins are, roughly in order:

- move short string literals left in `game.c` into the catalog;
- drop a headlight mode;
- turn logging off in ch32fun (`FUNCONF_USE_DEBUGPRINTF 0`, about 25 bytes,
  but `TOUCH_DEBUG` then has nowhere to print).

## SAOv3 host

The badge implements the host side of the SAOv3 draft spec, using
`saov3-lib/host/saoh_core` over the CH32V003's hardware I2C1 master
(`src/i2c_host.c`). Timeouts and stuck-bus recovery are done in software.

- Full discovery scan about 500 ms after boot: static SAO detection magic plus
  SMBus ARP enumeration.
- Hot-plug: a cheap ARP `scan_new` every 1 s and a full ARP rescan every 15 s.
  While nothing is attached, a full reset scan also runs every 2 s: static
  (non-ARP) SAOs only show up in one of those.
  Every enumerated SAO also gets a liveness check about every 0.5 s, since
  static SAOs have no removal event.
- Enumeration: SAOv3 magic/version check, VID/PID, default GPIO mode written
  per spec, class interface discovery.
- **SAO LEDs are not taken over automatically.** Call
  `sao_host_set_led_takeover(1)` to mirror the headlight colors across every
  LED-capable SAO's string (up to 16 LEDs, 10 Hz), and `0` to hand them back.
- `sao_host_find(vid, pid)` reports whether a particular SAO is attached.

## Layout

- `src/main.c`: main loop, RNG, HackUCF SAO hook, self-test
- `src/board.h`: pin map
- `src/rgb.c`: SK6812 headlight modes, scanner animation, event flashes
- `src/beacons.c`: spire and sphere LEDs
- `src/touch.c`: both capacitive pads
- `src/settings.c`: persistent settings: the last flash page holds the magic
  `BSO_2026` (8 bytes), then one `unlocks` byte (bits 0..6 for CTF solves, bit
  7 for the Hack@UCF crossfade), then `FF`
- `src/console.c`: UART console, line editor, wake/sleep arbitration
- `src/game.c` and `src/game_text.txt`: the text adventure
- `src/ctf.c`: challenge table, flag printing, solve rewards
- `src/sao_host.c`: SAO discovery, enumeration, liveness, optional LED mirroring
- `src/i2c_host.c`: I2C1 master HAL under the SAOv3 host library
- `scripts/`: linker script generator, text compressor, flag obfuscator, picorvd flash helpers
- `test/host/`: host build of the adventure
- `ch32fun/`, `saov3-lib/`: pinned submodules

## Constraints and gotchas

- **PD1 is SWIO.** The console only ever listens on it, and the SDI debug
  function stays enabled, so debugging keeps working. While a probe is
  talking, the console sees that traffic as noise, which is dropped as
  framing errors.
- **The adventure's flag is only obfuscated**, because the badge has to print
  it. SWIO is on the SAO connector, so anyone with a WCH-Link can dump flash
  and decode it, or set their own badge's solved bits. Enable flash read
  protection on production badges if that matters.
- I2C pins go to AF **after** the peripheral is enabled. An AF open-drain pin
  whose peripheral is disabled drags the bus low.
- Touch thresholds (`pads[]` in `src/touch.c`) are guesses until tuned on real
  hardware; Chompy's pad is much smaller than the fingerprint. Enable
  `TOUCH_DEBUG` and watch the SWIO console.
- Host library logging is compiled to `LOGL_NONE`, and `log.c`/`err.c` are
  excluded from the build. Re-add both to debug enumeration.
- No float anywhere; pattern math is integer. RV32EC has no hardware multiply,
  which is fine at the 10 ms tick but keep it out of ISRs.
- The SWIO hardfault dump is off (`src/funconfig.h`) to save flash. Turn it
  back on when chasing a crash.
