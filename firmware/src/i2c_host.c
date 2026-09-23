// CH32V003 I2C1 master -- the SMBus/I2C HAL under the SAOv3 host library.
//
// The CH32V003's I2C block is the classic STM32F1-style engine minus the
// slave-side SMBus extras (no SMBUS/SMBTYPE/ENARP bits in CTLR1). Nothing a
// *master* needs is missing: START/STOP, repeated start, ACK/POS control,
// NAK via AF, and native tolerance of slave clock stretching. SMBus PEC is
// computed by the host library in software, so the hardware PEC unit is
// unused. What the hardware does not have is SMBus timeouts -- every flag
// wait below is bounded in software instead, and a wedged bus is recovered
// by bit-banging the clock (i2c_bus_clear).
//
// The receive sequences for 1, 2, and N>=3 bytes follow the F1 reference
// manual / AN2824 exactly -- this IP's ACK must be staged one byte ahead of
// time, which is what the POS bit and the BTF-based close-out are about.

#include "ch32fun.h"

#include "saoh_config.h"
#include "saoh/consts/err.h"
#include "saoh/hal.h"

#include "i2c_host.h"

// Per-flag wait bound. SMBus tTIMEOUT is 25-35 ms; a compliant stretching
// slave (e.g. the polled CH32 SAO device library) stays far under this.
#define I2C_TIMEOUT_TICKS (25 * DELAY_MS_TIME)

#define SDA_PIN 1   // PC1
#define SCL_PIN 2   // PC2

// ---------- pin + peripheral bring-up ----------

// Both bus pins to alternate function open drain, 50MHz (nibble 0xF).
// Only these two nibbles: PC6 belongs to the LED string.
static void i2c_pins_af(void)
{
	GPIOC->CFGLR = (GPIOC->CFGLR
		& ~(((uint32_t)0xF << (4 * SDA_PIN)) | ((uint32_t)0xF << (4 * SCL_PIN))))
		|  (((uint32_t)0xF << (4 * SDA_PIN)) | ((uint32_t)0xF << (4 * SCL_PIN)));
}

// Both bus pins to plain GPIO open drain (nibble 0x5 = 10MHz OD), released.
static void i2c_pins_gpio(void)
{
	GPIOC->BSHR = (1u << SDA_PIN) | (1u << SCL_PIN);
	GPIOC->CFGLR = (GPIOC->CFGLR
		& ~(((uint32_t)0xF << (4 * SDA_PIN)) | ((uint32_t)0xF << (4 * SCL_PIN))))
		|  (((uint32_t)0x5 << (4 * SDA_PIN)) | ((uint32_t)0x5 << (4 * SCL_PIN)));
}

static void i2c_periph_init(void)
{
	// Full peripheral reset so init is not at the mercy of whatever state a
	// failed transfer left behind.
	RCC->APB1PRSTR |= RCC_APB1Periph_I2C1;
	RCC->APB1PRSTR &= ~RCC_APB1Periph_I2C1;

	// APB1 runs at HCLK. FREQ field in MHz, then standard-mode 100 kHz:
	// CCR = Fpclk / (2 * Fscl).
	I2C1->CTLR2 = (uint16_t)(FUNCONF_SYSTEM_CORE_CLOCK / 1000000u);
	I2C1->CKCFGR = (uint16_t)(FUNCONF_SYSTEM_CORE_CLOCK / (2u * 100000u));

	I2C1->CTLR1 = I2C_CTLR1_PE;
}

// Slave stuck driving SDA low (classic mid-read hot-plug wedge): take the
// pins back as GPIO and clock SCL until the slave releases, then generate a
// STOP by hand and give the pins back to the peripheral.
static void i2c_bus_clear(void)
{
	i2c_pins_gpio();

	for (int i = 0; i < 9; i++) {
		if (GPIOC->INDR & (1u << SDA_PIN))
			break;
		GPIOC->BCR = (1u << SCL_PIN);
		Delay_Us(5);
		GPIOC->BSHR = (1u << SCL_PIN);
		Delay_Us(5);
	}

	// STOP: SDA low while SCL high, then release SDA.
	GPIOC->BCR = (1u << SDA_PIN);
	Delay_Us(5);
	GPIOC->BSHR = (1u << SDA_PIN);
	Delay_Us(5);

	i2c_periph_init();
	i2c_pins_af();
}

void i2c_host_init(void)
{
	RCC->APB2PCENR |= RCC_APB2Periph_GPIOC | RCC_APB2Periph_AFIO;
	RCC->APB1PCENR |= RCC_APB1Periph_I2C1;

	i2c_periph_init();

	// Pins go to AF *after* the peripheral is enabled: an AF open-drain pin
	// whose peripheral is disabled outputs 0 and would drag the bus low.
	i2c_pins_af();
}

// ---------- flag plumbing ----------

// Wait for a STAR1 flag. AF (slave NAK) and bus errors abort the wait; so
// does the timeout, which is what bounds a stretching or absent slave.
static saoh_err_t i2c_wait_flag(uint16_t flag)
{
	uint32_t start = SysTick->CNT;

	for (;;) {
		uint16_t s = I2C1->STAR1;
		if (s & I2C_STAR1_AF)
			return SAOH_ERR_NAK;
		if (s & (I2C_STAR1_BERR | I2C_STAR1_ARLO))
			return SAOH_ERR_SYS;
		if (s & flag)
			return SAOH_ERR_OK;
		if ((uint32_t)(SysTick->CNT - start) > I2C_TIMEOUT_TICKS)
			return SAOH_ERR_SYS;
	}
}

// Abort path shared by every transfer: clear the sticky error flags, free
// the bus with a STOP, and if the bus does not go idle (wedged slave),
// rebuild it. Returns its argument so call sites can `return i2c_fail(err)`.
static saoh_err_t i2c_fail(saoh_err_t err)
{
	uint32_t start = SysTick->CNT;

	I2C1->STAR1 = 0;   // AF/BERR/ARLO are write-0-to-clear
	I2C1->CTLR1 = (I2C1->CTLR1 & ~I2C_CTLR1_POS) | I2C_CTLR1_STOP;

	while (I2C1->STAR2 & I2C_STAR2_BUSY) {
		if ((uint32_t)(SysTick->CNT - start) > I2C_TIMEOUT_TICKS) {
			i2c_bus_clear();
			break;
		}
	}

	return err;
}

// Wait for the bus to be free before claiming it. Recovers a stuck bus once
// before giving up.
static saoh_err_t i2c_begin(void)
{
	for (int attempt = 0; attempt < 2; attempt++) {
		uint32_t start = SysTick->CNT;
		while (I2C1->STAR2 & I2C_STAR2_BUSY) {
			if ((uint32_t)(SysTick->CNT - start) > I2C_TIMEOUT_TICKS)
				goto stuck;
		}
		return SAOH_ERR_OK;
stuck:
		i2c_bus_clear();
	}

	return SAOH_ERR_SYS;
}

// START (or repeated START) + address. Leaves the ADDR flag pending -- the
// caller clears it, because for reads the ACK/STOP staging has to happen
// around that clear. A NAK here is an absent device: the normal ping result.
static saoh_err_t i2c_start(uint8_t addr_rw)
{
	saoh_err_t err;

	I2C1->CTLR1 |= I2C_CTLR1_START;

	err = i2c_wait_flag(I2C_STAR1_SB);
	if (err != SAOH_ERR_OK)
		return i2c_fail(SAOH_ERR_SYS);   // no NAK possible yet; anything is a bus fault

	(void)I2C1->STAR1;   // SB clears on STAR1 read + DATAR write
	I2C1->DATAR = addr_rw;

	err = i2c_wait_flag(I2C_STAR1_ADDR);
	if (err != SAOH_ERR_OK)
		return i2c_fail(err);

	return SAOH_ERR_OK;
}

#define I2C_CLEAR_ADDR() do { (void)I2C1->STAR1; (void)I2C1->STAR2; } while (0)

// ---------- transfer cores ----------

// Write phase. send_stop=0 leaves the transfer open (BTF set, SCL held) so a
// repeated START can follow -- that is what makes SMBus reads atomic.
static saoh_err_t i2c_do_write(uint8_t addr, const uint8_t *data, xferlen_t len, int send_stop)
{
	saoh_err_t err;

	err = i2c_start((uint8_t)(addr << 1));
	if (err != SAOH_ERR_OK)
		return err;

	I2C_CLEAR_ADDR();

	if (len == 0) {
		// SMBus quick command / address-only ping.
		I2C1->CTLR1 |= I2C_CTLR1_STOP;
		return SAOH_ERR_OK;
	}

	for (xferlen_t i = 0; i < len; i++) {
		err = i2c_wait_flag(I2C_STAR1_TXE);
		if (err != SAOH_ERR_OK)
			return i2c_fail(err);
		I2C1->DATAR = data[i];
	}

	// BTF = last byte fully on the wire and ACKed (AF here = data NAKed).
	err = i2c_wait_flag(I2C_STAR1_BTF);
	if (err != SAOH_ERR_OK)
		return i2c_fail(err);

	if (send_stop)
		I2C1->CTLR1 |= I2C_CTLR1_STOP;

	return SAOH_ERR_OK;
}

// Read phase, F1-style. May be entered with the bus already owned (repeated
// start after a write phase).
static saoh_err_t i2c_do_read(uint8_t addr, uint8_t *data, xferlen_t len)
{
	saoh_err_t err;
	uint8_t dummy;

	if (len == 0) {
		// A true SMBus quick read (no data byte at all) cannot be generated
		// by this IP -- it always clocks a byte in. Approximate with a
		// single discarded, NACKed byte. The host library never issues one
		// (its device ping is a quick write), so this is belt-and-braces.
		data = &dummy;
		len = 1;
	}

	if (len == 1) {
		I2C1->CTLR1 &= ~I2C_CTLR1_ACK;

		err = i2c_start((uint8_t)((addr << 1) | 1));
		if (err != SAOH_ERR_OK)
			return err;

		// ADDR-clear and STOP must land before the lone byte finishes.
		__disable_irq();
		I2C_CLEAR_ADDR();
		I2C1->CTLR1 |= I2C_CTLR1_STOP;
		__enable_irq();

		err = i2c_wait_flag(I2C_STAR1_RXNE);
		if (err != SAOH_ERR_OK)
			return i2c_fail(err);
		data[0] = (uint8_t)I2C1->DATAR;
	}
	else if (len == 2) {
		// POS: the NACK staged by clearing ACK applies to the *second* byte.
		I2C1->CTLR1 |= I2C_CTLR1_ACK | I2C_CTLR1_POS;

		err = i2c_start((uint8_t)((addr << 1) | 1));
		if (err != SAOH_ERR_OK) {
			I2C1->CTLR1 &= ~I2C_CTLR1_POS;
			return err;
		}

		__disable_irq();
		I2C_CLEAR_ADDR();
		I2C1->CTLR1 &= ~I2C_CTLR1_ACK;
		__enable_irq();

		err = i2c_wait_flag(I2C_STAR1_BTF);   // both bytes in DR + shift reg
		if (err != SAOH_ERR_OK) {
			I2C1->CTLR1 &= ~I2C_CTLR1_POS;
			return i2c_fail(err);
		}

		__disable_irq();
		I2C1->CTLR1 |= I2C_CTLR1_STOP;
		data[0] = (uint8_t)I2C1->DATAR;
		__enable_irq();
		data[1] = (uint8_t)I2C1->DATAR;

		I2C1->CTLR1 &= ~I2C_CTLR1_POS;
	}
	else {
		I2C1->CTLR1 |= I2C_CTLR1_ACK;

		err = i2c_start((uint8_t)((addr << 1) | 1));
		if (err != SAOH_ERR_OK)
			return err;

		I2C_CLEAR_ADDR();

		xferlen_t remaining = len;
		while (remaining > 3) {
			err = i2c_wait_flag(I2C_STAR1_RXNE);
			if (err != SAOH_ERR_OK)
				return i2c_fail(err);
			*data++ = (uint8_t)I2C1->DATAR;
			remaining--;
		}

		// Close-out per AN2824: with N-2 in DR and N-1 in the shift register
		// (BTF), un-ACK, read N-2 (starts N's reception, which gets the
		// NACK), STOP, read N-1, then N arrives normally.
		err = i2c_wait_flag(I2C_STAR1_BTF);
		if (err != SAOH_ERR_OK)
			return i2c_fail(err);

		I2C1->CTLR1 &= ~I2C_CTLR1_ACK;

		__disable_irq();
		*data++ = (uint8_t)I2C1->DATAR;
		I2C1->CTLR1 |= I2C_CTLR1_STOP;
		__enable_irq();

		*data++ = (uint8_t)I2C1->DATAR;

		err = i2c_wait_flag(I2C_STAR1_RXNE);
		if (err != SAOH_ERR_OK)
			return i2c_fail(err);
		*data++ = (uint8_t)I2C1->DATAR;
	}

	return SAOH_ERR_OK;
}

// ---------- SAOv3 host HAL ----------

saoh_err_t saoh_i2c_write_cb(void *opaque, uint8_t addr, const uint8_t *data, xferlen_t len)
{
	(void)opaque;
	saoh_err_t err;

	err = i2c_begin();
	if (err != SAOH_ERR_OK)
		return err;

	return i2c_do_write(addr, data, len, 1);
}

saoh_err_t saoh_i2c_read_cb(void *opaque, uint8_t addr, uint8_t *data, xferlen_t len)
{
	(void)opaque;
	saoh_err_t err;

	err = i2c_begin();
	if (err != SAOH_ERR_OK)
		return err;

	return i2c_do_read(addr, data, len);
}

saoh_err_t saoh_i2c_write_read_cb(void *opaque, uint8_t addr, const uint8_t *wdata, xferlen_t wlen,
                                  uint8_t *rdata, xferlen_t rlen)
{
	(void)opaque;
	saoh_err_t err;

	err = i2c_begin();
	if (err != SAOH_ERR_OK)
		return err;

	if (wlen) {
		err = i2c_do_write(addr, wdata, wlen, 0);   // no STOP: repeated start next
		if (err != SAOH_ERR_OK)
			return err;
	}

	return i2c_do_read(addr, rdata, rlen);
}
