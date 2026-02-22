/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "cmsis_os.h"
#include "usb_device.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "usbd_cdc_if.h"
#include <stdlib.h>
#include <string.h>
#include "stm32g0xx_ll_adc.h"
#include <stdio.h>
#include "common.h"
#include "version.h"
#include "FreeRTOS.h"
#include "task.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;

CRC_HandleTypeDef hcrc;

FDCAN_HandleTypeDef hfdcan1;

I2C_HandleTypeDef hi2c1;

IWDG_HandleTypeDef hiwdg;

TIM_HandleTypeDef htim1;
TIM_HandleTypeDef htim2;
TIM_HandleTypeDef htim3;

UART_HandleTypeDef huart5;

/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .priority = (osPriority_t) osPriorityHigh,
  .stack_size = 4096 * 4
};
/* Definitions for canRx */
osThreadId_t canRxHandle;
const osThreadAttr_t canRx_attributes = {
  .name = "canRx",
  .priority = (osPriority_t) osPriorityLow,
  .stack_size = 128 * 4
};
/* Definitions for monTask */
osThreadId_t monTaskHandle;
const osThreadAttr_t monTask_attributes = {
  .name = "monTask",
  .priority = (osPriority_t) osPriorityLow,
  .stack_size = 1024 * 4
};
/* Definitions for canTx */
osThreadId_t canTxHandle;
const osThreadAttr_t canTx_attributes = {
  .name = "canTx",
  .priority = (osPriority_t) osPriorityBelowNormal,
  .stack_size = 256 * 4
};
/* USER CODE BEGIN PV */
osMutexId_t uartMutexHandle;
const osMutexAttr_t uartMutex_attributes = {
  .name = "uartMutex"
};
void uart5_puts(const char * str);
volatile int32_t pulse_width_1000deg = 0;
volatile int32_t enc1_count = 0;
volatile uint8_t enc1_prev_state = 0;
volatile uint32_t can_rx_msg_count = 0;
volatile uint32_t last_rx_id = 0;
uint8_t g_device_id = 0;
uint32_t hashed_id_ = 0;

void load_config() {
    BootConfig_t *cfg = (BootConfig_t *)BOOT_CONFIG_ADDR;
    if (cfg->magic == BOOT_CONFIG_MAGIC) {
        g_device_id = cfg->deviceID & 0x3F;
    } else {
        g_device_id = 0; // Default
    }
}

void save_config(uint8_t new_id) {
    //char msg[64];
    extern CRC_HandleTypeDef hcrc;
    
    /* Read existing config from flash */
    BootConfig_t* flashConfig = (BootConfig_t *)BOOT_CONFIG_ADDR;
    
    /* Prepare new config in RAM at the designated location */
    BootConfig_t* ramConfig = RAM_CONFIG_ADDR;
    
    /* Start with current flash config if valid, otherwise zero */
    if (flashConfig->magic == BOOT_CONFIG_MAGIC) {
        memcpy(ramConfig, flashConfig, sizeof(BootConfig_t));
    } else {
        memset(ramConfig, 0, sizeof(BootConfig_t));
    }
    
    /* Update the deviceID */
    ramConfig->magic = BOOT_CONFIG_MAGIC;
    ramConfig->deviceID = new_id;
    
    /* Calculate CRC over appSize through reserved[] (exclude magic and crc fields) */
    /* CRC covers: appSize, appCrc, deviceID, reserved[1004] = 1016 bytes = 254 words */
    ramConfig->crc = HAL_CRC_Calculate(&hcrc, (uint32_t*)&ramConfig->appSize, 
                                       (sizeof(BootConfig_t) - 8) / 4); /* 8 = sizeof(magic) + sizeof(crc) */
    
    /* Set the magic word to trigger config programming on reboot */
    *MAGIC_WORD_ADDR = MAGIC_WORD_PROGRAM_CONFIG;
    
    __disable_irq();
    HAL_NVIC_SystemReset();
}
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_ADC1_Init(void);
static void MX_FDCAN1_Init(void);
static void MX_TIM3_Init(void);
static void MX_I2C1_Init(void);
static void MX_TIM1_Init(void);
static void MX_USART5_UART_Init(void);
static void MX_TIM2_Init(void);
static void MX_IWDG_Init(void);
static void MX_CRC_Init(void);
void StartDefaultTask(void *argument);
void canRxTask(void *argument);
void StartMonTask(void *argument);
void canTxTask(void *argument);

/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
void txstring(const char * str)
{
    int len = strlen(str);
    CDC_Transmit_FS((uint8_t*)str, len);
}

void uart5_puts(const char * str)
{
    if (uartMutexHandle != NULL) {
        osMutexAcquire(uartMutexHandle, osWaitForever);
    }
    HAL_UART_Transmit(&huart5, (uint8_t*)str, strlen(str), 100);
    if (uartMutexHandle != NULL) {
        osMutexRelease(uartMutexHandle);
    }
}

void configureTimerForRunTimeStats(void)
{
  // TIM2 is already configured and started for input capture
}

unsigned long getRunTimeCounterValue(void)
{
  return TIM2->CNT;
}

uint32_t murmur3_32(const uint8_t* key, size_t len, uint32_t seed)
{
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

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_ADC1_Init();
  MX_FDCAN1_Init();
  MX_TIM3_Init();
  // TODO Temp disable it for GPIO Limit switch inputs
  //MX_I2C1_Init();
  MX_TIM1_Init();
  MX_USART5_UART_Init();
  MX_TIM2_Init();
  MX_IWDG_Init();
  MX_CRC_Init();
  /* USER CODE BEGIN 2 */
  HAL_ADCEx_Calibration_Start(&hadc1);
  HAL_IWDG_Refresh(&hiwdg); // Early refresh

  load_config(); // Load device ID from flash

  HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, 1);
  HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, 1);

  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  uartMutexHandle = osMutexNew(&uartMutex_attributes);
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* creation of canRx */
  canRxHandle = osThreadNew(canRxTask, NULL, &canRx_attributes);

  /* creation of monTask */
  monTaskHandle = osThreadNew(StartMonTask, NULL, &monTask_attributes);

  /* creation of canTx */
  canTxHandle = osThreadNew(canTxTask, NULL, &canTx_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  uint32_t uid[3];
  uid[0] = HAL_GetUIDw0();
  uid[1] = HAL_GetUIDw1();
  uid[2] = HAL_GetUIDw2();
  hashed_id_ = murmur3_32((uint8_t*)uid, 12, 0);
  /* USER CODE END RTOS_EVENTS */

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI|RCC_OSCILLATORTYPE_LSI
                              |RCC_OSCILLATORTYPE_HSI48;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSI48State = RCC_HSI48_ON;
  RCC_OscInitStruct.HSIDiv = RCC_HSI_DIV1;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV1;
  RCC_OscInitStruct.PLL.PLLN = 8;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Configure the global features of the ADC (Clock, Resolution, Data Alignment and number of conversion)
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV2;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc1.Init.LowPowerAutoWait = DISABLE;
  hadc1.Init.LowPowerAutoPowerOff = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.Overrun = ADC_OVR_DATA_PRESERVED;
  hadc1.Init.SamplingTimeCommon1 = ADC_SAMPLETIME_160CYCLES_5;
  hadc1.Init.SamplingTimeCommon2 = ADC_SAMPLETIME_160CYCLES_5;
  hadc1.Init.OversamplingMode = DISABLE;
  hadc1.Init.TriggerFrequencyMode = ADC_TRIGGER_FREQ_HIGH;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_0;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLINGTIME_COMMON_1;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief CRC Initialization Function
  * @param None
  * @retval None
  */
static void MX_CRC_Init(void)
{

  /* USER CODE BEGIN CRC_Init 0 */

  /* USER CODE END CRC_Init 0 */

  /* USER CODE BEGIN CRC_Init 1 */

  /* USER CODE END CRC_Init 1 */
  hcrc.Instance = CRC;
  hcrc.Init.DefaultPolynomialUse = DEFAULT_POLYNOMIAL_ENABLE;
  hcrc.Init.DefaultInitValueUse = DEFAULT_INIT_VALUE_ENABLE;
  hcrc.Init.InputDataInversionMode = CRC_INPUTDATA_INVERSION_NONE;
  hcrc.Init.OutputDataInversionMode = CRC_OUTPUTDATA_INVERSION_DISABLE;
  hcrc.InputDataFormat = CRC_INPUTDATA_FORMAT_WORDS;
  if (HAL_CRC_Init(&hcrc) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN CRC_Init 2 */

  /* USER CODE END CRC_Init 2 */

}

/**
  * @brief FDCAN1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_FDCAN1_Init(void)
{

  /* USER CODE BEGIN FDCAN1_Init 0 */

  /* USER CODE END FDCAN1_Init 0 */

  /* USER CODE BEGIN FDCAN1_Init 1 */

  /* USER CODE END FDCAN1_Init 1 */
  hfdcan1.Instance = FDCAN1;
  hfdcan1.Init.ClockDivider = FDCAN_CLOCK_DIV1;
  hfdcan1.Init.FrameFormat = FDCAN_FRAME_FD_BRS;
  hfdcan1.Init.Mode = FDCAN_MODE_NORMAL;
  hfdcan1.Init.AutoRetransmission = ENABLE;
  hfdcan1.Init.TransmitPause = DISABLE;
  hfdcan1.Init.ProtocolException = DISABLE;
  hfdcan1.Init.NominalPrescaler = 4;
  hfdcan1.Init.NominalSyncJumpWidth = 1;
  hfdcan1.Init.NominalTimeSeg1 = 12;
  hfdcan1.Init.NominalTimeSeg2 = 3;
  hfdcan1.Init.DataPrescaler = 1;
  hfdcan1.Init.DataSyncJumpWidth = 1;
  hfdcan1.Init.DataTimeSeg1 = 1;
  hfdcan1.Init.DataTimeSeg2 = 1;
  hfdcan1.Init.StdFiltersNbr = 0;
  hfdcan1.Init.ExtFiltersNbr = 0;
  hfdcan1.Init.TxFifoQueueMode = FDCAN_TX_FIFO_OPERATION;
  if (HAL_FDCAN_Init(&hfdcan1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN FDCAN1_Init 2 */

  /* USER CODE END FDCAN1_Init 2 */

}

/**
  * @brief I2C1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.Timing = 0x10B17DB5;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Analogue filter
  */
  if (HAL_I2CEx_ConfigAnalogFilter(&hi2c1, I2C_ANALOGFILTER_ENABLE) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Digital filter
  */
  if (HAL_I2CEx_ConfigDigitalFilter(&hi2c1, 0) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */

  /* USER CODE END I2C1_Init 2 */

}

/**
  * @brief IWDG Initialization Function
  * @param None
  * @retval None
  */
static void MX_IWDG_Init(void)
{

  /* USER CODE BEGIN IWDG_Init 0 */

  /* USER CODE END IWDG_Init 0 */

  /* USER CODE BEGIN IWDG_Init 1 */

  /* USER CODE END IWDG_Init 1 */
  hiwdg.Instance = IWDG;
  hiwdg.Init.Prescaler = IWDG_PRESCALER_64;
  hiwdg.Init.Window = 4095;
  hiwdg.Init.Reload = 4095;
  if (HAL_IWDG_Init(&hiwdg) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN IWDG_Init 2 */

  /* USER CODE END IWDG_Init 2 */

}

/**
  * @brief TIM1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM1_Init(void)
{

  /* USER CODE BEGIN TIM1_Init 0 */

  /* USER CODE END TIM1_Init 0 */

  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_IC_InitTypeDef sConfigIC = {0};

  /* USER CODE BEGIN TIM1_Init 1 */

  /* USER CODE END TIM1_Init 1 */
  htim1.Instance = TIM1;
  htim1.Init.Prescaler = 0;
  htim1.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim1.Init.Period = 65535;
  htim1.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim1.Init.RepetitionCounter = 0;
  htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_IC_Init(&htim1) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterOutputTrigger2 = TIM_TRGO2_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim1, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigIC.ICPolarity = TIM_INPUTCHANNELPOLARITY_RISING;
  sConfigIC.ICSelection = TIM_ICSELECTION_DIRECTTI;
  sConfigIC.ICPrescaler = TIM_ICPSC_DIV1;
  sConfigIC.ICFilter = 0;
  if (HAL_TIM_IC_ConfigChannel(&htim1, &sConfigIC, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM1_Init 2 */

  /* USER CODE END TIM1_Init 2 */

}

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_IC_InitTypeDef sConfigIC = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 63;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 4294967295;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_IC_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigIC.ICPolarity = TIM_INPUTCHANNELPOLARITY_RISING;
  sConfigIC.ICSelection = TIM_ICSELECTION_DIRECTTI;
  sConfigIC.ICPrescaler = TIM_ICPSC_DIV1;
  sConfigIC.ICFilter = 1;
  if (HAL_TIM_IC_ConfigChannel(&htim2, &sConfigIC, TIM_CHANNEL_3) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigIC.ICPolarity = TIM_INPUTCHANNELPOLARITY_FALLING;
  sConfigIC.ICSelection = TIM_ICSELECTION_INDIRECTTI;
  sConfigIC.ICFilter = 0;
  if (HAL_TIM_IC_ConfigChannel(&htim2, &sConfigIC, TIM_CHANNEL_4) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */

}

/**
  * @brief TIM3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM3_Init(void)
{

  /* USER CODE BEGIN TIM3_Init 0 */

  /* USER CODE END TIM3_Init 0 */

  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_IC_InitTypeDef sConfigIC = {0};

  /* USER CODE BEGIN TIM3_Init 1 */

  /* USER CODE END TIM3_Init 1 */
  htim3.Instance = TIM3;
  htim3.Init.Prescaler = 63;
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = 65535;
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_IC_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigIC.ICPolarity = TIM_INPUTCHANNELPOLARITY_BOTHEDGE;
  sConfigIC.ICSelection = TIM_ICSELECTION_DIRECTTI;
  sConfigIC.ICPrescaler = TIM_ICPSC_DIV1;
  sConfigIC.ICFilter = 0;
  if (HAL_TIM_IC_ConfigChannel(&htim3, &sConfigIC, TIM_CHANNEL_3) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_IC_ConfigChannel(&htim3, &sConfigIC, TIM_CHANNEL_4) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM3_Init 2 */

  /* USER CODE END TIM3_Init 2 */

}

/**
  * @brief USART5 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART5_UART_Init(void)
{

  /* USER CODE BEGIN USART5_Init 0 */

  /* USER CODE END USART5_Init 0 */

  /* USER CODE BEGIN USART5_Init 1 */

  /* USER CODE END USART5_Init 1 */
  huart5.Instance = USART5;
  huart5.Init.BaudRate = 115200;
  huart5.Init.WordLength = UART_WORDLENGTH_8B;
  huart5.Init.StopBits = UART_STOPBITS_1;
  huart5.Init.Parity = UART_PARITY_NONE;
  huart5.Init.Mode = UART_MODE_TX_RX;
  huart5.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart5.Init.OverSampling = UART_OVERSAMPLING_16;
  huart5.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart5.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  huart5.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart5) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART5_Init 2 */

  /* USER CODE END USART5_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOC, CAN_STB_Pin|LED1_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(SYS_STATUS_GPIO_Port, SYS_STATUS_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, LED2_Pin|CAN_STATUS_Pin|SPARE1_Pin|SPARE2_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pins : CAN_STB_Pin LED1_Pin */
  GPIO_InitStruct.Pin = CAN_STB_Pin|LED1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pin : SYS_STATUS_Pin */
  GPIO_InitStruct.Pin = SYS_STATUS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(SYS_STATUS_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : LED2_Pin CAN_STATUS_Pin SPARE1_Pin SPARE2_Pin */
  GPIO_InitStruct.Pin = LED2_Pin|CAN_STATUS_Pin|SPARE1_Pin|SPARE2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM2 && htim->Channel == HAL_TIM_ACTIVE_CHANNEL_4)
  {
    uint32_t t_rise = HAL_TIM_ReadCapturedValue(htim, TIM_CHANNEL_3);
    uint32_t t_fall = HAL_TIM_ReadCapturedValue(htim, TIM_CHANNEL_4);

    uint32_t pulse_width = t_fall - t_rise;
    /*
    Refer to this for the Rev Through Bore Encoder Pulse Width specification:
    https://docs.broadcom.com/doc/pub-005892

    1025 us is the max value. We have a 64 MHz clock, therefore the max value should be 65600
    PWM period: 1025

      t=0us  t=1us                             t=1024us  t=1025us
        |      |                                   |         |
        v      v                                   v         v
      +---+                                                +---+
PWM   |   |                                                |   |  Absolute position = 16'h0
      +   +------------------------------------------------+   +
      :   :                                                :   :
      :   :                                                :   :
      +   +------------------------------------------------+   +
PWM   |                                                        |  Absolute position = 16'hFFFF
      +---+                                                +---+
        ^      ^                                   ^         ^
        |      |                                   |         |
      t=0us  t=1us                             t=1024us  t=1025us
    */ 
    if (pulse_width > 65600)
    {
      pulse_width = t_rise - t_fall;
    }
    int pulse_width_us = pulse_width / 64;
    pulse_width_1000deg = (pulse_width_us * 36000) / 1025;
  }
  else if (htim->Instance == TIM3)
  {
    // Software Quadrature Decoder for TIM3 CH3 (PB0) and CH4 (PB1)
    // TIM3_CH3 is PB0, TIM3_CH4 is PB1
    uint8_t a = (GPIOB->IDR & GPIO_PIN_0) ? 1 : 0;
    uint8_t b = (GPIOB->IDR & GPIO_PIN_1) ? 1 : 0;
    uint8_t curr_state = (a << 1) | b;
    static const int8_t q_table[16] = {0, 1, -1, 0, -1, 0, 0, 1, 1, 0, 0, -1, 0, -1, 1, 0};
    
    enc1_count += q_table[(enc1_prev_state << 2) | curr_state];
    enc1_prev_state = curr_state;
  }
}

/* USER CODE END 4 */

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* init code for USB_Device */
  MX_USB_Device_Init();
  /* USER CODE BEGIN 5 */

  /* Infinite loop */
  for(;;)
  {
    osDelay(100);
  }
  /* USER CODE END 5 */
}

/* USER CODE BEGIN Header_canRxTask */
/**
* @brief Function implementing the canRx thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_canRxTask */
void canRxTask(void *argument)
{
  /* USER CODE BEGIN canRxTask */
  HAL_GPIO_WritePin(CAN_STB_GPIO_Port, CAN_STB_Pin, GPIO_PIN_RESET);

  HAL_FDCAN_Stop(&hfdcan1); // Ensure it's stopped before configuring filter

  FDCAN_FilterTypeDef sFilterConfig;
  sFilterConfig.IdType = FDCAN_EXTENDED_ID;
  sFilterConfig.FilterIndex = 0;
  sFilterConfig.FilterType = FDCAN_FILTER_MASK;
  sFilterConfig.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
  // Temporary: Accept ALL messages for debugging
  sFilterConfig.FilterID1 = 0;
  sFilterConfig.FilterID2 = 0;
  HAL_FDCAN_ConfigFilter(&hfdcan1, &sFilterConfig);

  HAL_FDCAN_Start(&hfdcan1);


  /* Infinite loop */
  for(;;)
  {
    // Handle Incoming CAN messages
    FDCAN_RxHeaderTypeDef rxHeader;
    uint8_t rxData[8];
    while (HAL_FDCAN_GetRxMessage(&hfdcan1, FDCAN_RX_FIFO0, &rxHeader, rxData) == HAL_OK)
    {
        can_rx_msg_count++;
        last_rx_id = rxHeader.Identifier;
        if (rxHeader.Identifier == WPILIB_HEARTBEAT_ID)
        {
            HAL_GPIO_TogglePin(CAN_STATUS_GPIO_Port, CAN_STATUS_Pin);
        }

        uint32_t apiClass = (rxHeader.Identifier >> 10) & 0x3F;
        uint32_t devId = rxHeader.Identifier & 0x3F;

        if (apiClass == 0 && devId == 0) { // System Broadcast Class
            uint32_t target_uid = *((uint32_t *)&rxData[0]);
            if (target_uid == hashed_id_) {
                uint8_t new_id = rxData[4] & 0x3F;
                save_config(new_id); // Does not return - reboots to program config via bootloader
            }
        }

        if (apiClass == 1 && devId == g_device_id) { // Control Class for OUR device
            uint8_t cmd = rxData[0];
            if (cmd == CAN_CMD_START) {
                // Send ACK 0xAA 0x01 to indicate app-to-bootloader transition
                FDCAN_TxHeaderTypeDef txHeader;
                txHeader.Identifier = rxHeader.Identifier;
                txHeader.IdType = FDCAN_EXTENDED_ID;
                txHeader.TxFrameType = FDCAN_DATA_FRAME;
                txHeader.DataLength = FDCAN_DLC_BYTES_8;
                txHeader.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
                txHeader.BitRateSwitch = FDCAN_BRS_OFF;
                txHeader.FDFormat = FDCAN_CLASSIC_CAN;
                txHeader.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
                txHeader.MessageMarker = 0; 
                
                uint8_t ackData[8] = {0xAA, 0x01, 0, 0, 0, 0, 0, 0};
                HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan1, &txHeader, ackData);
                
                // Set Magic Word and Reboot (survives bootloader startup at this address)
                *MAGIC_WORD_ADDR = MAGIC_WORD_PROGRAM_FW;
                __disable_irq();
                HAL_NVIC_SystemReset();
              } 
            }
    }
    osDelay(1);
  }
  /* USER CODE END canRxTask */
}

/* USER CODE BEGIN Header_StartMonTask */
/**
* @brief Function implementing the monTask thread.
* @param argument: Not used
* @retval None
*/
static int compareTaskStatus(const void *a, const void *b)
{
    const TaskStatus_t *p1 = (const TaskStatus_t *)a;
    const TaskStatus_t *p2 = (const TaskStatus_t *)b;
    if (p1->xTaskNumber < p2->xTaskNumber) return -1;
    if (p1->xTaskNumber > p2->xTaskNumber) return 1;
    return 0;
}
/* USER CODE END Header_StartMonTask */
void StartMonTask(void *argument)
{
  /* USER CODE BEGIN StartMonTask */
  char stats_buffer[1024];
  /* Infinite loop */
  for(;;)
  {
    osDelay(1000);
    HAL_IWDG_Refresh(&hiwdg);
    
    // Clear screen and home cursor
    uart5_puts("\r\n\r\n");
    
    // Header
    uart5_puts("================ SYSTEM PERFORMANCE MONITOR ================\r\n");
    
    // Stack and Task Info
    uart5_puts("--- Task Status ---\r\n");
    uart5_puts("Name             State  Prio  FreeStack(Words)  ID\r\n");
    uart5_puts("----------------------------------------------------------\r\n");
    vTaskList(stats_buffer);
    uart5_puts(stats_buffer);

    // CPU Usage
    uart5_puts("\r\n--- CPU Usage ---\r\n");
    uart5_puts("Name             Abs Time       Time %\r\n");
    uart5_puts("----------------------------------------------------------\r\n");
    
    UBaseType_t uxArraySize = uxTaskGetNumberOfTasks();
    TaskStatus_t *pxTaskStatusArray = pvPortMalloc(uxArraySize * sizeof(TaskStatus_t));

    if (pxTaskStatusArray != NULL) {
        uint32_t ulTotalRunTime;
        uxArraySize = uxTaskGetSystemState(pxTaskStatusArray, uxArraySize, &ulTotalRunTime);

        if (ulTotalRunTime > 0) {
            // Sort by Task ID (xTaskNumber)
            qsort(pxTaskStatusArray, uxArraySize, sizeof(TaskStatus_t), compareTaskStatus);
            
            for (UBaseType_t x = 0; x < uxArraySize; x++) {
                uint32_t ulStatsAsPercentage = (uint32_t)((uint64_t)pxTaskStatusArray[x].ulRunTimeCounter * 10000 / ulTotalRunTime);
                uint32_t integral = ulStatsAsPercentage / 100;
                uint32_t fractional = ulStatsAsPercentage % 100;
                
                sprintf(stats_buffer, "%-16s %-14lu %2lu.%02lu%%\r\n",
                        pxTaskStatusArray[x].pcTaskName,
                        (unsigned long)pxTaskStatusArray[x].ulRunTimeCounter,
                        (unsigned long)integral, (unsigned long)fractional);
                uart5_puts(stats_buffer);
            }
        }
        vPortFree(pxTaskStatusArray);
    }
 
    // Heap Info
    // uart5_puts("\r\n--- Memory Info ---\r\n");
    // char heap_str[64];
    // sprintf(heap_str, "Free Heap: %u bytes\r\n", (unsigned int)xPortGetFreeHeapSize());
    // uart5_puts(heap_str);
    
    // snprintf(stats_buffer, sizeof(stats_buffer),
    //          "\r\n--- CAN Debug ---\r\n"
    //          "CAN Rx Count: %lu\r\n"
    //          "Last Rx ID: 0x%08lX\r\n",
    //          (unsigned long)can_rx_msg_count, (unsigned long)last_rx_id);
    // uart5_puts(stats_buffer);

    uart5_puts("============================================================\r\n");
    HAL_GPIO_TogglePin(SYS_STATUS_GPIO_Port, SYS_STATUS_Pin);

  }
  /* USER CODE END StartMonTask */
}

/* USER CODE BEGIN Header_canTxTask */
/**
* @brief Function implementing the canTx thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_canTxTask */
void canTxTask(void *argument)
{
  /* USER CODE BEGIN canTxTask */
  // TODO: Use the QWIIC port as a limit switch input.
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  GPIO_InitStruct.Pin = GPIO_PIN_9 | GPIO_PIN_10;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  HAL_TIM_IC_Start(&htim2, TIM_CHANNEL_3);
  HAL_TIM_IC_Start_IT(&htim2, TIM_CHANNEL_4);

  // Start TIM3 software encoder (Interrupts on both edges)
  HAL_TIM_IC_Start_IT(&htim3, TIM_CHANNEL_3);
  HAL_TIM_IC_Start_IT(&htim3, TIM_CHANNEL_4);

  uint32_t uid[3];
  uid[0] = HAL_GetUIDw0();
  uid[1] = HAL_GetUIDw1();
  uid[2] = HAL_GetUIDw2();
  uint32_t hashed_id = murmur3_32((uint8_t*)uid, 12, 0);

  FDCAN_TxHeaderTypeDef txGeneralStatus;
  txGeneralStatus.Identifier = BACK_PORCH_GENERAL_STATUS | g_device_id;
  txGeneralStatus.IdType = FDCAN_EXTENDED_ID;
  txGeneralStatus.TxFrameType = FDCAN_DATA_FRAME;
  txGeneralStatus.DataLength = FDCAN_DLC_BYTES_8;
  txGeneralStatus.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
  txGeneralStatus.BitRateSwitch = FDCAN_BRS_OFF;
  txGeneralStatus.FDFormat = FDCAN_CLASSIC_CAN;
  txGeneralStatus.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
  txGeneralStatus.MessageMarker = 0;

  FDCAN_TxHeaderTypeDef txSwVersion;
  txSwVersion.Identifier = BACK_PORCH_SW_VERSION | g_device_id;
  txSwVersion.IdType = FDCAN_EXTENDED_ID;
  txSwVersion.TxFrameType = FDCAN_DATA_FRAME;
  txSwVersion.DataLength = FDCAN_DLC_BYTES_8;
  txSwVersion.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
  txSwVersion.BitRateSwitch = FDCAN_BRS_OFF;
  txSwVersion.FDFormat = FDCAN_CLASSIC_CAN;
  txSwVersion.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
  txSwVersion.MessageMarker = 0;

  FDCAN_TxHeaderTypeDef txEncStatus;
  txEncStatus.Identifier = BACK_PORCH_ENCODER_STATUS | g_device_id;
  txEncStatus.IdType = FDCAN_EXTENDED_ID;
  txEncStatus.TxFrameType = FDCAN_DATA_FRAME;
  txEncStatus.DataLength = FDCAN_DLC_BYTES_8;
  txEncStatus.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
  txEncStatus.BitRateSwitch = FDCAN_BRS_OFF;
  txEncStatus.FDFormat = FDCAN_CLASSIC_CAN;
  txEncStatus.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
  txEncStatus.MessageMarker = 0;

  FDCAN_TxHeaderTypeDef txTofStatus;
  txTofStatus.Identifier = BACK_PORCH_TOF_STATUS | g_device_id;
  txTofStatus.IdType = FDCAN_EXTENDED_ID;
  txTofStatus.TxFrameType = FDCAN_DATA_FRAME;
  txTofStatus.DataLength = FDCAN_DLC_BYTES_8;
  txTofStatus.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
  txTofStatus.BitRateSwitch = FDCAN_BRS_OFF;
  txTofStatus.FDFormat = FDCAN_CLASSIC_CAN;
  txTofStatus.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
  txTofStatus.MessageMarker = 0;

  uint8_t TxData[8];

  ADC_ChannelConfTypeDef sConfig = {0};
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLINGTIME_COMMON_1;

  /* Infinite loop */
  for(;;)
  {
    osDelay(100);

    uint32_t vref_mv = 3300;
    uint16_t voltage_mv = 0;
    uint8_t current_mA = 0;
    int8_t temp_c = -127;

    // Read VREFINT to calculate actual VREF+
    sConfig.Channel = ADC_CHANNEL_VREFINT;
    HAL_ADC_ConfigChannel(&hadc1, &sConfig);
    HAL_ADC_Start(&hadc1);
    if (HAL_ADC_PollForConversion(&hadc1, 10) == HAL_OK) {
      uint32_t vref_data = HAL_ADC_GetValue(&hadc1);
      if (vref_data > 0) {
        vref_mv = (uint32_t)VREFINT_CAL_VREF * (*VREFINT_CAL_ADDR) / vref_data;
      }
    }
    HAL_ADC_Stop(&hadc1);

    // Sanity check for vref_mv
    if (vref_mv < 2000 || vref_mv > 3600) vref_mv = 3300;

    // Read AN0 (VIN_MEASURE)
    sConfig.Channel = ADC_CHANNEL_0;
    HAL_ADC_ConfigChannel(&hadc1, &sConfig);
    HAL_ADC_Start(&hadc1);
    if (HAL_ADC_PollForConversion(&hadc1, 10) == HAL_OK) {
      uint32_t adc_val = HAL_ADC_GetValue(&hadc1);
      voltage_mv = (uint16_t)((adc_val * vref_mv * 11) / 4095);
    }
    HAL_ADC_Stop(&hadc1);

    // Read AN1 (CURRENT_5V)
    sConfig.Channel = ADC_CHANNEL_1;
    HAL_ADC_ConfigChannel(&hadc1, &sConfig);
    HAL_ADC_Start(&hadc1);
    if (HAL_ADC_PollForConversion(&hadc1, 10) == HAL_OK) {
      uint32_t adc_val = HAL_ADC_GetValue(&hadc1);
      uint32_t current_val = (uint32_t)((adc_val * (uint64_t)vref_mv * 40) / 4095) / 1000;
      current_mA = current_val > 255 ? 255 : (uint8_t)current_val;
    }
    HAL_ADC_Stop(&hadc1);

    // Read Internal Temperature Sensor
    sConfig.Channel = ADC_CHANNEL_TEMPSENSOR;
    HAL_ADC_ConfigChannel(&hadc1, &sConfig);
    HAL_ADC_Start(&hadc1);
    if (HAL_ADC_PollForConversion(&hadc1, 10) == HAL_OK) {
      uint32_t ts_data = HAL_ADC_GetValue(&hadc1);
      int32_t ts_data_scaled = (int32_t)ts_data * vref_mv / 3000;
      temp_c = (int8_t)((((int32_t)TEMPSENSOR_CAL2_TEMP - TEMPSENSOR_CAL1_TEMP) * (ts_data_scaled - (int32_t)*TEMPSENSOR_CAL1_ADDR)) 
                       / ((int32_t)*TEMPSENSOR_CAL2_ADDR - (int32_t)*TEMPSENSOR_CAL1_ADDR) + TEMPSENSOR_CAL1_TEMP);
    }
    HAL_ADC_Stop(&hadc1);

    FDCAN_ProtocolStatusTypeDef ProtocolStatus;
    HAL_FDCAN_GetProtocolStatus(&hfdcan1, &ProtocolStatus);
    if (ProtocolStatus.BusOff) {
      HAL_FDCAN_Start(&hfdcan1);
      HAL_GPIO_TogglePin(LED1_GPIO_Port, LED1_Pin);
    }

    // Populate TxData based on General Status message format
    TxData[0] = (uint8_t)(hashed_id & 0xFF);
    TxData[1] = (uint8_t)((hashed_id >> 8) & 0xFF);
    TxData[2] = (uint8_t)((hashed_id >> 16) & 0xFF);
    TxData[3] = (uint8_t)((hashed_id >> 24) & 0xFF);

    TxData[4] = current_mA;
    TxData[5] = (uint8_t)(voltage_mv & 0xFF);
    TxData[6] = (uint8_t)((voltage_mv >> 8) & 0xFF);
    TxData[7] = (uint8_t)temp_c;

    while (HAL_FDCAN_GetTxFifoFreeLevel(&hfdcan1) == 0) osDelay(1);
    HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan1, &txGeneralStatus, TxData);

    // Populate TxData for SW Version
    TxData[0] = (uint8_t)(hashed_id & 0xFF);
    TxData[1] = (uint8_t)((hashed_id >> 8) & 0xFF);
    TxData[2] = (uint8_t)((hashed_id >> 16) & 0xFF);
    TxData[3] = (uint8_t)((hashed_id >> 24) & 0xFF);
    TxData[4] = 1 | (MAJOR_VERSION << 1) | (MINOR_VERSION << 4);
    TxData[5] = (uint8_t)(BUILD_NUMBER & 0xFF);
    TxData[6] = (uint8_t)((BUILD_NUMBER >> 8) & 0xFF);
    TxData[7] = (uint8_t)((BUILD_NUMBER >> 16) & 0xFF);
    
    while (HAL_FDCAN_GetTxFifoFreeLevel(&hfdcan1) == 0) osDelay(1);
    HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan1, &txSwVersion, TxData);

    TxData[0] = (uint8_t)(pulse_width_1000deg & 0xFF);
    TxData[1] = (uint8_t)((pulse_width_1000deg >> 8) & 0xFF);
    
    TxData[2] = (uint8_t)(enc1_count & 0xFF);
    TxData[3] = (uint8_t)((enc1_count >> 8) & 0xFF);
    TxData[4] = 0;
    TxData[5] = 0;
    TxData[6] = 0;
    TxData[7] = 0;

    while (HAL_FDCAN_GetTxFifoFreeLevel(&hfdcan1) == 0) osDelay(1);
    HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan1, &txEncStatus, TxData);

    uint8_t limit_switches = 0;
    // Bit 0: SDA (PA10), Bit 1: SCL (PA9)
    if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_10) == GPIO_PIN_RESET) limit_switches |= 0x01;
    if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_9) == GPIO_PIN_RESET) limit_switches |= 0x02;

    TxData[0] = 0; // API Status
    TxData[1] = limit_switches;
    TxData[2] = 0; // Distance low
    TxData[3] = 0; // Distance high
    TxData[4] = 0; // Ambient low
    TxData[5] = 0; // Ambient high
    TxData[6] = 0; // Signal low
    TxData[7] = 0; // Signal high
    
    while (HAL_FDCAN_GetTxFifoFreeLevel(&hfdcan1) == 0) osDelay(1);
    HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan1, &txTofStatus, TxData);
  }
  /* USER CODE END canTxTask */
}

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM6 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM6)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
