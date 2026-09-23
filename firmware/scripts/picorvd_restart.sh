#!/usr/bin/env bash
# Restart a picorvd probe's firmware from the host, without touching the board.
#
# Opening the probe's console port at 2400 baud is a magic trigger handled in
# usb_task.cpp: it calls watchdog_reboot(), which re-runs Application::init()
# and with it PicoSWIO::reset(), re-establishing the target's debug interface.
# That is the same recovery as physically replugging the Pico.
#
# This still works when the probe's shell task is wedged, because the USB task
# runs at a higher FreeRTOS priority (3 vs 2) and keeps servicing USB even when
# the console has stopped echoing.
#
# Note 1200 baud is a DIFFERENT, pre-existing trigger that drops the probe into
# the BOOTSEL loader for reflashing. Do not confuse the two.
#
# Usage: picorvd_restart.sh [--bootsel]

set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=scripts/picorvd_common.sh
. "$HERE/picorvd_common.sh"

MODE=restart
if [ "${1:-}" = "--bootsel" ]; then
	MODE=bootsel
fi

if ! picorvd_resolve_ports; then
	echo "picorvd: probe not found (expected two CDC ports)" >&2
	echo "         set PICORVD_CONSOLE / PICORVD_PORT to override." >&2
	exit 1
fi

if [ "$MODE" = "bootsel" ]; then
	echo "picorvd: rebooting $CONSOLE into BOOTSEL"
	stty_dev "$CONSOLE" "$PICORVD_BAUD_BOOTSEL" || true
	( exec 3<>"$CONSOLE" && exec 3>&- ) 2>/dev/null || true
	sleep 3
	echo "picorvd: look for the RPI-RP2 volume, then copy pico_rvd.uf2 to it"
	exit 0
fi

echo "picorvd: restarting firmware via $CONSOLE"

if ! picorvd_restart; then
	echo "picorvd: probe did not come back within the timeout." >&2
	exit 1
fi

if picorvd_target_alive; then
	echo "picorvd: back up, target responding (part id $PICORVD_LAST_PART_ID)"
else
	echo "picorvd: back up, but the target is not responding" \
	     "(part id ${PICORVD_LAST_PART_ID:-none})" >&2
	echo "         run 'halt_on_reset' on $CONSOLE and toggle the target's VCC." >&2
	exit 1
fi
