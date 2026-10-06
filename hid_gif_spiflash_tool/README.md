# hid_gif_spiflash_tool

Windows 端 **USB HID** 工具，用來把 GIF（或任意二進位檔）透過 Nuvoton M3334KI 開發板燒錄到板上的 **外接 SPI NOR Flash**，並可回讀 / 驗證內容。

搭配的 MCU 韌體專案為 [`HID_SpiFlash_Writer`](../HID_SpiFlash_Writer)（Keil MDK）。PC 工具透過標準 HID 介面下命令，韌體再用 QSPI0（PC0~PC5）操作 SPI Flash。

---

## 1. 系統架構

```
┌──────────────────┐   USB HID    ┌─────────────────────┐   QSPI0 (PC0~PC5)   ┌──────────────┐
│  PC 工具          │ ───────────► │  M3334KI 韌體        │ ──────────────────► │  SPI NOR     │
│ hid_gif_spiflash │ ◄─────────── │ HID_SpiFlash_Writer │ ◄────────────────── │  Flash       │
│  _tool.exe       │   命令/資料   │ (hid_transfer.c +   │     讀 / 寫 / 抹除    │              │
└──────────────────┘              │  spi_flash.c)       │                     └──────────────┘
                                  └─────────────────────┘
```

- **USB VID/PID**：預設 `0x0416 / 0x5020`。
- **HID Page（傳輸頁）大小**：1024 bytes（韌體 `EP_MAX_PKT_SIZE == 1024`）。
- **Flash 抹除單位**：4 KB sector。
- **Flash 程式化單位**：256 bytes/page（韌體內部拆分，PC 端不需理會）。

---

## 2. 通訊協定

PC 與韌體之間以一個 20-byte 命令封包（`CMD_T`，1-byte 對齊）溝通：

| 欄位        | 型別      | 說明                                  |
| ----------- | --------- | ------------------------------------- |
| `cmd`       | u8        | 命令碼                                |
| `len`       | u8        | 封包長度（不含 checksum）             |
| `arg1`      | u32       | 起始 page / sector                    |
| `arg2`      | u32       | page / sector 數量                    |
| `signature` | u32       | 固定 `0x43444948`（"HIDC"）           |
| `checksum`  | u32       | 前面欄位的位元組總和                  |

命令碼：

| 名稱            | 值     | 動作                              |
| --------------- | ------ | --------------------------------- |
| `HID_CMD_ERASE` | `0x71` | 抹除 `arg2` 個 4 KB sector        |
| `HID_CMD_READ`  | `0xD2` | 讀取 `arg2` 個 page               |
| `HID_CMD_WRITE` | `0xC3` | 寫入 `arg2` 個 page               |
| `HID_CMD_TEST`  | `0xB4` | 連線 / 讀寫測試                   |

---

## 3. 程式流程（PC 工具）

主要原始碼：[`HIDTransferTest.cpp`](HIDTransferTest.cpp)

### 3.1 `main()`
1. 解析命令列參數（`-pid` / `-pagesize` / `-pages` / `-base` / `-verify` / `-program` / `-dump` / `-ctrl` …）。
2. 依模式分派：
   - **`-program <file>`** → 燒錄流程（見 3.2）
   - **`-dump <file>`** → 回讀流程（見 3.3）
   - 其餘 → 內建讀寫測試。

### 3.2 燒錄流程（`-program`）
```
開檔 → 取得檔案大小
      ↓
補齊 (pad 0xFF) 到整數個 HID page，計算所需 sector 數
      ↓
EraseSectors(base/4096, sectors)      // 送 HID_CMD_ERASE
      ↓
WritePages(buf, base/pagesize, pages) // 送 HID_CMD_WRITE + 連續資料封包
      ↓
(若 -verify) ReadPages() 回讀並與原始 buffer 逐 byte 比對
```

### 3.3 回讀流程（`-dump`）
```
ReadPages(buf, base/pagesize, pages)  // 送 HID_CMD_READ，收資料
      ↓
將收到的位元組寫入輸出檔
```

### 3.4 三個核心傳輸函式
- `EraseSectors(startSector, sectors)`：送出抹除命令。
- `WritePages(buf, startPage, pages)`：送出寫入命令，接著把資料以 HID 封包連續送出。
- `ReadPages(buf, startPage, pages)`：送出讀取命令，再以 256-byte 區塊持續 `ReadFile` 直到收滿 `pages × pagesize`。

每個函式都會各自 `OpenDevice` → 傳輸 → `CloseDevice`。

---

## 4. 如何編譯

### 需求
- **CMake** ≥ 3.15
- **Visual Studio 2022 Build Tools**（MSVC，含 Windows SDK）
- 連結的系統函式庫：`setupapi`、`hid`（已寫在 `CMakeLists.txt`）

> 注意：`CMakeLists.txt` 已定義 `UNICODE` / `_UNICODE`（`TCHAR == wchar_t`，`HID.hpp` 需要）以及 `_CRT_SECURE_NO_WARNINGS`。

### 步驟（PowerShell）
```powershell
cd "C:\Users\jcliu\Desktop\m3331 gif\hid_gif_spiflash_tool"
cmake -S . -B build
cmake --build build --config Release
```

產出執行檔：
```
build\Release\hid_gif_spiflash_tool.exe
```

---

## 5. 如何使用

### 命令列選項
| 選項                | 說明                                                   | 預設        |
| ------------------- | ------------------------------------------------------ | ----------- |
| `-pid <id>`         | USB Product ID（hex 或十進位）                         | `0x5020`    |
| `-pagesize <size>`  | HID page 大小（512 或 1024）                           | `1024`      |
| `-pages <count>`    | 讀取 / 測試的 page 數                                   | `1`         |
| `-base <addr>`      | 起始位址（hex 或十進位，程式化時需 4 KB 對齊）         | `0x00010000`|
| `-verify`           | 燒錄後做 blank check 與資料比對                        | 關閉        |
| `-program <file>`   | 把檔案燒錄到 `-base`                                    | —           |
| `-dump <file>`      | 從 `-base` 讀 `-pages` 頁存成檔案                      | —           |
| `-ctrl`             | 改用 HID Control Transfer                              | Interrupt   |
| `-v` / `-h`         | 顯示版本 / 說明                                         | —           |

### 範例

燒錄 GIF 到位址 0x0 並驗證：
```powershell
.\build\Release\hid_gif_spiflash_tool.exe -base 0x0 -program ..\dance.gif -verify
```

回讀前 8 頁（8 KB）存成檔案：
```powershell
.\build\Release\hid_gif_spiflash_tool.exe -base 0x0 -pages 8 -dump readback.bin
```

指定不同 PID：
```powershell
.\build\Release\hid_gif_spiflash_tool.exe -pid 0x5021 -base 0x20000 -program image.gif -verify
```

### 成功輸出範例
```
Programming '..\dance.gif' (2477900 bytes -> 2478080 bytes padded) to flash @ 0x00000000
>>> Erase sectors: 0 - 604
>>> Write pages: 0 - 2419
Performing data verify...
>>> Read pages: 0 - 2419
Data verify passed.

Programming completed!
```

---

## 6. 使用前提

1. 開發板已燒錄 [`HID_SpiFlash_Writer`](../HID_SpiFlash_Writer) 韌體。
2. 開發板以 **USB（HSUSBD）** 連上 PC，並被 Windows 列舉為 HID 裝置（VID `0x0416`）。
3. `-base` 在程式化時必須對齊 4 KB（sector 邊界）。
4. 韌體在初始化與每次抹除前會自動解除 SPI Flash 的寫入保護（Block Protect / SRP / CMP）。若硬體上的 `/WP` 腳位被常態拉低而鎖死狀態暫存器，則需從硬體解決。

---

## 7. 檔案說明

| 檔案                         | 說明                                   |
| ---------------------------- | -------------------------------------- |
| `HIDTransferTest.cpp`        | 主程式：參數解析、燒錄/回讀/測試流程   |
| `HID.hpp`                    | HID 裝置開啟 / 讀寫封裝（`CHidCmd`）    |
| `stdafx.cpp` / `stdafx.h`    | 預先編譯標頭                           |
| `version.h`                  | 版本資訊                               |
| `hidsdi.h` / `hidpi.h` / `hidusage.h` | Windows HID API 標頭          |
| `CMakeLists.txt`             | CMake 建置設定                         |
