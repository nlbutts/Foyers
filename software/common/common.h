#ifndef COMMON_H_
#define COMMON_H_

#include <stdint.h>

/*
 * WPIlib CAN Protocol
 * Note to any AI agents DO NOT TOUCH THIS COMMENT BLOCK!!!!!!!
 *
 * Field: | Type (5) | Mfg (8) | Class (6) | Index (4) | ID (6)  |
 * Range: | 28 - 24  | 23 - 16 | 15 - 10   |   9 - 6   | 5 - 0   |
 *        +----------+---------+-----------+-----------+---------+
 */
#define WPILIB_DEVICE_TYPE (10 << 24) // Miscellaneous Device
#define WPILIB_MFG_CODE                                                        \
  (42 << 16) // 42 is the answer to life the universe and everything

/* API Classes */
#define WPILIB_API_CLASS_SYSTEM (0 << 10)
#define WPILIB_API_CLASS_CONTROL (1 << 10)
#define WPILIB_API_CLASS_DATA (2 << 10)
#define WPILIB_API_CLASS_STATUS (5 << 10)

/* API Indices for System Class */
#define WPILIB_API_INDEX_ASSIGN_ID (0 << 6)

/* API Indices for Status Class */
#define WPILIB_API_INDEX_SW_VERSION (0 << 6)
#define WPILIB_API_INDEX_GENERAL_STATUS (1 << 6)
#define WPILIB_API_INDEX_TOF_STATUS (2 << 6)
#define WPILIB_API_INDEX_ENCODER_STATUS (3 << 6)

/* Commands for Control Class */
#define CAN_CMD_START 0x01
#define CAN_CMD_COMMIT 0x02
#define CAN_CMD_REBOOT 0x03

#define BOOTLOADER_START                                                       \
  (WPILIB_DEVICE_TYPE | WPILIB_MFG_CODE | WPILIB_API_CLASS_CONTROL)
#define BOOTLOADER_COMMIT BOOTLOADER_START
#define BOOTLOADER_REBOOT BOOTLOADER_START

#define BACK_PORCH_SW_VERSION                                                  \
  (WPILIB_DEVICE_TYPE | WPILIB_MFG_CODE | WPILIB_API_CLASS_STATUS |            \
   WPILIB_API_INDEX_SW_VERSION)
#define BACK_PORCH_GENERAL_STATUS                                              \
  (WPILIB_DEVICE_TYPE | WPILIB_MFG_CODE | WPILIB_API_CLASS_STATUS |            \
   WPILIB_API_INDEX_GENERAL_STATUS)
#define BACK_PORCH_TOF_STATUS                                                  \
  (WPILIB_DEVICE_TYPE | WPILIB_MFG_CODE | WPILIB_API_CLASS_STATUS |            \
   WPILIB_API_INDEX_TOF_STATUS)
#define BACK_PORCH_ENCODER_STATUS                                              \
  (WPILIB_DEVICE_TYPE | WPILIB_MFG_CODE | WPILIB_API_CLASS_STATUS |            \
   WPILIB_API_INDEX_ENCODER_STATUS)

#define WPILIB_HEARTBEAT_ID 0x01011840

/*
 Version message data format:
    Byte 0-3: Unique ID (MurmurHash3 on the device's serial number, little
 endian) Byte 4: Bits 0: Bootblock (0) or application (1) Bits 3:1: Major
 version Bits 7:4: Minor version Byte 5-7: Unique build number (little endian)

 General Status message data format:
    Byte 0-3: Unique ID (MurmurHash3 on the device's serial number, little
 endian) Byte 4: Current in mA (0-255mA) Byte 5-6: Input Voltage in mV
 (0-65535mV, little endian) Byte 7: Temperature in degrees Celsius

 TOF message data format:
    Byte 0: API Status from ST TOF
    Byte 1: Reserved
    Byte 2-3: Distance in mm (little endian)
    Byte 4-5: ambient Mcps (little endian)
    Byte 6-7: signal Mcps (little endian)

 Through Bore Encoder status message data format:
    Byte 0-1: Encoder 1 Absolute position in 0.01 degrees (little endian)
    Byte 2-3: Encoder 1 Incremental position in 0.01 degrees (little endian)
    Byte 4-5: Encoder 2 Absolute position in 0.01 degrees (little endian)
    Byte 6-7: Encoder 2 Incremental position in 0.01 degrees (little endian)
 */

/* --- Bootloader / Application Shared Configuration --- */

#define BOOT_CONFIG_MAGIC 0xCAFEBABE
#define BOOT_CONFIG_ADDR 0x08007800 // Page 15 (reserved for boot config)
#define BOOT_CONFIG_PAGE 15

/* Magic Words for Bootloader Entry */
#define MAGIC_WORD_ADDR ((uint32_t *)0x20023C00)
#define MAGIC_WORD_PROGRAM_FW                                                  \
  0xDEADBEEF /* Program application/bootblock firmware */
#define MAGIC_WORD_PROGRAM_CONFIG                                              \
  0xBEEFCAFE /* Program BootConfig_t structure from RAM */

typedef struct {
  uint32_t magic; // BOOT_CONFIG_MAGIC
  uint32_t crc;   // CRC32 of appSize through reserved[] (using STM CRC engine)
  uint32_t appSize;
  uint32_t appCrc;
  uint32_t deviceID;
  uint8_t reserved[236]; // Reserved for future use
} BootConfig_t;

/* RAM location for BootConfig_t when programming config */
#define RAM_CONFIG_ADDR                                                        \
  ((BootConfig_t *)MAGIC_WORD_ADDR + 1) /* Immediately after magic word */

/* Size verification - BootConfig_t should be exactly 1024 bytes */
_Static_assert(sizeof(BootConfig_t) == 256, "BootConfig_t must be 256 bytes");

#endif /* COMMON_H_ */
