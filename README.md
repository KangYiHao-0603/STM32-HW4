# STM32 BLE Accelerometer — GATT Server (B-L475E-IOT01A) + GATT Client (Raspberry Pi 4)

STM32 IoT node 作為 BLE **Peripheral / GATT Server**，以 FreeRTOS 的兩個 task 讀取 LSM6DSL 的**真實**三軸加速度並 notify；Raspberry Pi 4 作為 **Central / GATT Client**（Python + bleak），可訂閱加速度並寫入取樣頻率。
加分題實作 **option (b)**：LSM6DSL significant motion detection，經 GPIO EXTI 觸發後 notify RPi。



---

## 硬體

| 項目 | 型號 / 介面 |
|---|---|
| MCU 板 | B-L475E-IOT01A（STM32L475VG, Cortex-M4F） |
| BLE 模組 | SPBTLE-RF（BlueNRG-MS, BLE 4.1）— SPI3, CS=PD13, IRQ=PE6, RST=PA8 |
| 加速度計 | LSM6DSL — I2C2（PB10/PB11），INT1=PD11 |
| Debug UART | USART1（PB6/PB7）→ ST-Link VCP, 115200 |
| Client | Raspberry Pi 4, Raspberry Pi OS 64-bit, BlueZ |

## 系統架構

```
STM32 (GATT Server)                                   RPi 4 (GATT Client)
┌────────────────────────────────────┐               ┌─────────────────────────┐
│ TASK_BLE                           │               │ ble_client.py (bleak)   │
│  hci_user_evt_proc()               │               │  scan → connect         │
│  GAP connect / disconnect          │   BLE 4.1     │  notify: ACC, MOTION    │
│  GATT write (FREQ) / CCCD          │◄─────────────►│  write : FREQ           │
│  re-advertise after disconnect     │               │  auto reconnect         │
├────────────────────────────────────┤               └─────────────────────────┘
│ TASK_ACC                           │
│  每 1/freq 秒讀 LSM6DSL → notify ACC │
│  INT1 (significant motion) → MOTION │
└────────────────────────────────────┘
  PE6 ISR / PD11 ISR 只 set thread flag；所有 ACI 呼叫以 bleMutex 保護
```

## GATT Service

| Characteristic | UUID | 屬性 | 格式 |
|---|---|---|---|
| Service | `7a1e0000-3c2b-4d5e-9f10-a1b2c3d4e5f6` | | |
| ACC | `7a1e0001-3c2b-4d5e-9f10-a1b2c3d4e5f6` | Notify, Read | 6 B：int16 x, y, z（mg, little-endian） |
| FREQ | `7a1e0002-3c2b-4d5e-9f10-a1b2c3d4e5f6` | Write, Write w/o resp, Read | 2 B：uint16 Hz（1–100，預設 10） |
| MOTION | `7a1e0003-3c2b-4d5e-9f10-a1b2c3d4e5f6` | Notify, Read | 2 B：uint16 事件計數 |

廣播名稱：`STM32ACC`

## 設計重點

- **TASK_BLE**：唯一呼叫 `hci_user_evt_proc()` 的 task，負責 GAP 連線/斷線與 GATT write 事件；由 PE6 IRQ 以 thread flag 喚醒（10 ms timeout 保底），斷線後重新廣播。
- **TASK_ACC**：以 tick 計算絕對取樣時間點，避免累積誤差；FREQ 變更於下一週期生效，並同步調整 LSM6DSL ODR（≥ 2×取樣頻率，下限 26 Hz）。
- **執行緒安全**：BlueNRG-MS HCI 非 thread-safe，所有 ACI 呼叫都在 `bleMutex` 內；ISR 不做 SPI/I2C 存取；I2C 只由 TASK_ACC 存取。
- **Significant motion**：設定 embedded function bank A 的 `SM_THS`、`CTRL10_C`（FUNC_EN, SIGN_MOTION_EN）、`INT1_CTRL`（INT1_SIGN_MOT），事件後讀 `FUNC_SRC1` 清除。
- **頻率上限 100 Hz**：受 BLE 連線間隔與 BlueNRG-MS TX buffer 限制；buffer 滿時丟棄該筆並統計。

## 專案結構

```
firmware/                       STM32CubeIDE 專案
├─ BlueNRG_MS/App/acc_gatt.c/.h 自訂 GATT service
├─ Core/Src/app_tasks.c         TASK_BLE / TASK_ACC / significant motion
├─ Core/Src/sensor_io.c         LSM6DSL I2C2 存取
├─ Drivers/BSP/                 LSM6DSL BSP（STM32CubeL4）
└─ *.ioc                        CubeMX 設定
rpi/
├─ ble_client.py                GATT client
└─ requirements.txt
docs/                           測試截圖與紀錄
```

## 建置與執行

**STM32**：以 STM32CubeIDE 匯入 `firmware/`（File → Import → Existing Projects into Workspace），Build 後燒錄。

**Raspberry Pi 4**：
```bash
sudo apt install -y bluez python3-venv
python3 -m venv ~/ble-env && source ~/ble-env/bin/activate
pip install -r rpi/requirements.txt

python rpi/ble_client.py --freq 20      # 連線並設為 20 Hz
python rpi/ble_client.py --quiet        # 只顯示接收速率
python rpi/ble_client.py --csv acc.csv  # 存檔
```
執行中輸入數字 + Enter 可即時修改取樣頻率。

---

## 測試結果

> TODO：取得燒錄線後補上

### 1. 連線與 GATT 服務（nRF Connect）
<!-- docs/nrf_connect_service.png -->

### 2. RPi 接收加速度數據
<!-- 貼上終端機輸出 -->

### 3. 取樣頻率設定

| 設定頻率 (Hz) | RPi 實際接收 (samples/s) | 丟棄 (dropped/s) |
|---|---|---|
| 1 | | |
| 10 | | |
| 50 | | |
| 100 | | |

### 4. Significant motion（option b）
<!-- 靜置不觸發、拿起走動觸發的紀錄 -->

### 5. Demo 影片
<!-- 連結 -->
