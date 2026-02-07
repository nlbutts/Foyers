#include "bootloader.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include "version.h"

/* Version Defines */
#define MAJOR_VERSION 1
#define MINOR_VERSION 0

/* Extern handles from main.c */
extern FDCAN_HandleTypeDef hfdcan1;
extern CRC_HandleTypeDef hcrc;
extern UART_HandleTypeDef huart5;
extern IWDG_HandleTypeDef hiwdg;
//extern uint32_t _magic_word_start; // Symbol from linker script

/* Global State */
static BootloaderState_t currentState = BOOT_STATE_IDLE;
static uint32_t bytesReceived = 0;
static uint32_t expectedCrc = 0;
static uint32_t lastDeviceID = 0;

/* RAM Buffer Pointer - pointing to the reserved section */
static uint8_t* pRamBuffer = (uint8_t*)RAM_BUFFER_ADDR;

/* Private Prototypes */
static void ProcessControlPacket(uint8_t* data, uint32_t devId);
static void ProcessDataPacket(uint8_t* data, uint32_t len);
static void VerifyAndFlash(void);
static void SendStatus(uint8_t status, uint32_t data);

__attribute__((section(".RamFunc"), noinline))
static void FlashBootloaderFromRAM(uint8_t* src, uint32_t size) {
    // Disable interrupts to ensure atomicity and avoid vector table access during erase
    __disable_irq();

    // 1. Unlock Flash
    if (FLASH->CR & FLASH_CR_LOCK) {
        FLASH->KEYR = 0x45670123;
        FLASH->KEYR = 0xCDEF89AB;
    }

    // 2. Erase pages 0 to 14 (30KB bootloader area)
    for (uint32_t page = 0; page <= 14; page++) {
        // Clear error flags
        FLASH->SR = FLASH_SR_EOP | FLASH_SR_OPERR | FLASH_SR_WRPERR | FLASH_SR_PGAERR | FLASH_SR_PGSERR | FLASH_SR_MISERR | FLASH_SR_FASTERR;

        // Set page erase
        FLASH->CR = FLASH_CR_PER | (page << FLASH_CR_PNB_Pos);
        FLASH->CR |= FLASH_CR_STRT;

        // Wait for busy (using BSY1 for G0B1)
        while (FLASH->SR & FLASH_SR_BSY1);
        
        // Feed IWDG
        IWDG->KR = 0xAAAA;
    }

    // 3. Program the new bootloader (8 bytes at a time)
    for (uint32_t i = 0; i < size; i += 8) {
        uint32_t addr = 0x08000000 + i;
        uint32_t data_low = *(uint32_t*)(src + i);
        uint32_t data_high = *(uint32_t*)(src + i + 4);

        // Clear error flags
        FLASH->SR = FLASH_SR_EOP | FLASH_SR_OPERR | FLASH_SR_WRPERR | FLASH_SR_PGAERR | FLASH_SR_PGSERR | FLASH_SR_MISERR | FLASH_SR_FASTERR;

        // Set programming mode
        FLASH->CR = FLASH_CR_PG;

        // Write first word
        *(__IO uint32_t*)addr = data_low;
        // Write second word
        *(__IO uint32_t*)(addr + 4) = data_high;

        // Wait for busy
        while (FLASH->SR & FLASH_SR_BSY1);

        // Feed IWDG
        IWDG->KR = 0xAAAA;
    }

    // 4. Lock Flash
    FLASH->CR |= FLASH_CR_LOCK;

    // 5. System Reset
    SCB->AIRCR = ((0x5FAUL << SCB_AIRCR_VECTKEY_Pos) |
                 SCB_AIRCR_SYSRESETREQ_Msk);

    while (1);
}

/* API Command Definitions (API Class 1) */
#define CMD_START       0x01 // Payload: [Reserved]
#define CMD_COMMIT      0x02 // Payload: [CRC32 (4 bytes)]
#define CMD_REBOOT      0x03 // Payload: None

/* Logging Helper */
static void Log(const char* format, ...) {
    char buf[128];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    if (len > 0) {
        if (len >= (int)sizeof(buf)) {
            len = (int)sizeof(buf) - 1;
        }
        HAL_UART_Transmit(&huart5, (uint8_t*)buf, len, 100);
        HAL_UART_Transmit(&huart5, (uint8_t*)"\r\n", 2, 10);
    }
}

/* --- Implementation --- */

void Bootloader_Init(void) {
    Log("Bootloader Init...");
    FDCAN_FilterTypeDef sFilterConfig;

    /* Configure Rx Filter to accept broadcast-ish or specific messages */
    /* WPILIB Mask: Match Device Type, Mfg Code, and Device ID */
    
    sFilterConfig.IdType = FDCAN_EXTENDED_ID;
    sFilterConfig.FilterIndex = 0;
    sFilterConfig.FilterType = FDCAN_FILTER_MASK;
    sFilterConfig.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    sFilterConfig.FilterID1 = WPILIB_DEVICE_TYPE | WPILIB_MFG_CODE;
    sFilterConfig.FilterID2 = 0x1FFF0000; // Mask: Check DevType and Mfg. Ignore API and Device ID.
    
    if (HAL_FDCAN_ConfigFilter(&hfdcan1, &sFilterConfig) != HAL_OK) {
        Log("Filter Config Error!");
        Error_Handler();
    }

    if (HAL_FDCAN_Start(&hfdcan1) != HAL_OK) {
        Log("FDCAN Start Error!");
        Error_Handler();
    }

    if (HAL_FDCAN_ActivateNotification(&hfdcan1, FDCAN_IT_RX_FIFO0_NEW_MESSAGE, 0) != HAL_OK) {
        Log("Notification Error!");
        Error_Handler();
    }
    Log("Bootloader Ready.");
}

static uint32_t lastVersionSendTime = 0;

/* MurmurHash3 from application */
static uint32_t murmur3_32(const uint8_t* key, size_t len, uint32_t seed) {
    uint32_t h = seed;
    uint32_t k;
    const uint32_t* data = (const uint32_t*)key;
    const size_t nblocks = len / 4;

    for (size_t i = 0; i < nblocks; i++) {
        k = data[i];
        k *= 0xcc9e2d51;
        k = (k << 15) | (k >> 17);
        k *= 0x1b873593;
        h ^= k;
        h = (h << 13) | (h >> 19);
        h = h * 5 + 0xe6546b64;
    }

    const uint8_t* tail = (const uint8_t*)(key + nblocks * 4);
    k = 0;
    switch (len & 3) {
        case 3: k ^= tail[2] << 16;
        case 2: k ^= tail[1] << 8;
        case 1: k ^= tail[0];
                k *= 0xcc9e2d51;
                k = (k << 15) | (k >> 17);
                k *= 0x1b873593;
                h ^= k;
    }

    h ^= len;
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}

static void SendVersion(void) {
    FDCAN_TxHeaderTypeDef TxHeader;
    uint8_t TxData[8];
    uint32_t uid[3];
    
    uid[0] = HAL_GetUIDw0();
    uid[1] = HAL_GetUIDw1();
    uid[2] = HAL_GetUIDw2();
    
    uint32_t hash = murmur3_32((uint8_t*)uid, 12, 0);
    
    /* Construct Message */
    /* Byte 0-3: Hash */
    memcpy(&TxData[0], &hash, 4);
    
    /* Byte 4: Version Info 
       Bit 0: 0 (Bootloader)
       Bits 3:1: Major
       Bits 7:4: Minor
    */
    TxData[4] = 0 | (MAJOR_VERSION << 1) | (MINOR_VERSION << 4);
    
    /* Byte 5-7: Build Number */
    uint32_t build = BUILD_NUMBER;
    TxData[5] = build & 0xFF;
    TxData[6] = (build >> 8) & 0xFF;
    TxData[7] = (build >> 16) & 0xFF;

    TxHeader.Identifier = WPILIB_DEVICE_TYPE | WPILIB_MFG_CODE | WPILIB_API_CLASS_STATUS | WPILIB_API_INDEX_SW_VERSION | 0; // Device ID 0
    TxHeader.IdType = FDCAN_EXTENDED_ID;
    TxHeader.TxFrameType = FDCAN_DATA_FRAME;
    TxHeader.DataLength = FDCAN_DLC_BYTES_8;
    TxHeader.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
    TxHeader.BitRateSwitch = FDCAN_BRS_OFF;
    TxHeader.FDFormat = FDCAN_CLASSIC_CAN;
    TxHeader.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
    TxHeader.MessageMarker = 0;
    
    HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan1, &TxHeader, TxData);
}

void Bootloader_Loop(void) {
    HAL_IWDG_Refresh(&hiwdg);
    
    uint32_t now = HAL_GetTick();
    if (now - lastVersionSendTime >= 100) {
        lastVersionSendTime = now;
        SendVersion();
        //Log("Running bootloader...");
    }

    /* State Machine Housekeeping */
    switch (currentState) {
        case BOOT_STATE_VERIFYING:
             VerifyAndFlash();
             break;
        case BOOT_STATE_COMPLETE:
             Log("Update Complete. Resetting...");
             HAL_Delay(200); // Wait for logs and CAN message to clear
             HAL_NVIC_SystemReset();
             break;
        default:
            break;
    }
}

void Bootloader_RxCallback(void) {
    FDCAN_RxHeaderTypeDef RxHeader;
    uint8_t RxData[64]; // Increased to 64 to safely handle FDCAN frames

    if (HAL_FDCAN_GetRxMessage(&hfdcan1, FDCAN_RX_FIFO0, &RxHeader, RxData) != HAL_OK) {
        return;
    }

    // Convert DLC to actual byte length
    uint32_t len = 0;
    if (RxHeader.DataLength <= 8) {
        len = RxHeader.DataLength;
    } else {
        // FDCAN DLC lookup
        static const uint8_t dlcToLen[] = {0,1,2,3,4,5,6,7,8,12,16,20,24,32,48,64};
        if (RxHeader.DataLength <= 15) {
            len = dlcToLen[RxHeader.DataLength];
        }
    }

    // Extract Device ID from message (lower 6 bits) - stored only in CMD_START
    uint32_t rxDeviceID = RxHeader.Identifier & 0x3F;

    // Extract API Class from ID (Bits 15-10)
    uint32_t apiClass = (RxHeader.Identifier >> 10) & 0x3F;

    if (apiClass == 1) {
        Log("Processed Control Packet CMD=0x%02X", RxData[0]);
        ProcessControlPacket(RxData, rxDeviceID);
    } else if (apiClass == 2) {
        ProcessDataPacket(RxData, len);
    }
}

static void ProcessControlPacket(uint8_t* data, uint32_t devId) {
    uint8_t cmd = data[0];
    Log("Control Packet: CMD=0x%02X", cmd);
    
    switch (cmd) {
        case CMD_START:
            Log("CMD_START received. Resetting buffer.");
            currentState = BOOT_STATE_RECEIVING;
            bytesReceived = 0;
            lastDeviceID = devId; // Capture device ID only on CMD_START
            memset(pRamBuffer, 0, APP_SIZE_MAX); // Clear buffer for clean programming
            SendStatus(0x00, 0); // OK
            break;

        case CMD_COMMIT:
            if (currentState == BOOT_STATE_RECEIVING) {
                memcpy(&expectedCrc, &data[1], 4);
                Log("CMD_COMMIT: Expected CRC=0x%08X, Size=%lu", expectedCrc, bytesReceived);
                currentState = BOOT_STATE_VERIFYING; // Trigger processing in Bootloader_Loop
            } else {
                Log("CMD_COMMIT ignored: not in RECEIVING state");
            }
            break;
            
        case CMD_REBOOT:
             Log("CMD_REBOOT: System Reset");
             HAL_Delay(100);
             NVIC_SystemReset();
             break;
    }
}

static void ProcessDataPacket(uint8_t* data, uint32_t len) {
    if (currentState != BOOT_STATE_RECEIVING) return;
    
    if (bytesReceived + len > APP_SIZE_MAX) {
        Log("Buffer Overflow: %lu + %lu > %d", bytesReceived, len, APP_SIZE_MAX);
        currentState = BOOT_STATE_ERROR;
        SendStatus(0xFF, bytesReceived); // Overflow
        return;
    }

    memcpy(pRamBuffer + bytesReceived, data, len);
    bytesReceived += len;
}

static void VerifyAndFlash(void) {
    Log("Verifying RAM Buffer...");
    uint32_t calculatedCrc = HAL_CRC_Calculate(&hcrc, (uint32_t*)pRamBuffer, bytesReceived / 4); 
    Log("Calculated CRC: 0x%08X", calculatedCrc);

    if (calculatedCrc == expectedCrc) {
        Log("CRC Match! Checking image type...");
        
        // Inspect Vector Table: Word 0 is SP, Word 1 is Reset Vector
        uint32_t resetVector = *(uint32_t*)(pRamBuffer + 4);
        
        // Bootloader area is 0x08000000 - 0x080077FF (Pages 0-14)
        if (resetVector >= 0x08000000 && resetVector < 0x08007800) {
            Log("Detected Bootloader Image (Reset Vector: 0x%08X)", resetVector);
            Log("Jumping to RAM for self-reprogramming...");
            SendStatus(0x02, 0); // Status 0x02: Self-updating
            HAL_Delay(100);
            FlashBootloaderFromRAM(pRamBuffer, bytesReceived);
            // Should not return
        }

        Log("Detected Application Image. Starting Flash Erase...");
        currentState = BOOT_STATE_FLASHING;
        
        FLASH_EraseInitTypeDef EraseInitStruct;
        uint32_t PageError;
        
        HAL_FLASH_Unlock();
        /* Clear flash error flags */
        __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_OPERR | FLASH_FLAG_PROGERR | FLASH_FLAG_WRPERR | 
                               FLASH_FLAG_PGAERR | FLASH_FLAG_SIZERR | FLASH_FLAG_PGSERR | 
                               FLASH_FLAG_MISERR | FLASH_FLAG_FASTERR | FLASH_FLAG_RDERR | 
                               FLASH_FLAG_OPTVERR);
        
        EraseInitStruct.TypeErase = FLASH_TYPEERASE_PAGES;
        EraseInitStruct.Banks = FLASH_BANK_1;
        EraseInitStruct.Page = 16;
        EraseInitStruct.NbPages = 48; // 96KB
        
        // Erase is slow, feed IWDG frequently
        HAL_IWDG_Refresh(&hiwdg);
        HAL_StatusTypeDef status;
        status = HAL_FLASHEx_Erase(&EraseInitStruct, &PageError);
        if (status != HAL_OK) {
             Log("Erase Error at page %lu status: %d", PageError, status);
             currentState = BOOT_STATE_ERROR;
             HAL_FLASH_Lock();
             return;
        }
        HAL_IWDG_Refresh(&hiwdg);

        Log("Programming %lu bytes to Flash...", bytesReceived);
        for (uint32_t i = 0; i < bytesReceived; i += 8) {
            HAL_IWDG_Refresh(&hiwdg); // Feed WDT during programming
            uint64_t data = *((uint64_t*)(pRamBuffer + i));
            if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, APP_START_ADDR + i, data) != HAL_OK) {
                Log("Program Error at addr 0x%08lX", APP_START_ADDR + i);
                currentState = BOOT_STATE_ERROR;
                HAL_FLASH_Lock();
                return;
            }
        }
        
        /* Preserve existing BootConfig_t and update app-related fields */
        BootConfig_t newConfig __attribute__((aligned(8)));
        BootConfig_t* oldConfig = (BootConfig_t*)BOOT_CONFIG_ADDR;
        
        /* Start with zeroed config */
        memset(&newConfig, 0, sizeof(BootConfig_t));
        
        /* Preserve deviceID and reserved data if old config was valid */
        if (oldConfig->magic == BOOT_CONFIG_MAGIC) {
            newConfig.deviceID = oldConfig->deviceID;
            memcpy(newConfig.reserved, oldConfig->reserved, sizeof(newConfig.reserved));
        } else {
            /* Fallback: use ID from the last received CAN frame or default */
            newConfig.deviceID = lastDeviceID;
        }
        
        /* Set application info */
        newConfig.magic = BOOT_CONFIG_MAGIC;
        newConfig.appSize = bytesReceived;
        newConfig.appCrc = expectedCrc;
        
        /* Calculate CRC over appSize through reserved[] (exclude magic and crc fields) */
        newConfig.crc = HAL_CRC_Calculate(&hcrc, (uint32_t*)&newConfig.appSize, 
                                          (sizeof(BootConfig_t) - offsetof(BootConfig_t, appSize)) / 4);

        Log("Updating Boot Config (CRC=0x%08X)...", newConfig.crc);
        EraseInitStruct.Page = BOOT_CONFIG_PAGE;
        EraseInitStruct.NbPages = 1;
        
        if (HAL_FLASHEx_Erase(&EraseInitStruct, &PageError) != HAL_OK) {
             Log("Config Erase Error at page %lu", PageError);
             currentState = BOOT_STATE_ERROR;
             HAL_FLASH_Lock();
             return;
        }
        
        /* Program entire BootConfig_t structure (1024 bytes) */
        uint64_t *ptr = (uint64_t *)&newConfig;
        for (uint32_t i = 0; i < sizeof(BootConfig_t); i += 8) {
            HAL_IWDG_Refresh(&hiwdg);
            if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, BOOT_CONFIG_ADDR + i, ptr[i/8]) != HAL_OK) {
                Log("Config Program Error at offset %lu", i);
                currentState = BOOT_STATE_ERROR;
                HAL_FLASH_Lock();
                return;
            }
        }
        
        HAL_FLASH_Lock();
        SendStatus(0x01, 0); // Success message
        currentState = BOOT_STATE_COMPLETE; // Trigger reset in Bootloader_Loop
    } else {
        Log("CRC MISMATCH! Expected 0x%08X", expectedCrc);
        currentState = BOOT_STATE_ERROR;
        SendStatus(0xEE, calculatedCrc); // CRC Fail
    }
}

static void SendStatus(uint8_t status, uint32_t data) {
    FDCAN_TxHeaderTypeDef TxHeader;
    uint8_t TxData[8];
    
    TxHeader.Identifier = BOOTLOADER_START | lastDeviceID; 
    TxHeader.IdType = FDCAN_EXTENDED_ID;
    TxHeader.TxFrameType = FDCAN_DATA_FRAME;
    TxHeader.DataLength = FDCAN_DLC_BYTES_8;
    TxHeader.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
    TxHeader.BitRateSwitch = FDCAN_BRS_OFF;
    TxHeader.FDFormat = FDCAN_CLASSIC_CAN;
    TxHeader.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
    TxHeader.MessageMarker = 0;

    TxData[0] = 0xAA;
    TxData[1] = status;
    memcpy(&TxData[4], &data, 4);
    
    HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan1, &TxHeader, TxData);
}

/* Program BootConfig_t structure from RAM to flash */
static void ProgramConfigFromRAM(void) {
    Log("Programming BootConfig_t from RAM...");
    
    BootConfig_t* ramConfig = RAM_CONFIG_ADDR;
    BootConfig_t newConfig __attribute__((aligned(8)));
    
    /* Copy from RAM */
    memcpy(&newConfig, ramConfig, sizeof(BootConfig_t));

    Log("Config: Magic=0x%08X, DevID=%lu, AppSize=%lu, AppCRC=0x%08X, CRC=0x%08X",
        newConfig.magic, newConfig.deviceID, newConfig.appSize, newConfig.appCrc, newConfig.crc);
    
    /* Verify the magic word in the RAM config */
    if (newConfig.magic != BOOT_CONFIG_MAGIC) {
        Log("Invalid config magic in RAM: 0x%08X", newConfig.magic);
        return;
    }
    
    /* Recalculate CRC to ensure integrity */
    uint32_t calculatedCrc = HAL_CRC_Calculate(&hcrc, (uint32_t*)&newConfig.appSize,
                                                (sizeof(BootConfig_t) - offsetof(BootConfig_t, appSize)) / 4);
    
    if (calculatedCrc != newConfig.crc) {
        Log("Config CRC mismatch: calc=0x%08X, stored=0x%08X", calculatedCrc, newConfig.crc);
        return;
    }
    
    /* Erase config page */
    FLASH_EraseInitTypeDef EraseInitStruct;
    uint32_t PageError;
    
    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_OPERR | FLASH_FLAG_PROGERR | FLASH_FLAG_WRPERR | 
                           FLASH_FLAG_PGAERR | FLASH_FLAG_SIZERR | FLASH_FLAG_PGSERR | 
                           FLASH_FLAG_MISERR | FLASH_FLAG_FASTERR | FLASH_FLAG_RDERR | 
                           FLASH_FLAG_OPTVERR);
    
    EraseInitStruct.TypeErase = FLASH_TYPEERASE_PAGES;
    EraseInitStruct.Banks = FLASH_BANK_1;
    EraseInitStruct.Page = BOOT_CONFIG_PAGE;
    EraseInitStruct.NbPages = 1;
    
    HAL_IWDG_Refresh(&hiwdg);
    if (HAL_FLASHEx_Erase(&EraseInitStruct, &PageError) != HAL_OK) {
        Log("Config Erase Error at page %lu", PageError);
        HAL_FLASH_Lock();
        return;
    }
    
    /* Program entire BootConfig_t structure */
    uint64_t *ptr = (uint64_t *)&newConfig;
    for (uint32_t i = 0; i < sizeof(BootConfig_t); i += 8) {
        HAL_IWDG_Refresh(&hiwdg);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, BOOT_CONFIG_ADDR + i, ptr[i/8]) != HAL_OK) {
            Log("Config Program Error at offset %lu", i);
            HAL_FLASH_Lock();
            return;
        }
    }
    
    HAL_FLASH_Lock();
    Log("BootConfig_t programmed successfully. Resetting...");
    HAL_Delay(100);
    HAL_NVIC_SystemReset();
}

void Bootloader_CheckAndJump(void) {
    /* Check if Watchdog triggered the reset */
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST)) {
        Log("Watchdog Reset detected! Staying in Bootloader.");
        __HAL_RCC_CLEAR_RESET_FLAGS();
        return;
    }
    
    /* Clear reset flags for clean state if normal boot */
    __HAL_RCC_CLEAR_RESET_FLAGS();

    uint32_t magic = *MAGIC_WORD_ADDR;
    
    if (magic == MAGIC_WORD_PROGRAM_FW) {
        Log("Firmware Program Magic detected. Staying in bootloader.");
        *MAGIC_WORD_ADDR = 0; // Clear magic word so next boot is normal
        return; 
    }
    
    if (magic == MAGIC_WORD_PROGRAM_CONFIG) {
        Log("Config Program Magic detected. Programming config from RAM.");
        *MAGIC_WORD_ADDR = 0; // Clear magic word
        BootConfig_t* ramConfig = RAM_CONFIG_ADDR;
        Log("RAM Config: Magic=0x%08X, DevID=%lu, AppSize=%lu, AppCRC=0x%08X, CRC=0x%08X",
            ramConfig->magic, ramConfig->deviceID, ramConfig->appSize, ramConfig->appCrc, ramConfig->crc);
        ProgramConfigFromRAM();
        return; // ProgramConfigFromRAM will reset on success
    }
    
    BootConfig_t* pConfig = (BootConfig_t*)BOOT_CONFIG_ADDR;
    
    if (pConfig->magic == BOOT_CONFIG_MAGIC) {
        /* First verify BootConfig_t CRC */
        uint32_t configCrc = HAL_CRC_Calculate(&hcrc, (uint32_t*)&pConfig->appSize,
                                               (sizeof(BootConfig_t) - offsetof(BootConfig_t, appSize)) / 4);
        
        if (configCrc != pConfig->crc) {
            Log("BootConfig CRC fail: 0x%08X vs 0x%08X", configCrc, pConfig->crc);
            return;
        }
        
        Log("Valid config found. Verifying App...");
        uint32_t flashCrc = HAL_CRC_Calculate(&hcrc, (uint32_t*)APP_START_ADDR, pConfig->appSize / 4);
        
        if (flashCrc == pConfig->appCrc) {
            Log("App verified. Jumping to 0x%08X...", APP_START_ADDR);
            HAL_IWDG_Refresh(&hiwdg);
            HAL_RCC_DeInit();
            HAL_DeInit();
            
            SysTick->CTRL = 0;
            SysTick->LOAD = 0;
            SysTick->VAL = 0;
            
            void (*pJumpToApplication)(void);
            uint32_t JumpAddress = *(volatile uint32_t*)(APP_START_ADDR + 4);
            pJumpToApplication = (void (*)(void))JumpAddress;
            
            __set_MSP(*(volatile uint32_t*)APP_START_ADDR);
            pJumpToApplication();
        } else {
            Log("App CRC fail: 0x%08X vs 0x%08X", flashCrc, pConfig->appCrc);
        }
    } else {
        Log("No valid app config found.");
    }
}
