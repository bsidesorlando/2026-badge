#!/usr/bin/env bash
# Flash a firmware ELF to a CH32V00x target through a picorvd probe (RP2040).
#
# Recovers on its own where it can. The SWIO link to the target drops
# intermittently, and the symptoms are unhelpful: gdb hangs forever on
# "target extended-remote" rather than erroring, because picorvd's stub only
# accepts an attach while the target is halted. So the link is checked FIRST and
# repaired before gdb is ever launched:
#
#   1. part_id reads a plausible value                -> go
#   2. otherwise init_swio, a few times               -> usually enough
#   3. otherwise restart the probe (2400 baud touch)  -> as good as replugging
#   4. otherwise give up with an actionable message   -> needs halt_on_reset
#
# Environment overrides:
#   PICORVD_PORT     GDB server port (skips autodetection)
#   PICORVD_CONSOLE  console port
#   PICORVD_GDB      path to a RISC-V gdb
#   PICORVD_NO_HALT  set to 1 to skip the automatic halt
#   PICORVD_TIMEOUT  seconds before a stuck gdb is killed (default 60)

set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=scripts/picorvd_common.sh
. "$HERE/picorvd_common.sh"

ELF="${1:-}"
if [ -z "$ELF" ]; then
	echo "usage: picorvd_upload.sh <firmware.elf>" >&2
	exit 2
fi
if [ ! -f "$ELF" ]; then
	echo "picorvd: no such file: $ELF" >&2
	exit 2
fi

if ! picorvd_resolve_ports; then
	echo "picorvd: probe not found (expected two CDC ports)" >&2
	echo "         is the Pico plugged in? set PICORVD_CONSOLE / PICORVD_PORT." >&2
	exit 1
fi

# --- locate a RISC-V gdb --------------------------------------------------
if [ -n "${PICORVD_GDB:-}" ]; then
	GDB="$PICORVD_GDB"
elif command -v riscv-wch-elf-gdb >/dev/null 2>&1; then
	GDB=riscv-wch-elf-gdb
elif [ -x "$HOME/.platformio/packages/toolchain-riscv/bin/riscv-wch-elf-gdb" ]; then
	GDB="$HOME/.platformio/packages/toolchain-riscv/bin/riscv-wch-elf-gdb"
elif command -v gdb-multiarch >/dev/null 2>&1; then
	GDB=gdb-multiarch
else
	echo "picorvd: no RISC-V gdb found; set PICORVD_GDB to its path." >&2
	echo "         (a plain 'gdb' is usually host-native and cannot load rv32 ELFs)" >&2
	exit 1
fi

# --- make sure the target is actually there -------------------------------
ensure_target() {
	picorvd_target_alive && return 0

	echo "picorvd: target not responding (part id ${PICORVD_LAST_PART_ID:-none})," \
	     "re-initializing SWIO" >&2

	for _try in 1 2 3; do
		picorvd_console_cmd init_swio >/dev/null
		if picorvd_target_alive; then
			echo "picorvd: link recovered (part id $PICORVD_LAST_PART_ID)"
			return 0
		fi
	done

	# Deliberately NOT sending `reset` here. On a dead link every debug-module
	# read is all-ones, and older probe firmware spins forever in
	# RVDebug::reset() waiting for ALLHAVERESET to clear, killing the shell.
	echo "picorvd: still nothing, restarting the probe" >&2

	if ! picorvd_restart; then
		echo "picorvd: probe did not come back after the restart." >&2
		return 1
	fi

	for _try in 1 2 3; do
		picorvd_console_cmd init_swio >/dev/null
		if picorvd_target_alive; then
			echo "picorvd: link recovered after restart (part id $PICORVD_LAST_PART_ID)"
			return 0
		fi
	done

	return 1
}

if ! ensure_target; then
	echo "picorvd: target still not responding." >&2
	echo "         Open $CONSOLE, run 'halt_on_reset', and toggle the target's" >&2
	echo "         VCC when it prompts. Then flash again." >&2
	exit 1
fi

# --- halt, so the GDB stub will accept an attach --------------------------
if [ "${PICORVD_NO_HALT:-0}" != "1" ]; then
	picorvd_console_cmd halt >/dev/null
	echo "picorvd: halt requested via $CONSOLE"
fi

echo "picorvd: flashing $(basename "$ELF") via $PORT"

# -nx so a stray ~/.gdbinit cannot redirect us at a different target.
"$GDB" -nx --batch \
	-ex "set confirm off" \
	-ex "set remotetimeout 30" \
	-ex "target extended-remote $PORT" \
	-ex "load" \
	-ex "set \$pc = 0" \
	-ex "detach" \
	"$ELF" &
GDB_PID=$!

# Watchdog: macOS has no coreutils timeout(1), and an attach against an
# unresponsive target blocks forever. Poll in 1s steps and exit as soon as gdb
# is gone -- a single long sleep would outlive a kill and, still holding stdout,
# block whoever is capturing our output.
TIMEOUT="${PICORVD_TIMEOUT:-60}"
(
	i=0
	while [ "$i" -lt "$TIMEOUT" ]; do
		kill -0 "$GDB_PID" 2>/dev/null || exit 0
		sleep 1
		i=$((i + 1))
	done
	if kill -0 "$GDB_PID" 2>/dev/null; then
		echo "" >&2
		echo "picorvd: gdb still running after ${TIMEOUT}s, killing it." >&2
		kill -9 "$GDB_PID" 2>/dev/null || true
	fi
) &
WATCHDOG_PID=$!

RC=0
wait "$GDB_PID" || RC=$?
kill "$WATCHDOG_PID" 2>/dev/null || true
wait "$WATCHDOG_PID" 2>/dev/null || true

# --- leave the target running ---------------------------------------------
# gdb's `detach` leaves the hart halted, and its `load` latches ALLHAVERESET,
# which makes RVDebug::resume() refuse ("Can't resume while in reset!").
# RVDebug::reset() is the only path that sends ACKHAVERESET, so reset first.
# Safe to do here: the link was verified above.
if [ "$RC" -eq 0 ]; then
	picorvd_console_cmd reset >/dev/null
	REPLY_TXT=$(picorvd_console_cmd resume)

	case "$REPLY_TXT" in
		*"Resume OK"*)
			echo "picorvd: target resumed"
			;;
		*"Resume failed"*)
			echo "picorvd: RESUME FAILED -- the firmware is NOT running." >&2
			echo "         Run 'halt_on_reset' on $CONSOLE and toggle VCC." >&2
			RC=1
			;;
		*)
			echo "picorvd: resume sent, but no confirmation read back." >&2
			echo "         Verify on $CONSOLE before assuming it is running." >&2
			;;
	esac
fi

exit "$RC"
