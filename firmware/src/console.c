// Serial console: USART1 on PD0 (TX) / PD1 (RX), interrupt-driven ring
// buffers, a minimal line editor, and wake/sleep arbitration against SAOs
// that may want the GPIO pins (see console.h).

#include "ch32fun.h"

#include "console.h"
#include "game.h"
#include "sao_host.h"
#include "selftest.h"
#include "util.h"

#include <string.h>

#define CON_BAUD 115200

// Both sizes must be powers of two. RX holds a whole pasted line (LINE_MAX
// plus its ending): the main loop only drains it every tick, and a line
// arrives in ~4 ms.
#define RX_SIZE 64
#define TX_SIZE 128

#define LINE_MAX 48

static volatile uint8_t rx_buf[RX_SIZE];
static volatile uint8_t rx_head, rx_tail;
static volatile uint8_t tx_buf[TX_SIZE];
static volatile uint8_t tx_head, tx_tail;

static uint8_t  awake;

static char    line[LINE_MAX + 1];
static uint8_t line_len;
static uint8_t esc_state;   // swallowing an ANSI escape sequence (arrow keys)
static char    prev_c;      // CR LF is one line ending, not two
static uint8_t con_polled;  // in the fault handler: write the UART directly

// While asleep (no line editor running), watches the raw input stream for a
// "selftest" line. SELFTEST_DEAD: this line already went off the rails; wait
// for its end. While awake, the edited line is checked instead (con_rx), so
// backspace and ^C don't get in the way.
static const char selftest_cmd[] = "selftest";
#define SELFTEST_CMD_LEN (sizeof(selftest_cmd) - 1)
#define SELFTEST_DEAD    0xFF
static uint8_t selftest_match;

void USART1_IRQHandler(void) __attribute__((interrupt));
void USART1_IRQHandler(void)
{
	uint16_t st = USART1->STATR;

	// Reading STATR then DATAR also clears ORE/FE/NE. Bytes that arrived
	// with framing or noise errors are dropped: that is what a floating or
	// SWIO-busy pin produces.
	if (st & (USART_STATR_RXNE | USART_STATR_ORE)) {
		uint8_t c = (uint8_t)USART1->DATAR;
		uint8_t next = (uint8_t)((rx_head + 1) & (RX_SIZE - 1));

		if (!(st & (USART_STATR_FE | USART_STATR_NE)) && next != rx_tail) {
			rx_buf[rx_head] = c;
			rx_head = next;
		}
	}

	if ((USART1->CTLR1 & USART_CTLR1_TXEIE) && (st & USART_STATR_TXE)) {
		if (tx_tail == tx_head) {
			USART1->CTLR1 &= ~USART_CTLR1_TXEIE;
		}
		else {
			USART1->DATAR = tx_buf[tx_tail];
			tx_tail = (uint8_t)((tx_tail + 1) & (TX_SIZE - 1));
		}
	}
}

// PD0 nibble: 0x9 = AF push-pull (TX driving), 0x4 = floating input.
static void tx_pin(uint8_t drive)
{
	uint32_t mode = drive ? GPIO_CFGLR_OUT_10Mhz_AF_PP : GPIO_CFGLR_IN_FLOAT;

	GPIOD->CFGLR = (GPIOD->CFGLR & ~(0xFu << (4 * 0))) | (mode << (4 * 0));
}

void con_init(void)
{
	RCC->APB2PCENR |= RCC_APB2Periph_USART1 | RCC_APB2Periph_AFIO | RCC_APB2Periph_GPIOD;

	// USART1 partial remap 1: TX PD0, RX PD1.
	AFIO->PCFR1 = (AFIO->PCFR1 & ~AFIO_PCFR1_USART1_REMAP_1) | AFIO_PCFR1_USART1_REMAP;

	// RX: input with the weak internal pull-up, so an empty SAO header idles
	// high instead of floating into a stream of framing errors.
	GPIOD->BSHR = 1u << 1;
	GPIOD->CFGLR = (GPIOD->CFGLR & ~(0xFu << (4 * 1))) | ((uint32_t)GPIO_CFGLR_IN_PUPD << (4 * 1));
	tx_pin(0);

	USART1->BRR = (FUNCONF_SYSTEM_CORE_CLOCK + CON_BAUD / 2) / CON_BAUD;
	USART1->CTLR1 = USART_CTLR1_UE | USART_CTLR1_TE | USART_CTLR1_RE | USART_CTLR1_RXNEIE;

	NVIC_EnableIRQ(USART1_IRQn);
}

uint8_t con_active(void)
{
	return awake;
}

void con_putc(char c)
{
	if (!awake)
		return;

	if (c == '\n')
		con_putc('\r');

	if (con_polled) {
		while (!(USART1->STATR & USART_STATR_TXE)) { }
		USART1->DATAR = (uint8_t)c;
		return;
	}

	uint8_t next = (uint8_t)((tx_head + 1) & (TX_SIZE - 1));
	while (next == tx_tail) { }   // the ISR drains it

	tx_buf[tx_head] = (uint8_t)c;
	tx_head = next;
	USART1->CTLR1 |= USART_CTLR1_TXEIE;
}

void con_puts(const char *s)
{
	while (*s)
		con_putc(*s++);
}

void con_putu(uint32_t v)
{
	char buf[11];
	int i = sizeof(buf) - 1;

	buf[i] = 0;
	do {
		buf[--i] = (char)('0' + v % 10);
		v /= 10;
	} while (v);

	con_puts(&buf[i]);
}

void con_puthex(uint32_t v, uint8_t digits)
{
	while (digits--)
		con_putc("0123456789abcdef"[(v >> (4 * digits)) & 0xF]);
}

void con_reboot(void)
{
	while (tx_tail != tx_head) { }                   // the ISR drains it
	while (!(USART1->STATR & USART_STATR_TC)) { }   // last stop bit out
	NVIC_SystemReset();
	for (;;) { }
}

// ---------- faults ----------
// Any CPU exception -- in practice a PEEK at an address the chip doesn't
// implement -- lands here instead of ch32fun's default spin-forever handler:
// say what happened on the console, then reset the badge. Interrupts are off
// in here, so con_putc() switches to polling the UART (see con_polled).
//
// Never returns, so no interrupt attribute: saving and restoring registers
// for a return that never comes only costs flash. It has to live in the same
// section as ch32fun's weak default it replaces.
void HardFault_Handler(void) __attribute__((section(VECTOR_HANDLER_SECTION), used, noreturn));
void HardFault_Handler(void)
{
	if (awake) {
		// Whatever was still queued goes out first (e.g. the "0x...: " of
		// the PEEK that crashed).
		con_polled = 1;
		while (tx_tail != tx_head) {
			uint8_t c = tx_buf[tx_tail];
			tx_tail = (uint8_t)((tx_tail + 1) & (TX_SIZE - 1));
			con_putc((char)c);
		}
		// Just the cause: this core leaves mtval at 0 on load faults, so
		// there is no faulting address to report.
		con_puts("\n*** CRASH: mcause ");
		con_puthex(__get_MCAUSE(), 8);
		con_puts(" -- rebooting ***\n");
		while (!(USART1->STATR & USART_STATR_TC)) { }   // last stop bit out
	}

	NVIC_SystemReset();
	for (;;) { }
}

static void con_wake(void)
{
	awake = 1;
	line_len = 0;
	esc_state = 0;
	tx_pin(1);

	game_banner();
	con_puts("> ");
}

static void con_sleep(void)
{
	USART1->CTLR1 &= ~USART_CTLR1_TXEIE;
	tx_tail = tx_head;
	tx_pin(0);
	awake = 0;
}

static void con_rx(char c)
{
	if (esc_state) {
		// ESC [ <params> <final>: done at the first byte in @..~.
		if (esc_state == 1 || !(c >= '@' && c <= '~'))
			esc_state = (c == '[' || esc_state > 1) ? 2 : 0;
		else
			esc_state = 0;
		return;
	}

	switch (c) {
	case 0x1B:
		esc_state = 1;
		break;

	case '\r':
	case '\n':
		con_puts("\n");
		line[line_len] = 0;
		line_len = 0;
		if (!strcmp(line, selftest_cmd)) {
			selftest_start();
			con_puts("Self-test: hold both pads to exit.\n");
		}
		else {
			game_command(line);
		}
		if (awake)
			con_puts("> ");
		break;

	case 0x08:
	case 0x7F:
		if (line_len) {
			line_len--;
			con_puts("\b \b");
		}
		break;

	case 0x03:   // ^C
		line_len = 0;
		con_puts("^C\n> ");
		break;

	default:
		if (c >= ' ' && c <= '~' && line_len < LINE_MAX) {
			line[line_len++] = c;
			con_putc(c);
		}
		break;
	}
}

void con_tick(void)
{
	uint8_t sao_present = sao_host_count() != 0;

	if (awake && sao_present)
		con_sleep();

	while (rx_tail != rx_head) {
		char c = (char)rx_buf[rx_tail];
		rx_tail = (uint8_t)((rx_tail + 1) & (RX_SIZE - 1));

		if ((uint8_t)c == SELFTEST_BYTE) {
			selftest_start();
			continue;
		}

		char prev = prev_c;
		prev_c = c;
		if (c == '\n' && prev == '\r')
			continue;

		if (!awake) {
			if (c == '\r' || c == '\n') {
				if (selftest_match == SELFTEST_CMD_LEN)
					selftest_start();
				else if (!sao_present)
					con_wake();
				selftest_match = 0;
			}
			else if (selftest_match < SELFTEST_CMD_LEN && c == selftest_cmd[selftest_match]) {
				selftest_match++;
			}
			else {
				selftest_match = SELFTEST_DEAD;
			}
			continue;
		}

		con_rx(c);
	}
}
