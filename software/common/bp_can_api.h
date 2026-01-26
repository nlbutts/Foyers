#ifndef BP_CAN_API_H_
#define BP_CAN_API_H_

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
#define WPILIB_API_CLASS_CONTROL (1 << 10)
#define WPILIB_API_CLASS_DATA (2 << 10)
#define WPILIB_API_CLASS_STATUS (5 << 10)

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

#endif /* BP_CAN_API_H_ */