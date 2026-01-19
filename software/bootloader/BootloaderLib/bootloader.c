#include "bootloader.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
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
uint32_t * _magic_word_start = (uint32_t *)0x20000000; // Define it here for simplicity

/* Global State */
static BootloaderState_t currentState = BOOT_STATE_IDLE;
static uint32_t bytesReceived = 0;
static uint32_t expectedCrc = 0;
static uint32_t lastDeviceID = 0;

/* RAM Buffer Pointer - pointing to the reserved section */
static uint8_t* pRamBuffer = (uint8_t*)RAM_BUFFER_ADDR;

/* Private Prototypes */
static void ProcessControlPacket(uint8_t* data);
static void ProcessDataPacket(uint8_t* data, uint32_t len);
static void VerifyAndFlash(void);
static void SendStatus(uint8_t status, uint32_t data);

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

/* API Status Class */
#define API_CLASS_STATUS (5 << 10)
#define API_INDEX_SW_VERSION (0 << 6)

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

    TxHeader.Identifier = WPILIB_DEVICE_TYPE | WPILIB_MFG_CODE | API_CLASS_STATUS | API_INDEX_SW_VERSION | 0; // Device ID 0
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
    }

    /* State Machine Housekeeping if needed */
    switch (currentState) {
        case BOOT_STATE_VERIFYING:
        case BOOT_STATE_FLASHING:
             break;
        default:
            break;
    }
}

void Bootloader_RxCallback(void) {
    FDCAN_RxHeaderTypeDef RxHeader;
    uint8_t RxData[8];

    if (HAL_FDCAN_GetRxMessage(&hfdcan1, FDCAN_RX_FIFO0, &RxHeader, RxData) != HAL_OK) {
        return;
    }

    // Save Device ID for response (lower 6 bits)
    lastDeviceID = RxHeader.Identifier & 0x3F;

    // Extract API Class from ID (Bits 15-10)
    uint32_t apiClass = (RxHeader.Identifier >> 10) & 0x3F;

    if (apiClass == 1) {
        ProcessControlPacket(RxData);
    } else if (apiClass == 2) {
        ProcessDataPacket(RxData, RxHeader.DataLength);
    }
}

static void ProcessControlPacket(uint8_t* data) {
    uint8_t cmd = data[0];
    Log("Control Packet: CMD=0x%02X", cmd);
    
    switch (cmd) {
        case CMD_START:
            Log("CMD_START received. Resetting buffer.");
            currentState = BOOT_STATE_RECEIVING;
            bytesReceived = 0;
            SendStatus(0x00, 0); // OK
            break;

        case CMD_COMMIT:
            if (currentState == BOOT_STATE_RECEIVING) {
                memcpy(&expectedCrc, &data[1], 4);
                Log("CMD_COMMIT: Expected CRC=0x%08X, Size=%lu", expectedCrc, bytesReceived);
                currentState = BOOT_STATE_VERIFYING;
                VerifyAndFlash();
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
        Log("CRC Match! Starting Flash Erase...");
        currentState = BOOT_STATE_FLASHING;
        
        FLASH_EraseInitTypeDef EraseInitStruct;
        uint32_t PageError;
        
        HAL_FLASH_Unlock();
        
        EraseInitStruct.TypeErase = FLASH_TYPEERASE_PAGES;
        EraseInitStruct.Banks = FLASH_BANK_1;
        EraseInitStruct.Page = 16;
        EraseInitStruct.NbPages = 48; // 96KB
        
        if (HAL_FLASHEx_Erase(&EraseInitStruct, &PageError) != HAL_OK) {
             Log("Erase Error at page %lu", PageError);
             currentState = BOOT_STATE_ERROR;
             HAL_FLASH_Lock();
             return;
        }

        Log("Programming %lu bytes to Flash...", bytesReceived);
        for (uint32_t i = 0; i < bytesReceived; i += 8) {
            uint64_t data = *((uint64_t*)(pRamBuffer + i));
            if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, APP_START_ADDR + i, data) != HAL_OK) {
                Log("Program Error at addr 0x%08lX", APP_START_ADDR + i);
                currentState = BOOT_STATE_ERROR;
                HAL_FLASH_Lock();
                return;
            }
        }
        
        Log("Updating Boot Config...");
        EraseInitStruct.Page = 15;
        EraseInitStruct.NbPages = 1;
        
        if (HAL_FLASHEx_Erase(&EraseInitStruct, &PageError) != HAL_OK) {
             Log("Config Erase Error at page %lu", PageError);
             currentState = BOOT_STATE_ERROR;
             HAL_FLASH_Lock();
             return;
        }
        
        uint64_t configData1 = ((uint64_t)expectedCrc << 32) | bytesReceived;
        uint64_t configData2 = 0xCAFEBABE;
        
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, BOOT_CONFIG_ADDR, configData1) != HAL_OK) {
            Log("Config1 Program Error");
            currentState = BOOT_STATE_ERROR;
        }
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, BOOT_CONFIG_ADDR + 8, configData2) != HAL_OK) {
            Log("Config2 Program Error");
            currentState = BOOT_STATE_ERROR;
        }
        
        HAL_FLASH_Lock();
        Log("Update Complete. Resetting...");
        currentState = BOOT_STATE_IDLE;
        SendStatus(0x01, 0); // Success
        
        HAL_Delay(100);
        NVIC_SystemReset();
        
    } else {
        Log("CRC MISMATCH! Expected 0x%08X", expectedCrc);
        currentState = BOOT_STATE_ERROR;
        SendStatus(0xEE, calculatedCrc); // CRC Fail
    }
}

static void SendStatus(uint8_t status, uint32_t data) {
    FDCAN_TxHeaderTypeDef TxHeader;
    uint8_t TxData[8];
    
    TxHeader.Identifier = WPILIB_DEVICE_TYPE | WPILIB_MFG_CODE | lastDeviceID | (API_CLASS_CONTROL); 
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
    
    if (magic == HEAD_MAGIC_WORD) {
        Log("Magic Word detected. Staying in bootloader.");
        return; 
    }
    
    BootConfig_t* pConfig = (BootConfig_t*)BOOT_CONFIG_ADDR;
    
    if (pConfig->magic == 0xCAFEBABE) {
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
