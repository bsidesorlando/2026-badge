#ifndef _I2C_HOST_H
#define _I2C_HOST_H

// CH32V003 I2C1 in master mode, backing the SAOv3 host library's HAL
// callbacks (saoh_i2c_write_cb / saoh_i2c_read_cb / saoh_i2c_write_read_cb).
//
// SCL PC2 / SDA PC1, 100 kHz standard mode. The peripheral handles slave
// clock stretching natively; NAKs surface through the AF flag. Every wait is
// bounded by an SMBus-style 25 ms timeout, and a wedged bus (slave holding
// SDA low across a hot-plug) is recovered by clocking it out manually.

void i2c_host_init(void);

#endif
