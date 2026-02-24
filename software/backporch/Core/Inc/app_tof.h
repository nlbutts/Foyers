#ifndef APP_TOF_H
#define APP_TOF_H

#include <stdint.h>

void app_tof_init(void);
void app_tof_read(uint16_t *distance_mm, uint8_t *range_status);

#endif /* APP_TOF_H */
