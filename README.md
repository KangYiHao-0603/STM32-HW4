# STM32-HW4：BLE Accelerometer GATT Server (B-L475E-IOT01A) + Raspberry Pi Client

STM32 IoT node 作為 BLE **Peripheral / GATT Server**，以 FreeRTOS 的兩個 task（`TASK_BLE`、`TASK_ACC`）讀取 LSM6DSL 的**真實**三軸加速度並 notify；Raspberry Pi 作為 **Central / GATT Client**（Python + bleak），訂閱加速度並可寫入取樣頻率。

加分題實作 **Option problem_1 (b)**：初始化 LSM6DSL 的 **significant motion detection**，偵測到時 LSM6DSL 經 INT1（PD11）以 GPIO EXTI 中斷通知 STM32，STM32 再透過 GATT notify 通知 RPi。

> **狀態**：已完成並實機驗證（STM32 以 nRF Connect 驗證；RPi 驗證連線、即時數據、頻率設定與 significant motion）。

---

## 硬體

| 項目 | 型號 / 介面 |
|---|---|
| MCU 板 | B-L475E-IOT01A（STM32L475VG, Cortex-M4F, 80 MHz） |
| BLE 模組 | SPBTLE-RF（BlueNRG-MS, BLE 4.1）— SPI3, CS=PD13, IRQ=PE6, RST=PA8 |
| 加速度計 | LSM6DSL — I2C2（PB10/PB11, 100 kHz），INT1=PD11 |
| Debug UART | USART1（PB6/PB7）→ ST-Link VCP, 115200 8N1 |
| Client | Raspberry Pi 3 Model B（Raspberry Pi OS 64-bit, BlueZ）；Pi 4 亦可 |

## 軟體環境

| 項目 | 版本 |
|---|---|
| STM32CubeIDE | 2.2.0 |
| STM32CubeMX | 6.18.1 |
| STM32Cube FW_L4 | V1.18.2 |
| X-CUBE-BLE1 | 7.1.0（基底：`SensorDemo_BLESensor-App`） |
| RTOS | FreeRTOS（CMSIS-RTOS v2），timebase TIM6 |
| Client | Python 3 + bleak |

## 系統架構

```
STM32 (GATT Server)                                   Raspberry Pi (GATT Client)
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
| (a) ACC | `7a1e0001-3c2b-4d5e-9f10-a1b2c3d4e5f6` | Notify, Read | 6 B：int16 x, y, z（mg, little-endian） |
| (b) FREQ | `7a1e0002-3c2b-4d5e-9f10-a1b2c3d4e5f6` | Write, Write w/o resp, Read | 2 B：uint16 Hz（1–100，預設 10） |
| MOTION（Option problem_1 (b)） | `7a1e0003-3c2b-4d5e-9f10-a1b2c3d4e5f6` | Notify, Read | 2 B：uint16 事件計數 |

- 廣播名稱：`STM32ACC`（連線後的 GAP Device Name 仍為 SensorDemo 預設的 `BlueNRG`，不影響功能）
- 實機 attribute handle：`serv=0x000C acc=0x000D freq=0x0010 motion=0x0012`

## 設計重點

- **TASK_BLE**：唯一呼叫 `hci_user_evt_proc()` 的 task，負責 GAP 連線/斷線與 GATT write 事件；由 PE6 IRQ 以 thread flag 喚醒（10 ms timeout 保底），斷線後依 `set_connectable` 重新廣播。
- **TASK_ACC**：以 tick 計算絕對取樣時間點，避免累積誤差；FREQ 變更於下一週期生效，並同步調整 LSM6DSL ODR（≥ 2×取樣頻率，下限 26 Hz）。
- **執行緒安全**：BlueNRG-MS HCI 非 thread-safe，所有 ACI 呼叫都在 `bleMutex` 內；ISR 只 set thread flag；I2C 只由 TASK_ACC 存取，不需額外的鎖。
- **Significant motion（Option problem_1 (b)）**：embedded function bank A 設 `SM_THS`（6 步），`CTRL10_C` 開 `FUNC_EN | SIGN_MOTION_EN`，`INT1_CTRL` 開 `INT1_SIGN_MOT`。PD11 EXTI 喚醒 TASK_ACC，讀 `FUNC_SRC1` 清除事件後 notify MOTION。
- **頻率上限 100 Hz**：受 BLE 連線間隔與 BlueNRG-MS TX buffer 限制；buffer 滿時丟棄該筆並統計 `dropped`。

## 專案結構

```
firmware/                         STM32CubeIDE 專案（函式庫已複製進專案，不依賴外部 Repository）
├─ BlueNRG_MS/App/acc_gatt.c/.h   自訂 GATT service
├─ BlueNRG_MS/App/sensor.c/.h     GAP/GATT 事件分派（ACC_BLE_PATCH）
├─ BlueNRG_MS/App/gatt_db.c       停用 SensorDemo 原本的 service（ACC_BLE_PATCH）
├─ Core/Src/app_tasks.c           TASK_BLE / TASK_ACC / significant motion
├─ Core/Src/sensor_io.c           LSM6DSL 用的 I2C2 存取（暫存器層級）
├─ Drivers/BSP/                   LSM6DSL BSP（STM32CubeL4）
└─ projects.ioc                   CubeMX 設定
rpi/
├─ ble_client.py                  GATT client
└─ requirements.txt
tools/
└─ fake_stm32_server.py           在筆電模擬 STM32 GATT server（測試 client 用，資料為模擬值）
docs/                             測試截圖與紀錄
```

> ⚠️ `sensor.c`、`sensor.h`、`gatt_db.c` 沒有 USER CODE 區塊，CubeMX 重新 Generate 會覆蓋其中標記 `/* ACC_BLE_PATCH */` 的修改。Generate 後請執行：
> ```bash
> git checkout -- firmware/BlueNRG_MS/App/sensor.c firmware/BlueNRG_MS/App/sensor.h firmware/BlueNRG_MS/App/gatt_db.c
> ```

## 執行步驟（每次測試照這個順序）

本板 BLE MAC：`C3:AD:7D:EB:78:98`（廣播名稱 `STM32ACC`）

### Step 1：STM32 燒錄
1. STM32CubeIDE 開啟 `firmware/` 專案
2. **Project → Clean → Build**，Console 最後出現 `Finished building target: projects.elf`
3. 按 **Run**（綠色播放鍵）燒錄

### Step 2：開啟 STM32 的 UART log
Tera Term → Setup → Serial port → **STMicroelectronics STLink Virtual COM Port**，**115200** 8N1，按板子黑色 RESET 鍵，應看到：
```
[GATT] AccService ready: serv=0x000C ...
[BLE] TASK_BLE started
[GAP] advertising...
[ACC] TASK_ACC started, 10 Hz
```

### Step 3：RPi 執行 client
> 手機的 nRF Connect 要先斷線，BlueNRG 同時只能連一個裝置。

```bash
cd ~/STM32-HW4
source ~/ble-env/bin/activate
python rpi/ble_client.py --addr C3:AD:7D:EB:78:98 --freq 20
```

### Step 4：測試操作（client 執行中）
| 操作 | 預期結果 |
|---|---|
| 翻轉、晃動板子 | x / y / z 跟著改變，平放 z ≈ 1000 mg |
| 輸入 `10` + Enter | `[input] 10` → `STM32 now = 10 Hz`，`[rate]` 約 10 |
| 輸入 `5` + Enter | `[rate]` 約 5 |
| 拿著板子走 10 步以上 | `*** Significant motion detected (#1) ***` |
| 結束 | **Ctrl + C** |

### 常用指令
```bash
# 存一份 log（-u 讓輸出不被緩衝）
python -u rpi/ble_client.py --addr C3:AD:7D:EB:78:98 --freq 20 | tee ~/rpi_log.txt

# 只看接收速率
python rpi/ble_client.py --addr C3:AD:7D:EB:78:98 --freq 20 --quiet

# 存成 CSV
python rpi/ble_client.py --addr C3:AD:7D:EB:78:98 --freq 20 --csv ~/acc.csv

# 重新掃描 MAC（看到 STM32ACC 後輸入 scan off、exit）
bluetoothctl
#   scan on

# 更新程式
cd ~/STM32-HW4 && git pull

# 藍牙異常時
sudo rfkill unblock bluetooth
sudo systemctl restart hciuart bluetooth
```
- 數據預設每秒最多印 5 筆避免洗版（`--print-hz N` 調整、`--all` 全部印出）；CSV 記錄每一筆
- 啟用 SSH：`sudo systemctl enable --now ssh`，查 IP：`hostname -I`

### RPi 第一次安裝（只需做一次）
```bash
sudo apt update
sudo apt install -y bluez python3-venv git
cd ~
git clone https://github.com/KangYiHao-0603/STM32-HW4.git
python3 -m venv ~/ble-env
source ~/ble-env/bin/activate
pip install -r ~/STM32-HW4/rpi/requirements.txt
```

---

## 測試結果

### 1. STM32 開機與 GATT 服務

```
[GATT] AccService ready: serv=0x000C acc=0x000D freq=0x0010 motion=0x0012
[BLE] TASK_BLE started
[ACC] significant motion enabled (threshold=6 steps)
[ACC] TASK_ACC started, 10 Hz
[GAP] advertising...
[GAP] connected, handle=0x0801
[GATT] ACC notify ON
[ACC] set=10 Hz sent=10/s dropped=0/s xyz=(17,59,998) mg
```
連續執行 2.5 分鐘以上無重新開機；ACC notify 開關（CCCD）切換後，送出筆數即時對應改變。
<!-- docs/nrf_connect_service.png -->

### 2. 真實加速度數據（LSM6DSL）

翻轉板子到六個方向，每個姿勢只有一軸接近 ±1000 mg（重力），確認為感測器實際量測值：

| 姿勢 | x (mg) | y (mg) | z (mg) |
|---|---|---|---|
| 平放（正面朝上） | 17 | 59 | **998** |
| 翻面（正面朝下） | 183 | -48 | **-988** |
| 側立 1 | **1012** | -92 | 51 |
| 側立 2 | 56 | **-1002** | -82 |
| 側立 3 | 27 | **960** | 247 |

劇烈晃動時讀值在 ±2000 mg 附近飽和，原因是 BSP 預設量測範圍為 ±2g。

### 3. RPi 設定取樣頻率

RPi 寫入 FREQ 後讀回確認（`[freq] requested 10 Hz, STM32 now = 10 Hz`），STM32 下一週期即改變送出速率：

| 設定頻率 (Hz) | RPi 實際接收 (samples/s) | STM32 dropped/s |
|---|---|---|
| 5 | 4.9 – 5.1 | 0 |
| 10 | 9.8 – 10.3 | 0 |
| 20 | 19.5 – 19.6 | 0 |

說明：
- 接收速率在設定值 ±1 Hz 內波動。BLE 依連線間隔批次傳送 notify，每秒統計時落在邊界的資料可能被算到相鄰的一秒，長時間平均與設定值一致。
- 改變頻率後的第一個統計值同時包含新舊頻率的資料，屬於過渡值。
- 劇烈晃動時速率仍維持 19.5 – 19.6 samples/s（20 Hz 設定）。

### 4. Option problem_1 (b)：Significant motion detection

拿著板子行走，LSM6DSL 偵測到 significant motion 後經 INT1（PD11）觸發 EXTI，STM32 notify MOTION，RPi 顯示：
```
*** Significant motion detected (#1) ***
*** Significant motion detected (#2) ***
```
可連續觸發，第一次事件後不需重設計步器即可再次偵測。
<!-- docs/rpi_significant_motion.png -->

### 5. Demo 影片
<!-- 連結 -->

---

## 開發中遇到的問題

| 問題 | 原因 | 解法 |
|---|---|---|
| 燒錄後不斷重新開機 | TASK_BLE stack 僅 256 words；`TOTAL_HEAP_SIZE` 僅 3000 bytes，加大 stack 後 task 建立失敗 | TASK_BLE 1024 words、TASK_ACC 512 words、heap 20000 bytes |
| BLE 中斷呼叫 RTOS API 會卡死 | `hci_tl_lowlevel_init()` 把 EXTI9_5 優先權設為 0，高於 FreeRTOS 允許的範圍 | 在其 USER CODE 區塊改回 5 |
| 廣播名稱變回 BlueNRG、事件沒有傳到 GATT service | CubeMX Generate 覆蓋了沒有 USER CODE 區塊的 `sensor.c`、`gatt_db.c`、`sensor.h` | 重新套用並標記 `ACC_BLE_PATCH`，commit 後可用 git 還原 |
| `stm32l475e_iot01.c` 與 `custom.c` 重複定義 | 兩者都定義 `BSP_LED/PB/COM` | 不使用 `stm32l475e_iot01.c`，另寫 `sensor_io.c` 提供 `SENSOR_IO_xxx` |
| `Cannot run program "make"` | Windows 智慧型應用程式控制（Smart App Control）封鎖未簽章的 `make.exe` | 關閉 Smart App Control |
| 專案搬移後找不到函式庫 | CubeMX 以相對路徑參照外部 Repository | 改為「Copy only the necessary library files」，並移除殘留的連結 |