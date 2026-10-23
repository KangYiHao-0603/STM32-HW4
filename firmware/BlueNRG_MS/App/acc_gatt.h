/**
 * acc_gatt.h
 * Custom Accelerometer GATT service for B-L475E-IOT01A (BlueNRG-MS / X-CUBE-BLE1)
 *
 *   Service : 7a1e0000-3c2b-4d5e-9f10-a1b2c3d4e5f6
 *   ACC     : 7a1e0001-...  NOTIFY | READ   6 bytes  int16 x,y,z (mg, little-endian)
 *   FREQ    : 7a1e0002-...  WRITE  | READ   2 bytes  uint16 Hz (1..100)
 *   MOTION  : 7a1e0003-...  NOTIFY | READ   2 bytes  uint16 event counter (option b)
 */
#ifndef ACC_GATT_H
#define ACC_GATT_H

#include <stdint.h>
#include <stdbool.h>

#define ACC_FREQ_DEFAULT_HZ   10u
#define ACC_FREQ_MIN_HZ       1u
#define ACC_FREQ_MAX_HZ       100u

/* 建立 GATT service 與 characteristics。取代原本的 Add_HWServW2ST_Service()/Add_SWServW2ST_Service() */
uint8_t  AccGatt_Init(void);

/* 下列兩個函式會呼叫 ACI，呼叫端必須先取得 bleMutex */
uint8_t  AccGatt_UpdateAcc(const int16_t xyz[3]);
uint8_t  AccGatt_UpdateMotion(uint16_t count);

/* 狀態查詢（給 TASK_ACC 用，不需 mutex） */
uint16_t AccGatt_GetFreq(void);
bool     AccGatt_TakeFreqChanged(void);   /* 讀取後清除旗標 */
bool     AccGatt_IsConnected(void);
bool     AccGatt_AccNotifyEnabled(void);
bool     AccGatt_MotionNotifyEnabled(void);

/* 事件 hook：在 SensorDemo 既有的事件 callback 中呼叫（執行於 TASK_BLE 內） */
void     AccGatt_OnConnect(uint16_t conn_handle);
void     AccGatt_OnDisconnect(void);
void     AccGatt_OnAttrModified(uint16_t attr_handle, uint8_t len, const uint8_t *data);

#endif /* ACC_GATT_H */
