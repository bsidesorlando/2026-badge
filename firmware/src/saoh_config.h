#ifndef SAOH_CONFIG_H
#define SAOH_CONFIG_H

// SAOv3 host library configuration. The library includes "saoh_config.h" by
// name, which resolves here via -Isrc.

// The CH32V003 I2C peripheral has no true SMBus block read (it cannot adjust
// the receive length from the first received byte), so the library reads the
// full maximum length and truncates. SAOv3 devices are required to pad
// over-length block reads, so this is fine.
#define SAOH_CFG_SUPPORT_SMBUS_BLKREAD 0

// LOGL_NONE: logging on a 16K part would drag in printf and the error-string
// table (which is also why err.c/log.c are excluded from the build in
// platformio.ini). Bump to 3 (LOGL_INFO) and add those sources back for bench
// debugging over the SWIO console.
#define SAOH_CFG_LOG_MIN_LEVEL 0

#define SAOH_CFG_CMD_RETRY_CNT 3

#endif
