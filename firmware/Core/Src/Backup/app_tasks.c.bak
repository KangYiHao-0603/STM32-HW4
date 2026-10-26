/**
 * app_tasks.c
 * FreeRTOS (CMSIS-RTOS v2) tasks: TASK_BLE / TASK_ACC
 *
 * CubeMX 設定前提（見 BLE_INTEGRATION.md）：
 *   - Task "TASK_BLE"，Entry Function = StartTask02，Code Generation Option = As external
 *   - Task "TASK_ACC"，Entry Function = StartTask03，Code Generation Option = As external
 *     （entry 名稱沿用本專案 .ioc 目前的設定，見 freertos.c）
 *   - Mutex "bleMutex"
 *   - EXTI9_5 (PE6, BLE IRQ) 與 EXTI15_10 (PD11, LSM6DSL INT1) 的 preemption priority >= 5
 *     （hci_tl_lowlevel_init() 會把 EXTI9_5 設成 0，已在其 USER CODE 區塊改回 5）
 */
#include "main.h"
#include "cmsis_os2.h"

#include <stdio.h>
#include <stdbool.h>

#include "hci.h"
#include "bluenrg_def.h"
#include "acc_gatt.h"

/* ======================== 選項 ======================== */
/* 1 = 啟用 option (b) LSM6DSL significant motion detection（PD11 / EXTI11） */
#define ENABLE_SIG_MOTION      1
/* 0 = MX_BlueNRG_MS_Init() 仍由 CubeMX 放在 main()（kernel 啟動前）呼叫
 * 1 = main() 裡已移除該呼叫，改在 TASK_BLE 內初始化 */
#define BLE_INIT_IN_TASK       0
/* significant motion 的步數門檻（SM_THS，預設 6） */
#define SIG_MOTION_THRESHOLD   6u

/* ======================== 外部符號 ======================== */
/* 由 CubeMX 在 main.c 產生 */
extern osThreadId_t TASK_BLEHandle;
extern osThreadId_t TASK_ACCHandle;
extern osMutexId_t  bleMutexHandle;

/* X-CUBE-BLE1 SensorDemo（sensor.c）裡的廣播控制 */
extern volatile uint8_t set_connectable;
extern void Set_DeviceConnectable(void);
#if BLE_INIT_IN_TASK
extern void MX_BlueNRG_MS_Init(void);
#endif

/* stm32l475e_iot01_accelero.c 提供。不 include stm32l475e_iot01_accelero.h：
 * 它會帶入 stm32l475e_iot01.h，其 Led_TypeDef/Button_TypeDef/COM_TypeDef 和 main.h → custom.h 重複定義 */
typedef enum
{
  ACCELERO_OK = 0,
  ACCELERO_ERROR = 1,
  ACCELERO_TIMEOUT = 2
} ACCELERO_StatusTypeDef;
extern ACCELERO_StatusTypeDef BSP_ACCELERO_Init(void);
extern void BSP_ACCELERO_AccGetXYZ(int16_t *pDataXYZ);

/* sensor_io.c 提供的 I2C2 存取函式（本專案不使用 stm32l475e_iot01.c） */
extern void    SENSOR_IO_Write(uint8_t Addr, uint8_t Reg, uint8_t Value);
extern uint8_t SENSOR_IO_Read(uint8_t Addr, uint8_t Reg);

/* ======================== 常數 ======================== */
#define FLAG_BLE_IRQ      0x0001u   /* TASK_BLE：BlueNRG 有事件 */
#define FLAG_MOTION_IRQ   0x0002u   /* TASK_ACC：LSM6DSL INT1 */

#define LSM6DSL_ADDR            0xD4u   /* SA0 = 0 */
#define REG_FUNC_CFG_ACCESS     0x01u   /* bit7 FUNC_CFG_EN：切換到 embedded function bank A */
#define REG_INT1_CTRL           0x0Du   /* bit6 INT1_SIGN_MOT */
#define REG_CTRL1_XL            0x10u   /* [7:4] ODR_XL, [3:2] FS_XL, [1:0] BW */
#define REG_CTRL10_C            0x19u   /* bit2 FUNC_EN, bit0 SIGN_MOTION_EN */
#define REG_FUNC_SRC1           0x53u   /* bit6 SIGN_MOTION_IA */
#define BANKA_SM_THS            0x13u   /* bank A：significant motion 步數門檻 */

#define FUNC_CFG_EN             0x80u
#define INT1_SIGN_MOT           0x40u
#define CTRL10_FUNC_EN          0x04u
#define CTRL10_SIGN_MOTION_EN   0x01u
#define FUNC_SRC1_SIGN_MOTION   0x40u

/* ======================== 共用 helper ======================== */
static inline bool kernel_running(void)
{
  return osKernelGetState() == osKernelRunning;
}

static inline void ble_lock(void)   { osMutexAcquire(bleMutexHandle, osWaitForever); }
static inline void ble_unlock(void) { osMutexRelease(bleMutexHandle); }

/* ================================================================
 * 中斷 → 喚醒 task（只 set flag，不碰 ACI / I2C）
 * ================================================================ */

/* 在 hci_tl_interface.c 的 hci_tl_lowlevel_isr() 結尾呼叫（見 README Step 6） */
void App_BleIrqNotify(void)
{
  if (kernel_running() && TASK_BLEHandle != NULL)
  {
    osThreadFlagsSet(TASK_BLEHandle, FLAG_BLE_IRQ);
  }
}

#if ENABLE_SIG_MOTION
/* PD11 = LSM6DSL_INT1（EXTI11）。stm32l4xx_it.c 的 EXTI15_10_IRQHandler 會呼叫
 * HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_11) → 這裡。專案中沒有其他 HAL_GPIO_EXTI_Callback：
 * PE6（BLE）與 PC13（按鈕）都走 HAL_EXTI_RegisterCallback 機制，不經過這個函式。 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == GPIO_PIN_11 && kernel_running() && TASK_ACCHandle != NULL)
  {
    osThreadFlagsSet(TASK_ACCHandle, FLAG_MOTION_IRQ);
  }
}
#endif

/* ================================================================
 * LSM6DSL helper（只在 TASK_ACC 內呼叫，I2C 不需額外鎖）
 * ================================================================ */

/* 依取樣頻率設定加速度計 ODR：取 >= 2*freq 的最小檔位，下限 26 Hz（significant motion 需要） */
static void Acc_SetOdr(uint16_t freq_hz)
{
  uint8_t  odr_bits;
  uint32_t need = (uint32_t)freq_hz * 2u;

  if      (need <= 26u)  odr_bits = 0x2u;  /*  26 Hz */
  else if (need <= 52u)  odr_bits = 0x3u;  /*  52 Hz */
  else if (need <= 104u) odr_bits = 0x4u;  /* 104 Hz */
  else                   odr_bits = 0x5u;  /* 208 Hz */

  uint8_t ctrl1 = SENSOR_IO_Read(LSM6DSL_ADDR, REG_CTRL1_XL);
  ctrl1 = (uint8_t)((ctrl1 & 0x0Fu) | (odr_bits << 4));  /* 保留 FS 與 BW */
  SENSOR_IO_Write(LSM6DSL_ADDR, REG_CTRL1_XL, ctrl1);
}

#if ENABLE_SIG_MOTION
static void Acc_EnableSigMotion(void)
{
  /* 1. 切到 bank A 設定步數門檻，完成後立刻切回 */
  SENSOR_IO_Write(LSM6DSL_ADDR, REG_FUNC_CFG_ACCESS, FUNC_CFG_EN);
  SENSOR_IO_Write(LSM6DSL_ADDR, BANKA_SM_THS, SIG_MOTION_THRESHOLD);
  SENSOR_IO_Write(LSM6DSL_ADDR, REG_FUNC_CFG_ACCESS, 0x00u);

  /* 2. 開啟 embedded functions + significant motion */
  uint8_t c10 = SENSOR_IO_Read(LSM6DSL_ADDR, REG_CTRL10_C);
  c10 |= (CTRL10_FUNC_EN | CTRL10_SIGN_MOTION_EN);
  SENSOR_IO_Write(LSM6DSL_ADDR, REG_CTRL10_C, c10);

  /* 3. 事件導到 INT1 腳（PD11） */
  uint8_t i1 = SENSOR_IO_Read(LSM6DSL_ADDR, REG_INT1_CTRL);
  i1 |= INT1_SIGN_MOT;
  SENSOR_IO_Write(LSM6DSL_ADDR, REG_INT1_CTRL, i1);

  /* 4. 先讀一次清掉殘留事件 */
  (void)SENSOR_IO_Read(LSM6DSL_ADDR, REG_FUNC_SRC1);

  printf("[ACC] significant motion enabled (threshold=%u steps)\r\n", SIG_MOTION_THRESHOLD);
}
#endif

/* ================================================================
 * TASK_BLE：GAP 連線/斷線、GATT write 等 BLE 事件處理
 * ================================================================ */
void StartTask02(void *argument)
{
  (void)argument;

#if BLE_INIT_IN_TASK
  ble_lock();
  MX_BlueNRG_MS_Init();      /* 內含 Sensor_DeviceInit() → AccGatt_Init() */
  ble_unlock();
#endif
  printf("[BLE] TASK_BLE started\r\n");

  for (;;)
  {
    /* IRQ 喚醒；10 ms timeout 當作保底輪詢 */
    (void)osThreadFlagsWait(FLAG_BLE_IRQ, osFlagsWaitAny, 10);

    ble_lock();
    /* 原本 User_Process() 負責的「重新廣播」：開機與斷線後 set_connectable 會被設成 1 */
    if (set_connectable)
    {
      Set_DeviceConnectable();
      set_connectable = 0;
      printf("[GAP] advertising...\r\n");
    }
    /* 處理 HCI 事件 → 觸發 sensor.c / gatt_db.c 裡的事件 callback → AccGatt_OnXxx() */
    hci_user_evt_proc();
    ble_unlock();
  }
}

/* ================================================================
 * TASK_ACC：依指定頻率讀取 LSM6DSL 並 notify
 * ================================================================ */
void StartTask03(void *argument)
{
  (void)argument;
  int16_t  xyz[3];
  uint32_t sent = 0, dropped = 0;
#if ENABLE_SIG_MOTION
  uint16_t motion_count = 0;
#endif

  if (BSP_ACCELERO_Init() != ACCELERO_OK)
  {
    printf("[ACC] BSP_ACCELERO_Init FAILED\r\n");
  }
  uint16_t freq = AccGatt_GetFreq();
  Acc_SetOdr(freq);
#if ENABLE_SIG_MOTION
  Acc_EnableSigMotion();
#endif
  printf("[ACC] TASK_ACC started, %u Hz\r\n", freq);

  const uint32_t tick_hz = osKernelGetTickFreq();
  uint32_t period    = tick_hz / freq;
  uint32_t next      = osKernelGetTickCount() + period;
  uint32_t stat_next = osKernelGetTickCount() + tick_hz;

  for (;;)
  {
    /* ---- 等到下一個取樣時間點；期間若有 motion 中斷會提早醒來 ---- */
    uint32_t now = osKernelGetTickCount();
    int32_t  remain = (int32_t)(next - now);
    if (remain > 0)
    {
      uint32_t flags = osThreadFlagsWait(FLAG_MOTION_IRQ, osFlagsWaitAny, (uint32_t)remain);
#if ENABLE_SIG_MOTION
      if (!(flags & 0x80000000u) && (flags & FLAG_MOTION_IRQ))
      {
        uint8_t src = SENSOR_IO_Read(LSM6DSL_ADDR, REG_FUNC_SRC1);   /* 讀取即清除 */
        if (src & FUNC_SRC1_SIGN_MOTION)
        {
          motion_count++;
          printf("[ACC] *** significant motion #%u ***\r\n", motion_count);
          if (AccGatt_MotionNotifyEnabled())
          {
            ble_lock();
            AccGatt_UpdateMotion(motion_count);
            ble_unlock();
          }
        }
        continue;   /* 回去繼續等取樣時間 */
      }
#else
      (void)flags;
#endif
      continue;
    }

    /* ---- 取樣時間到 ---- */
    if (AccGatt_TakeFreqChanged())
    {
      freq   = AccGatt_GetFreq();
      period = tick_hz / freq;
      Acc_SetOdr(freq);
      next   = now;                         /* 從現在重新起算 */
      printf("[ACC] frequency -> %u Hz\r\n", freq);
    }

    next += period;
    if ((int32_t)(next - now) <= 0) next = now + period;   /* 落後太多就重新同步 */

    BSP_ACCELERO_AccGetXYZ(xyz);

    if (AccGatt_AccNotifyEnabled())
    {
      ble_lock();
      tBleStatus st = AccGatt_UpdateAcc(xyz);
      ble_unlock();

      if (st == BLE_STATUS_SUCCESS)                         sent++;
      else if (st == BLE_STATUS_INSUFFICIENT_RESOURCES)     dropped++;   /* TX buffer 滿，丟棄這筆 */
      else { dropped++; printf("[ACC] update err 0x%02X\r\n", st); }
    }

    /* ---- 每秒統計一次 ---- */
    if ((int32_t)(now - stat_next) >= 0)
    {
      stat_next += tick_hz;
      printf("[ACC] set=%u Hz sent=%lu/s dropped=%lu/s xyz=(%d,%d,%d) mg\r\n",
             freq, (unsigned long)sent, (unsigned long)dropped, xyz[0], xyz[1], xyz[2]);
      sent = dropped = 0;
    }
  }
}
