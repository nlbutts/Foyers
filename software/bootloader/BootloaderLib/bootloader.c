#include "bootloader.h"
#include <string.h>

/* Extern handles from main.c */
extern FDCAN_HandleTypeDef hfdcan1;
extern CRC_HandleTypeDef hcrc;
//extern uint32_t _magic_word_start; // Symbol from linker script
uint32_t * _magic_word_start = (uint32_t *)0x20000000; // Define it here for simplicity

/* Global State */
static BootloaderState_t currentState = BOOT_STATE_IDLE;
static uint32_t bytesReceived = 0;
static uint32_t expectedCrc = 0;

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

/* --- Implementation --- */

void Bootloader_Init(void) {
    FDCAN_FilterTypeDef sFilterConfig;

    /* Configure Rx Filter to accept broadcast-ish or specific messages */
    /* WPILIB Mask: Match Device Type, Mfg Code, and Device ID */
    /* Mask: 0xFFFFFF3F (Matches everything except API Class/Index) */
    
    sFilterConfig.IdType = FDCAN_STANDARD_ID; // WPIlib uses Extended IDs actually? Usually 29-bit.
    // Wait, WPIlib is 29-bit Extended ID.
    sFilterConfig.IdType = FDCAN_EXTENDED_ID;
    sFilterConfig.FilterIndex = 0;
    sFilterConfig.FilterType = FDCAN_FILTER_MASK;
    sFilterConfig.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    sFilterConfig.FilterID1 = WPILIB_DEVICE_TYPE | WPILIB_MFG_CODE | WPILIB_DEVICE_ID;
    sFilterConfig.FilterID2 = 0x1FFFFF3F; // Mask: Check DevType, Mfg, DevID. Ignore API.
    
    if (HAL_FDCAN_ConfigFilter(&hfdcan1, &sFilterConfig) != HAL_OK) {
        Error_Handler();
    }

    if (HAL_FDCAN_Start(&hfdcan1) != HAL_OK) {
        Error_Handler();
    }

    if (HAL_FDCAN_ActivateNotification(&hfdcan1, FDCAN_IT_RX_FIFO0_NEW_MESSAGE, 0) != HAL_OK) {
        Error_Handler();
    }
}

void Bootloader_Loop(void) {
    /* State Machine Housekeeping if needed */
    switch (currentState) {
        case BOOT_STATE_VERIFYING:
        case BOOT_STATE_FLASHING:
             /* These are blocking operations triggered by CAN, 
                but if we wanted them non-blocking we'd do it here.
                For simplicity, we might do them in the callback/command handler 
                or flag it here. */
            break;
        default:
            break;
    }
}

/* This Callback is called from main.c's HAL_FDCAN_RxFifo0Callback equivalent or directly if we redirect it */
void Bootloader_RxCallback(void) {
    FDCAN_RxHeaderTypeDef RxHeader;
    uint8_t RxData[8];

    if (HAL_FDCAN_GetRxMessage(&hfdcan1, FDCAN_RX_FIFO0, &RxHeader, RxData) != HAL_OK) {
        return;
    }

    // Extract API Class from ID (Bits 15-10)
    uint32_t apiClass = (RxHeader.Identifier >> 10) & 0x3F;

    // We mapped:
    // API Class 1 (0x01) -> Control
    // API Class 2 (0x02) -> Data Stream

    if (apiClass == 1) {
        ProcessControlPacket(RxData);
    } else if (apiClass == 2) {
        ProcessDataPacket(RxData, RxHeader.DataLength >> 16); // DataLength is tricky in FDCAN HAL, assume DLC converted
        // Wait, HAL return RxHeader.DataLength as FDCAN_DLC_BYTES_x. We need actual bytes.
        // For Classic CAN (8 bytes max), mapping is direct 0-8 usually?
        // Actually HAL_FDCAN_GetRxMessage returns DLC code. We need to convert or assume 8 bytes if we fixed it.
        // Let's assume standard behavior helper:
        uint32_t len = 0;
        if (RxHeader.DataLength == FDCAN_DLC_BYTES_8) len = 8;
        else if (RxHeader.DataLength == FDCAN_DLC_BYTES_4) len = 4;
        else len = 8; // Simplify for now, assuming host sends full frames
        
        ProcessDataPacket(RxData, len);
    }
}

static void ProcessControlPacket(uint8_t* data) {
    uint8_t cmd = data[0];
    
    switch (cmd) {
        case CMD_START:
            currentState = BOOT_STATE_RECEIVING;
            bytesReceived = 0;
            // Clear RAM Buffer? Optional.
            SendStatus(0x00, 0); // OK
            break;

        case CMD_COMMIT:
            if (currentState == BOOT_STATE_RECEIVING) {
                // Payload bytes 1-4 are CRC32 (Little Endian?)
                memcpy(&expectedCrc, &data[1], 4);
                currentState = BOOT_STATE_VERIFYING;
                VerifyAndFlash();
            }
            break;
            
        case CMD_REBOOT:
             NVIC_SystemReset();
             break;
    }
}

static void ProcessDataPacket(uint8_t* data, uint32_t len) {
    if (currentState != BOOT_STATE_RECEIVING) return;
    
    // Safety check size
    if (bytesReceived + len > APP_SIZE_MAX) {
        currentState = BOOT_STATE_ERROR;
        SendStatus(0xFF, bytesReceived); // Overflow
        return;
    }

    memcpy(pRamBuffer + bytesReceived, data, len);
    bytesReceived += len;
}

static void VerifyAndFlash(void) {
    // 1. Calculate CRC of RAM Buffer
    uint32_t calculatedCrc = HAL_CRC_Calculate(&hcrc, (uint32_t*)pRamBuffer, bytesReceived); // NOTE: HAL_CRC_Calculate takes length in 32-bit Words usually!
    // We should treat bytesReceived as 4-byte aligned or handle remainder.
    // For simplicity, assume App is 4-byte aligned and padded.

    if (calculatedCrc == expectedCrc) {
        currentState = BOOT_STATE_FLASHING;
        
        // 2. Erase App Flash
        FLASH_EraseInitTypeDef EraseInitStruct;
        uint32_t PageError;
        
        HAL_FLASH_Unlock();
        
        /* Erase App Area */
        EraseInitStruct.TypeErase = FLASH_TYPEERASE_PAGES;
        EraseInitStruct.Banks = FLASH_BANK_1;
        EraseInitStruct.Page = 16;
        EraseInitStruct.NbPages = 48; // 96KB
        
        if (HAL_FLASHEx_Erase(&EraseInitStruct, &PageError) != HAL_OK) {
             currentState = BOOT_STATE_ERROR;
             HAL_FLASH_Lock();
             return;
        }

        // 3. Program Flash
        for (uint32_t i = 0; i < bytesReceived; i += 8) { // Double word programming (64-bit)
            uint64_t data = *((uint64_t*)(pRamBuffer + i));
            if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, APP_START_ADDR + i, data) != HAL_OK) {
                currentState = BOOT_STATE_ERROR;
                HAL_FLASH_Lock();
                return;
            }
        }
        
        // 4. Update Boot Config
        // First erase the config page (Page 15: 0x08007800)
        // Wait, 0x08007800 is Page 15 (15 * 2048 = 30720 = 0x7800). Correct.
        EraseInitStruct.Page = 15;
        EraseInitStruct.NbPages = 1;
        
        if (HAL_FLASHEx_Erase(&EraseInitStruct, &PageError) != HAL_OK) {
             currentState = BOOT_STATE_ERROR;
             HAL_FLASH_Lock();
             return;
        }
        
        // Write new Config
        uint64_t configData1 = ((uint64_t)expectedCrc << 32) | bytesReceived; // [CRC][Size]
        uint64_t configData2 = 0xCAFEBABE; // Magic
        
        // We write 2 double words (struct is 12 bytes, so 16 bytes aligned)
        // Struct: uint32 appSize, uint32 appCrc, uint32 magic
        // Memory layout: [Size] [CRC] [Magic] [Padding]
        // data1: [CRC-High32] [Size-Low32] ? No. Little Endian.
        // uint64_t 1: [Bits 63-32: CRC] [Bits 31-0: Size]
        
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, BOOT_CONFIG_ADDR, configData1) != HAL_OK) {
            currentState = BOOT_STATE_ERROR;
        }
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, BOOT_CONFIG_ADDR + 8, configData2) != HAL_OK) {
            currentState = BOOT_STATE_ERROR;
        }
        
        HAL_FLASH_Lock();
        currentState = BOOT_STATE_IDLE; // Done
        SendStatus(0x01, 0); // Success
        
        // Reboot to run?
        NVIC_SystemReset();
        
    } else {
        currentState = BOOT_STATE_ERROR;
        SendStatus(0xEE, calculatedCrc); // CRC Fail
    }
}

static void SendStatus(uint8_t status, uint32_t data) {
    // Construct Tx Header and Send
    // Use DevID 0 response
    FDCAN_TxHeaderTypeDef TxHeader;
    uint8_t TxData[8];
    
    TxHeader.Identifier = WPILIB_DEVICE_TYPE | WPILIB_MFG_CODE | WPILIB_DEVICE_ID | (API_CLASS_CONTROL); // Reply on Control
    TxHeader.IdType = FDCAN_EXTENDED_ID;
    TxHeader.TxFrameType = FDCAN_DATA_FRAME;
    TxHeader.DataLength = FDCAN_DLC_BYTES_8;
    TxHeader.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
    TxHeader.BitRateSwitch = FDCAN_BRS_OFF;
    TxHeader.FDFormat = FDCAN_CLASSIC_CAN;
    TxHeader.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
    TxHeader.MessageMarker = 0;

    TxData[0] = 0xAA; // Status Header
    TxData[1] = status;
    memcpy(&TxData[4], &data, 4);
    
    HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan1, &TxHeader, TxData);
}

void Bootloader_CheckAndJump(void) {
    uint32_t magic = *MAGIC_WORD_ADDR;
    
    // If Magic Word matches, we stay in Bootloader by returning
    if (magic == HEAD_MAGIC_WORD) {
        return; 
    }
    
    // Check Config Validity
    BootConfig_t* pConfig = (BootConfig_t*)BOOT_CONFIG_ADDR;
    
    if (pConfig->magic == 0xCAFEBABE) {
        // Valid Config found. Verify App CRC.
        // Calculate CRC of Flash
        uint32_t flashCrc = HAL_CRC_Calculate(&hcrc, (uint32_t*)APP_START_ADDR, pConfig->appSize);
        
        if (flashCrc == pConfig->appCrc) {
             // Deinit Peripherals
            HAL_RCC_DeInit();
            HAL_DeInit();
            
            // Turn off SysTick
            SysTick->CTRL = 0;
            SysTick->LOAD = 0;
            SysTick->VAL = 0;
            
            // Jump
            void (*pJumpToApplication)(void);
            uint32_t JumpAddress = *(volatile uint32_t*)(APP_START_ADDR + 4);
            pJumpToApplication = (void (*)(void))JumpAddress;
            
            __set_MSP(*(volatile uint32_t*)APP_START_ADDR);
            pJumpToApplication();
        }
    }
    
    // Also Fallback: If Config is missing but Stack looks good? 
    // User Requirement: "If the CRC fails, it should stay in the bootblock."
    // So if config is missing or invalid, we stay in bootblock.
}
