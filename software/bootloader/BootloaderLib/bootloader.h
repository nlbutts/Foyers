#ifndef __BOOTLOADER_H
#define __BOOTLOADER_H

#include "main.h"
#include <stdint.h>

#include "bp_can_api.h"

/* WPILib CAN Addressing Constants */
#define WPILIB_DEVICE_ID (0) // Default Device ID

/* Magic Word for Bootloader Entry */
#define HEAD_MAGIC_WORD 0xDEADBEEF
#define MAGIC_WORD_ADDR ((uint32_t *)&_magic_word_start)

/* Memory Map Constants */
#define APP_START_ADDR 0x08008000
#define APP_SIZE_MAX (96 * 1024)
#define RAM_BUFFER_ADDR 0x2000C000

/* Boot Config Storage (Last Page of Bootloader Flash: 0x08007800) */
#define BOOT_CONFIG_ADDR 0x08007800

typedef struct {
  uint32_t appSize;
  uint32_t appCrc;
  uint32_t magic; // 0xCAFEBABE to indicate valid config
} BootConfig_t;

/* Bootloader State Machine */
typedef enum {
  BOOT_STATE_IDLE,
  BOOT_STATE_RECEIVING,
  BOOT_STATE_VERIFYING,
  BOOT_STATE_FLASHING,
  BOOT_STATE_ERROR
} BootloaderState_t;

/* Public Function Prototypes */
void Bootloader_Init(void);
void Bootloader_Loop(void);
void Bootloader_RxCallback(void);
void Bootloader_CheckAndJump(void);

#endif /* __BOOTLOADER_H */
