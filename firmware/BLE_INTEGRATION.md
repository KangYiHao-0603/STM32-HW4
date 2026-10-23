# STM32 BLE 加速度計（GATT Server）+ RPi 4（GATT Client）

板子：B-L475E-IOT01A（BlueNRG-MS + LSM6DSL），基底：X-CUBE-BLE1 `SensorDemo_BLESensor-App`

## 檔案

| 檔案 | 放到專案的位置 | 內容 |
|---|---|---|
| `stm32/BlueNRG_MS/App/acc_gatt.h/.c` | `BlueNRG_MS/App/` | 自訂 GATT service（ACC / FREQ / MOTION） |
| `stm32/Core/Src/app_tasks.c` | `Core/Src/` | TASK_BLE、TASK_ACC、significant motion |
| `rpi/ble_client.py` | RPi 上任意資料夾 | bleak GATT client |

## GATT 定義

| Characteristic | UUID | 屬性 | 格式 |
|---|---|---|---|
| Service | `7a1e0000-3c2b-4d5e-9f10-a1b2c3d4e5f6` | | |
| ACC | `7a1e0001-...` | Notify, Read | 6 bytes：int16 x,y,z（mg, little-endian） |
| FREQ | `7a1e0002-...` | Write, Write w/o resp, Read | 2 bytes：uint16 Hz（1~100，預設 10） |
| MOTION | `7a1e0003-...` | Notify, Read | 2 bytes：uint16 事件計數（option b） |

---

## 整合步驟

### Step 1：CubeMX 設定（`.ioc`）

**FreeRTOS（CMSIS_V2）→ Tasks and Queues**

| Task Name | Priority | Stack (words) | Entry Function | Code Generation Option |
|---|---|---|---|---|
| TASK_BLE | osPriorityAboveNormal | 1024 | `StartTaskBLE` | **As external** |
| TASK_ACC | osPriorityNormal | 512 | `StartTaskACC` | **As external** |

- 預設的 `defaultTask` 可以刪除。
- Mutexes 新增 `bleMutex`。
- Config parameters：`TOTAL_HEAP_SIZE` 設 20000 以上。

**選 As external 的原因**：CubeMX 不會產生 task 函式本體，改由 `app_tasks.c` 提供，重新 Generate Code 也不會覆蓋。

**SYS**：Timebase Source = **TIM6**。

**Option (b)**：PD11 設成 `GPIO_EXTI11`，**Rising edge**，No pull。

**NVIC（很重要）**：
- 勾選 EXTI line[9:5] 和 EXTI line[15:10]。
- 這兩個中斷的 **Preemption Priority 設成 5 以上**（數字 ≥ 5），並勾 **Uses FreeRTOS functions**。
- 原因：中斷裡會呼叫 `osThreadFlagsSet()`，優先權數字小於 5 會讓 FreeRTOS assert 或 HardFault。

Generate Code。

### Step 2：加入 BSP 與 include path
從 `STM32Cube_FW_L4_Vx.x\Drivers\BSP\` 複製到專案：
- `B-L475E-IOT01\stm32l475e_iot01.c/.h`、`stm32l475e_iot01_accelero.c/.h`
- `Components\lsm6dsl\`、`Components\Common\`

Project Properties → C/C++ Build → Settings → MCU GCC Compiler → Include paths，加入上述資料夾。

### Step 3：放入新檔案
把 `acc_gatt.h/.c` 放進 `BlueNRG_MS/App/`，`app_tasks.c` 放進 `Core/Src/`。重新整理專案（F5）。

### Step 4：修改 `sensor.c`（用 Ctrl+H 搜尋）

1. 檔案上方加：
   ```c
   #include "acc_gatt.h"
   ```
2. **Sensor_DeviceInit()** 中，找到新增原本 service 的呼叫（例如 `Add_HWServW2ST_Service()`、`Add_SWServW2ST_Service()`），**註解掉**，改成：
   ```c
   AccGatt_Init();
   ```
3. **裝置名稱**：搜尋 `AD_TYPE_COMPLETE_LOCAL_NAME`，把後面的字元改成 `'S','T','M','3','2','A','C','C'`（8 個字元以內，避免廣播封包超過 31 bytes）。若名稱來自 `BoardName` 之類的陣列，改那個陣列。
4. **hci_le_connection_complete_event(...)** 函式內加：
   ```c
   AccGatt_OnConnect(Connection_Handle);
   ```
5. **hci_disconnection_complete_event(...)** 函式內加：
   ```c
   AccGatt_OnDisconnect();
   ```
   這個函式原本就會設定 `set_connectable = TRUE`，TASK_BLE 會據此重新廣播，不用改。

### Step 5：修改 GATT write 事件（`gatt_db.c` 或 `sensor.c`）
搜尋 **`aci_gatt_attribute_modified_event`**，找到它的定義（不是宣告），把原本的內容**註解掉**（那是給舊 service 用的），改成：
```c
AccGatt_OnAttrModified(Attr_Handle, Attr_Data_Length, Attr_Data);
```
該檔案上方也要 `#include "acc_gatt.h"`。

> 參數名稱以你的檔案為準。如果函式還有 `Offset` 參數，忽略它即可。

### Step 6：讓 BLE 中斷喚醒 TASK_BLE（`hci_tl_interface.c`）
找到 **`hci_tl_lowlevel_isr()`**，在函式**結尾**（有 USER CODE 區塊就放在裡面）加：
```c
extern void App_BleIrqNotify(void);
App_BleIrqNotify();
```
沒加也能動，TASK_BLE 每 10 ms 會輪詢一次，只是延遲比較大。

### Step 7：確認 `main.c`
- **`MX_BlueNRG_MS_Process()`**：如果出現在任何會執行到的地方（例如 defaultTask），刪掉。BLE 事件改由 TASK_BLE 處理。
- **`MX_BlueNRG_MS_Init()`**：
  - 如果 CubeMX 把它放在 `osKernelStart()` 之前 → 保持不動，`app_tasks.c` 的 `BLE_INIT_IN_TASK` 維持 `0`。
  - 如果找不到這個呼叫 → 把 `BLE_INIT_IN_TASK` 改成 `1`。
- 如果 Phase 3 有在 main.c 寫加速度測試碼，刪掉。

### Step 8：Build 與常見錯誤

| 錯誤 | 處理 |
|---|---|
| `Service_UUID_t`、`CHAR_PROP_...` 等找不到 | 把 `gatt_db.c` 最上方的 `#include` 複製到 `acc_gatt.c` |
| `multiple definition of HAL_GPIO_EXTI_Callback` | 刪掉 `app_tasks.c` 裡那個函式，把其中的 `if` 區塊搬到既有的 `HAL_GPIO_EXTI_Callback` 內 |
| `undefined reference to set_connectable` / `Set_DeviceConnectable` | 在 `sensor.c` 確認實際名稱，同步修改 `app_tasks.c` 的 extern |
| `undefined reference to SENSOR_IO_Read` | Step 2 的 `stm32l475e_iot01.c` 沒加入編譯 |
| `undefined reference to TASK_BLEHandle` 等 | CubeMX 的 task 或 mutex 名稱和本文件不同，改成一致 |
| 開機後 HardFault | 檢查 NVIC 優先權（Step 1）與 task stack 大小 |

### 預期 UART log
```
[GATT] AccService ready: serv=0x000C acc=0x000D freq=0x0010 motion=0x0012
[BLE] TASK_BLE started
[GAP] advertising...
[ACC] TASK_ACC started, 10 Hz
[ACC] significant motion enabled (threshold=6 steps)
[GAP] connected, handle=0x0801
[GATT] ACC notify ON
[GATT] FREQ write: req=20 -> 20 Hz
[ACC] frequency -> 20 Hz
[ACC] set=20 Hz sent=20/s dropped=0/s xyz=(12,-8,1003) mg
```

---

## RPi 4 端

```bash
sudo apt update && sudo apt install -y bluez python3-venv
python3 -m venv ~/ble-env && source ~/ble-env/bin/activate
pip install bleak

python ble_client.py                 # 掃描名稱含 STM32 的裝置
python ble_client.py --freq 20       # 連線後設成 20 Hz
python ble_client.py --quiet         # 只顯示每秒接收筆數
python ble_client.py --csv acc.csv   # 存檔
```
執行中輸入數字再按 Enter 就能即時改頻率。拿起板子走幾步會印出 `*** Significant motion detected ***`。

如果掃不到：先用 `bluetoothctl` → `scan on` 確認 MAC，再用 `--addr XX:XX:XX:XX:XX:XX` 指定。

---

## 設計說明（報告可用）

- **TASK_BLE** 唯一負責 `hci_user_evt_proc()`，GAP 連線/斷線與 GATT write 事件都在這裡處理；斷線後依 `set_connectable` 重新廣播。
- **TASK_ACC** 以 `osKernelGetTickCount()` 計算絕對時間點，避免累積誤差；頻率變更會在下個週期生效，並同步調整 LSM6DSL ODR（≥ 2 倍取樣頻率，下限 26 Hz）。
- **bleMutex**：BlueNRG-MS 的 HCI 不是 thread-safe，所有 ACI 呼叫都在 mutex 內進行。
- **中斷只 set thread flag**：PE6（BLE）和 PD11（LSM6DSL INT1）的 ISR 不做 SPI/I2C 存取，實際處理在 task 中進行。
- **I2C 只在 TASK_ACC 存取**：motion 事件也交給 TASK_ACC 處理（`osThreadFlagsWait` 等待取樣時間或 motion 中斷），不需要額外的 I2C 鎖。
- **Significant motion**（LSM6DSL embedded function）：bank A 設 `SM_THS`、`CTRL10_C` 開 `FUNC_EN | SIGN_MOTION_EN`、`INT1_CTRL` 開 `INT1_SIGN_MOT`；事件後讀 `FUNC_SRC1` 清除。
- **頻率上限 100 Hz**：受 BLE 連線間隔與 BlueNRG-MS TX buffer 限制，TX buffer 滿時丟棄該筆並統計 dropped。
