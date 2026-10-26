/**
  ******************************************************************************
  * @file    App/sensor.h
  * @author  SRA Application Team
  * @brief   Header file for App/sensor.c
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

#ifndef SENSOR_H
#define SENSOR_H

#include <stdint.h>

#define IDB04A1 0
#define IDB05A1 1
/* ACC_BLE_PATCH: 廣播名稱，原為 'B','l','u','e','N','R','G'（8 字元以內，廣播封包 <= 31 bytes） */
#define SENSOR_DEMO_NAME   'S','T','M','3','2','A','C','C'
#define BDADDR_SIZE        6

void Set_DeviceConnectable(void);
void user_notify(void * pData);

extern uint8_t Application_Max_Attribute_Records[];

#endif /* SENSOR_H */
