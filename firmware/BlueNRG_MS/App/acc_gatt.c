/**
 * acc_gatt.c
 * Custom Accelerometer GATT service (GATT server) for B-L475E-IOT01A, BlueNRG-MS (X-CUBE-BLE1 7.1.0, BlueNRG-MS API)
 *
 * 呼叫規則：
 *  - AccGatt_Init()          : 在 MX_BlueNRG_MS_Init() 的 USER CODE PostTreatment 區塊呼叫
 *                              （原本 SensorDemo 的 Add_xxx_Service 已在 gatt_db.c 停用）
 *  - AccGatt_UpdateAcc/Motion: 會呼叫 ACI，呼叫前必須持有 bleMutex
 *  - AccGatt_OnXxx()         : 從既有的事件 callback 呼叫，這些 callback 由 hci_user_evt_proc()
 *                              在 TASK_BLE 中執行，TASK_BLE 呼叫時已持有 bleMutex
 */
#include "acc_gatt.h"

#include <stdio.h>
#include <string.h>

#include "bluenrg_def.h"
#include "bluenrg_conf.h"
#include "hci.h"
#include "bluenrg_gatt_aci.h"
#include "bluenrg_aci_const.h"

/* ---------------- UUID（128-bit，陣列為 little-endian，即字串反向） ----------------
 * Service : 7a1e0000-3c2b-4d5e-9f10-a1b2c3d4e5f6
 * ACC     : 7a1e0001-3c2b-4d5e-9f10-a1b2c3d4e5f6
 * FREQ    : 7a1e0002-3c2b-4d5e-9f10-a1b2c3d4e5f6
 * MOTION  : 7a1e0003-3c2b-4d5e-9f10-a1b2c3d4e5f6
 */
#define ACC_UUID_LE(id) { 0xf6,0xe5,0xd4,0xc3,0xb2,0xa1,0x10,0x9f, \
                          0x5e,0x4d,0x2b,0x3c,(id),0x00,0x1e,0x7a }

static const uint8_t SERVICE_UUID[16] = ACC_UUID_LE(0x00);
static const uint8_t ACC_UUID[16]     = ACC_UUID_LE(0x01);
static const uint8_t FREQ_UUID[16]    = ACC_UUID_LE(0x02);
static const uint8_t MOTION_UUID[16]  = ACC_UUID_LE(0x03);

/* ---------------- Handles 與狀態 ---------------- */
static uint16_t accServHandle;
static uint16_t accCharHandle;
static uint16_t freqCharHandle;
static uint16_t motionCharHandle;

static volatile uint16_t connHandle;
static volatile bool     connected;
static volatile bool     accNotifyOn;
static volatile bool     motionNotifyOn;
static volatile uint16_t accFreqHz = ACC_FREQ_DEFAULT_HZ;
static volatile bool     freqChanged;

/* attr handle 配置：Char 宣告 = charHandle、值 = charHandle+1、CCCD = charHandle+2 */
#define VALUE_HANDLE(h)  ((uint16_t)((h) + 1u))
#define CCCD_HANDLE(h)   ((uint16_t)((h) + 2u))

static void put_u16_le(uint8_t *buf, uint16_t v)
{
  buf[0] = (uint8_t)(v & 0xFFu);
  buf[1] = (uint8_t)(v >> 8);
}

static uint16_t clamp_freq(uint32_t f)
{
  if (f < ACC_FREQ_MIN_HZ) return ACC_FREQ_MIN_HZ;
  if (f > ACC_FREQ_MAX_HZ) return ACC_FREQ_MAX_HZ;
  return (uint16_t)f;
}

/* ================================================================ */
uint8_t AccGatt_Init(void)
{
  tBleStatus ret;

  /* 1 (service) + ACC 3 + FREQ 2 + MOTION 3 = 9，留一點餘裕 */
  ret = aci_gatt_add_serv(UUID_TYPE_128, SERVICE_UUID, PRIMARY_SERVICE, 12, &accServHandle);
  if (ret != BLE_STATUS_SUCCESS) { printf("[GATT] add_serv fail 0x%02X\r\n", ret); return ret; }

  /* (a) ACC：6 bytes，NOTIFY | READ */
  ret = aci_gatt_add_char(accServHandle, UUID_TYPE_128, ACC_UUID, 6,
                          CHAR_PROP_NOTIFY | CHAR_PROP_READ,
                          ATTR_PERMISSION_NONE,
                          GATT_DONT_NOTIFY_EVENTS,
                          16, 0, &accCharHandle);
  if (ret != BLE_STATUS_SUCCESS) { printf("[GATT] add ACC fail 0x%02X\r\n", ret); return ret; }

  /* (b) FREQ：2 bytes，WRITE | WRITE_WITHOUT_RESP | READ，寫入時通知應用層 */
  ret = aci_gatt_add_char(accServHandle, UUID_TYPE_128, FREQ_UUID, 2,
                          CHAR_PROP_WRITE | CHAR_PROP_WRITE_WITHOUT_RESP | CHAR_PROP_READ,
                          ATTR_PERMISSION_NONE,
                          GATT_NOTIFY_ATTRIBUTE_WRITE,
                          16, 0, &freqCharHandle);
  if (ret != BLE_STATUS_SUCCESS) { printf("[GATT] add FREQ fail 0x%02X\r\n", ret); return ret; }

  /* (option b) MOTION：2 bytes 事件計數，NOTIFY | READ */
  ret = aci_gatt_add_char(accServHandle, UUID_TYPE_128, MOTION_UUID, 2,
                          CHAR_PROP_NOTIFY | CHAR_PROP_READ,
                          ATTR_PERMISSION_NONE,
                          GATT_DONT_NOTIFY_EVENTS,
                          16, 0, &motionCharHandle);
  if (ret != BLE_STATUS_SUCCESS) { printf("[GATT] add MOTION fail 0x%02X\r\n", ret); return ret; }

  /* FREQ 初始值，讓 client 讀得到目前頻率 */
  uint8_t buf[2];
  put_u16_le(buf, accFreqHz);
  aci_gatt_update_char_value(accServHandle, freqCharHandle, 0, 2, buf);

  printf("[GATT] AccService ready: serv=0x%04X acc=0x%04X freq=0x%04X motion=0x%04X\r\n",
         accServHandle, accCharHandle, freqCharHandle, motionCharHandle);
  return BLE_STATUS_SUCCESS;
}

/* ================================================================ */
uint8_t AccGatt_UpdateAcc(const int16_t xyz[3])
{
  uint8_t buf[6];
  put_u16_le(&buf[0], (uint16_t)xyz[0]);
  put_u16_le(&buf[2], (uint16_t)xyz[1]);
  put_u16_le(&buf[4], (uint16_t)xyz[2]);
  /* client 已開啟 notify 時，update 會自動送出 notification */
  return aci_gatt_update_char_value(accServHandle, accCharHandle, 0, 6, buf);
}

uint8_t AccGatt_UpdateMotion(uint16_t count)
{
  uint8_t buf[2];
  put_u16_le(buf, count);
  return aci_gatt_update_char_value(accServHandle, motionCharHandle, 0, 2, buf);
}

/* ================================================================ */
uint16_t AccGatt_GetFreq(void)            { return accFreqHz; }
bool     AccGatt_IsConnected(void)        { return connected; }
bool     AccGatt_AccNotifyEnabled(void)   { return connected && accNotifyOn; }
bool     AccGatt_MotionNotifyEnabled(void){ return connected && motionNotifyOn; }

bool AccGatt_TakeFreqChanged(void)
{
  bool c = freqChanged;
  freqChanged = false;
  return c;
}

/* ================================================================
 * 事件 hook（在 TASK_BLE 的 hci_user_evt_proc() 中被呼叫）
 * ================================================================ */
void AccGatt_OnConnect(uint16_t conn_handle)
{
  connHandle     = conn_handle;
  connected      = true;
  accNotifyOn    = false;
  motionNotifyOn = false;
  printf("[GAP] connected, handle=0x%04X\r\n", conn_handle);
}

void AccGatt_OnDisconnect(void)
{
  connected      = false;
  accNotifyOn    = false;
  motionNotifyOn = false;
  printf("[GAP] disconnected\r\n");
}

void AccGatt_OnAttrModified(uint16_t attr_handle, uint8_t len, const uint8_t *data)
{
  if (attr_handle == VALUE_HANDLE(freqCharHandle))
  {
    /* GATT write request：設定取樣頻率 */
    uint32_t req = 0;
    if (len >= 2)      req = (uint32_t)data[0] | ((uint32_t)data[1] << 8);
    else if (len == 1) req = data[0];

    uint16_t f = clamp_freq(req);
    accFreqHz   = f;
    freqChanged = true;

    /* 超出範圍時把修正後的值寫回 DB，client 讀回來才會一致（TASK_BLE 已持有 mutex） */
    if (f != req)
    {
      uint8_t buf[2];
      put_u16_le(buf, f);
      aci_gatt_update_char_value(accServHandle, freqCharHandle, 0, 2, buf);
    }
    printf("[GATT] FREQ write: req=%lu -> %u Hz\r\n", (unsigned long)req, f);
  }
  else if (attr_handle == CCCD_HANDLE(accCharHandle))
  {
    accNotifyOn = (len > 0) && (data[0] & 0x01u);
    printf("[GATT] ACC notify %s\r\n", accNotifyOn ? "ON" : "OFF");
  }
  else if (attr_handle == CCCD_HANDLE(motionCharHandle))
  {
    motionNotifyOn = (len > 0) && (data[0] & 0x01u);
    printf("[GATT] MOTION notify %s\r\n", motionNotifyOn ? "ON" : "OFF");
  }
}
