# Shared helpers for talking to a picorvd probe (an RP2040 acting as a
# ch32v003 debugger). Sourced by picorvd_upload.sh and picorvd_restart.sh.
#
# picorvd enumerates two CDC ACM interfaces: the lower-numbered one is the
# interactive console, the higher-numbered one is the GDB server. Which names
# they get depends on which physical USB port the probe is plugged into, so
# they are resolved at run time rather than hardcoded.
#
# Environment overrides:
#   PICORVD_PORT     GDB server port (skips autodetection)
#   PICORVD_CONSOLE  console port
#   PICORVD_GDB      path to a RISC-V gdb

# Magic baud rates, handled in src/usb/usb_task.cpp on the probe:
#   1200 -> reboot into the BOOTSEL loader (for reflashing the probe itself)
#   2400 -> restart the firmware
# Both are serviced by the USB task, which runs at a higher FreeRTOS priority
# than the shell (3 vs 2), so they still work when the shell task is wedged.
PICORVD_BAUD_BOOTSEL=1200
PICORVD_BAUD_RESTART=2400

# BSD stty selects the device with -f, GNU stty with -F.
stty_dev() {
	_dev=$1
	shift
	stty -f "$_dev" "$@" 2>/dev/null || stty -F "$_dev" "$@" 2>/dev/null
}

# Populate CONSOLE and PORT. Returns 1 if the probe is not present.
picorvd_resolve_ports() {
	CONSOLE="${PICORVD_CONSOLE:-}"
	PORT="${PICORVD_PORT:-}"

	if [ -n "$CONSOLE" ] && [ -n "$PORT" ]; then
		return 0
	fi

	case "$(uname -s)" in
		Darwin) _pattern='/dev/cu.usbmodem*' ;;
		*)      _pattern='/dev/ttyACM*' ;;
	esac

	# shellcheck disable=SC2086
	_ports=$(ls $_pattern 2>/dev/null | sort || true)
	_count=$(printf '%s\n' "$_ports" | grep -c . || true)

	if [ "$_count" -ne 2 ]; then
		return 1
	fi

	[ -n "$CONSOLE" ] || CONSOLE=$(printf '%s\n' "$_ports" | head -1)
	[ -n "$PORT" ]    || PORT=$(printf '%s\n' "$_ports" | tail -1)

	return 0
}

# Wait for the probe to enumerate, up to roughly $1 seconds (default 15).
picorvd_wait_for_ports() {
	_limit=${1:-15}
	_waited=0

	while [ "$_waited" -lt "$_limit" ]; do
		if picorvd_resolve_ports && [ -c "$CONSOLE" ]; then
			# Settle: the port can appear a moment before it is usable.
			sleep 1
			return 0
		fi
		sleep 1
		_waited=$((_waited + 1))
	done

	return 1
}

# Send one console command and echo whatever comes back.
#
# Holds a single descriptor open for both directions: using separate redirects
# and then reopening to read loses the reply, because closing a tty discards its
# buffered input. `read -t 1` bounds each line and the counter bounds the loop,
# so this cannot hang.
picorvd_console_cmd() {
	_cmd=$1
	_reply=""

	if exec 3<>"$CONSOLE" 2>/dev/null; then
		stty_dev "$CONSOLE" 115200 raw -echo || true
		printf '%s\r\n' "$_cmd" >&3
		sleep 0.4

		_n=0
		while [ "$_n" -lt 60 ] && IFS= read -r -t 1 _line <&3; do
			_reply="$_reply$_line
"
			_n=$((_n + 1))
		done

		exec 3>&-
	fi

	printf '%s' "$_reply"
}

# True if the target answers with a plausible part ID. All-ones means SWIO is
# not in debug mode; all-zeros means the debug module is not answering either.
# This mirrors check_part_id() in the probe's own debug_commands.cpp.
#
# Asks twice and takes the last answer: the first read after re-initializing the
# link can return the previous transaction's data.
picorvd_target_alive() {
	_out=$(picorvd_console_cmd part_id; picorvd_console_cmd part_id)
	_id=$(printf '%s' "$_out" \
		| grep -oE 'DM_PARTID = 0x[0-9A-Fa-f]{8}' \
		| tail -1 \
		| grep -oE '0x[0-9A-Fa-f]{8}')

	PICORVD_LAST_PART_ID="$_id"

	case "$_id" in
		"") return 1 ;;
		0x[fF][fF][fF][fF][fF][fF][fF][fF]) return 1 ;;
		0x00000000) return 1 ;;
		*) return 0 ;;
	esac
}

# Restart the probe's firmware. This re-runs Application::init(), and with it
# PicoSWIO::reset(), which re-establishes the target's debug interface -- the
# same recovery as physically replugging the Pico.
picorvd_restart() {
	# Merely opening the port at the magic rate is the trigger; the probe acts
	# on the line-coding change, so there is nothing to write.
	stty_dev "$CONSOLE" "$PICORVD_BAUD_RESTART" || true
	( exec 3<>"$CONSOLE" && exec 3>&- ) 2>/dev/null || true

	# The probe drops off the bus and comes back.
	sleep 3
	picorvd_wait_for_ports 20
}
