#include "app_tof.h"
#include "vl53l0x_api.h"
#include "vl53l0x_platform.h"
#include "main.h"

extern I2C_HandleTypeDef hi2c1;

static VL53L0X_Dev_t tof_dev;
static VL53L0X_DEV p_tof_dev = &tof_dev;

void app_tof_init(void) {
    uint32_t refSpadCount;
    uint8_t isApertureSpads;
    uint8_t VhvSettings;
    uint8_t PhaseCal;

    // Initialize the device structure
    p_tof_dev->I2cHandle = &hi2c1;
    p_tof_dev->I2cDevAddr = 0x52; // Default VL53L0X 8-bit I2C address (0x29 << 1)
    p_tof_dev->Present = 1;

    VL53L0X_Error status = VL53L0X_ERROR_NONE;

    status = VL53L0X_DataInit(p_tof_dev);
    if (status != VL53L0X_ERROR_NONE) return;

    status = VL53L0X_StaticInit(p_tof_dev);
    if (status != VL53L0X_ERROR_NONE) return;

    status = VL53L0X_PerformRefCalibration(p_tof_dev, &VhvSettings, &PhaseCal);
    if (status != VL53L0X_ERROR_NONE) return;

    status = VL53L0X_PerformRefSpadManagement(p_tof_dev, &refSpadCount, &isApertureSpads);
    if (status != VL53L0X_ERROR_NONE) return;

    // Setup for continuous ranging
    status = VL53L0X_SetDeviceMode(p_tof_dev, VL53L0X_DEVICEMODE_CONTINUOUS_RANGING);
    if (status != VL53L0X_ERROR_NONE) return;

    status = VL53L0X_StartMeasurement(p_tof_dev);
}

void app_tof_read(uint16_t *distance_mm, uint8_t *range_status) {
    static uint16_t last_distance = 8191;
    static uint8_t last_status = 255;
    
    VL53L0X_RangingMeasurementData_t RangingMeasurementData;
    VL53L0X_Error status = VL53L0X_ERROR_NONE;
    uint8_t dataReady = 0;

    status = VL53L0X_GetMeasurementDataReady(p_tof_dev, &dataReady);
    if (status == VL53L0X_ERROR_NONE && dataReady == 1) {
        status = VL53L0X_GetRangingMeasurementData(p_tof_dev, &RangingMeasurementData);
        if (status == VL53L0X_ERROR_NONE) {
            last_distance = RangingMeasurementData.RangeMilliMeter;
            last_status = RangingMeasurementData.RangeStatus;
        }
        VL53L0X_ClearInterruptMask(p_tof_dev, VL53L0X_REG_SYSTEM_INTERRUPT_GPIO_NEW_SAMPLE_READY);
    }

    if (distance_mm) *distance_mm = last_distance;
    if (range_status) *range_status = last_status;
}
