# HID_SpiFlash_Writer

Nuvoton **M3334KI**（Cortex-M33）韌體：透過 **USB HID（HSUSBD，High-Speed）** 接收 PC 端命令，把資料（例如 GIF 動畫）寫入板上外接的 **SPI NOR Flash**，並支援回讀與抹除。

搭配的 PC 端工具為 [`hid_gif_spiflash_tool`](../hid_gif_spiflash_tool)。

---

## 1. 硬體與腳位

| 項目            | 設定                                              |
| --------------- | ------------------------------------------------- |
| MCU             | M3334KIGAE（Cortex-M33 @ 180 MHz）                |
| USB             | HSUSBD（High-Speed USB Device）                   |
| SPI Flash 介面  | QSPI0，single-line 模式                           |
| Debug UART      | UART0（PB12 =RXD、PB13 =TXD），115200-8n1         |

QSPI0 腳位（PC0~PC5）：

| 腳位 | 功能            |
| ---- | --------------- |
| PC0  | QSPI0_MOSI0     |
| PC1  | QSPI0_MISO0     |
| PC2  | QSPI0_CLK       |
| PC3  | QSPI0_SS (/CS)  |
| PC4  | QSPI0_MOSI1(D2) |
| PC5  | QSPI0_MISO1(D3) |

> 預設只使用單線（MOSI0/MISO0）傳輸；D2/D3 保留，未啟用 Quad 模式。

---

## 2. 原始碼結構

| 檔案                | 說明                                                              |
| ------------------- | ----------------------------------------------------------------- |
| `main.c`            | `SYS_Init()` 時脈/腳位初始化；`main()` 初始化 Flash、HSUSBD 並啟動 |
| `hid_transfer.c`    | HID 協定核心：命令解析、讀/寫/抹除資料階段處理、USB 中斷           |
| `hid_transfer.h`    | 端點、封包大小（`EP_MAX_PKT_SIZE`）、命令定義                     |
| `descriptors.c`     | USB 裝置 / 設定 / HID 描述元                                       |
| `spi_flash.c`       | SPI NOR Flash 驅動（QSPI0）：JEDEC ID、讀/寫/抹除、解除寫入保護    |
| `spi_flash.h`       | Flash 幾何常數與 API 宣告                                          |
| `Keil/`             | Keil MDK 專案（`HID_SpiFlash_Writer.uvprojx`）與建置輸出          |

---

## 3. 程式流程

### 3.1 開機初始化（`main.c`）
```
SYS_Init()                 // 設定 180 MHz 核心時脈、UART0、HSUSBD PHY、QSPI0、PC0~PC5 腳位
UART_Open(UART0, 115200)   // 除錯訊息輸出
SpiFlash_Init()            // QSPI0 Master / Mode-0 / 48 MHz，手動控制 /CS
SpiFlash_ReadJedecID()     // 印出 Flash JEDEC ID 確認通訊正常
SpiFlash_ClearWriteProtect() // 解除 Block Protect / SRP / CMP 寫入保護
HSUSBD_Open() + HID_Init() // 設定 HID class、端點
HSUSBD_Start()             // 開始列舉，進入主迴圈等待命令
```

### 3.2 命令協定
PC 以 20-byte 命令封包（`CMD_T`）下指令：

| 欄位        | 型別 | 說明                              |
| ----------- | ---- | --------------------------------- |
| `u8Cmd`     | u8   | 命令碼                            |
| `u8Size`    | u8   | 封包長度（不含 checksum）         |
| `u32Arg1`   | u32  | 起始 page / sector                |
| `u32Arg2`   | u32  | page / sector 數                  |
| `u32Signature` | u32 | `0x43444948`（"HIDC"），資料階段兼作計數器 |
| `u32Checksum`  | u32 | 檢查碼                         |

命令碼：

| 名稱            | 值     | 動作                      |
| --------------- | ------ | ------------------------- |
| `HID_CMD_ERASE` | `0x71` | 抹除 N 個 4 KB sector     |
| `HID_CMD_READ`  | `0xD2` | 回讀 N 個 page            |
| `HID_CMD_WRITE` | `0xC3` | 寫入 N 個 page            |
| `HID_CMD_TEST`  | `0xB4` | 連線測試                  |

### 3.3 寫入流程（Host → Device）
`HID_GetOutReport()`（EPB，Interrupt OUT）：
1. 第一包為命令封包 → `ProcessCommand()` 解析為 `HID_CMD_WRITE`。
2. 後續資料包累積到 `g_u8PageBuff`，滿一個 `PAGE_SIZE`（1024）後：
   - 以 256-byte 為單位呼叫 `SpiFlash_PageProgram256()` 寫入 Flash。
   - page 計數 +1，直到寫完所有 page。

### 3.4 抹除流程
`HID_CmdEraseSectors()`：
1. 先呼叫 `SpiFlash_ClearWriteProtect()` 確保未鎖定。
2. 對每個 sector 呼叫 `SpiFlash_SectorErase4K()`（命令 `0x20`）。

### 3.5 回讀流程（Device → Host）
`HID_CmdReadPages()` 送出第一包，之後由 `HID_SetInReport()`（EPA，Interrupt IN）接力：
1. 每頁用 `SpiFlash_Read()`（命令 `0x03`）讀入 `g_u8PageBuff`。
2. 以 `g_u8PageBuff[PAGE_SIZE - g_u32BytesInPageBuf + i]` 連續送出，直到所有 page 傳完。

### 3.6 SPI Flash 驅動（`spi_flash.c`）
| API                          | SPI 命令 | 說明                                 |
| ---------------------------- | -------- | ------------------------------------ |
| `SpiFlash_ReadJedecID()`     | `0x9F`   | 讀 24-bit JEDEC ID                   |
| `SpiFlash_SectorErase4K()`   | `0x20`   | 抹除 4 KB sector（先 WREN）          |
| `SpiFlash_ChipErase()`       | `0xC7`   | 全片抹除                             |
| `SpiFlash_PageProgram256()`  | `0x02`   | 程式化最多 256 bytes（先 WREN）      |
| `SpiFlash_Read()`            | `0x03`   | 標準讀取                             |
| `SpiFlash_ClearWriteProtect()` | `0x05`/`0x35`/`0x01` | 讀 SR1/SR2，清除 BP/TB/SEC/SRP/CMP 後回寫 |

---

## 4. 如何編譯

### 需求
- **Keil MDK**（ARM Compiler 6 / ARMCLANG），已安裝 **Nuvoton M3331/M3334 Device Family Pack**。
- Nu-Link（或相容）除錯器用於下載。

### 使用 Keil IDE
1. 開啟 [`Keil/HID_SpiFlash_Writer.uvprojx`](Keil/HID_SpiFlash_Writer.uvprojx)。
2. **Project → Build Target**（F7）。
3. 產出：`Keil/obj/HID_SpiFlash_Writer.axf` 及 `Keil/obj/HID_SpiFlash_Writer.bin`。

### 命令列（PowerShell）
```powershell
# 編譯（rebuild）
& "C:\Keil_v5\UV4\UV4.exe" -j0 -b "C:\Users\jcliu\Desktop\m3331 gif\HID_SpiFlash_Writer\Keil\HID_SpiFlash_Writer.uvprojx" -o build.log

# 下載到板子（透過已設定的 Nu-Link）
& "C:\Keil_v5\UV4\UV4.exe" -j0 -f "C:\Users\jcliu\Desktop\m3331 gif\HID_SpiFlash_Writer\Keil\HID_SpiFlash_Writer.uvprojx"
```
> `UV4.exe` 回傳 `ExitCode=0` 代表成功。編譯訊息會寫入 `-o` 指定的 log。

---

## 5. 如何使用

1. 用 Keil + Nu-Link 將韌體下載到 M3334KI 開發板。
2. 開啟 UART0（115200-8n1）可看到開機訊息與 JEDEC ID：
   ```
   NuMicro HSUSBD HID -> SPI NOR Flash Writer
   SPI Flash JEDEC ID: 0xEF4018
   ```
3. 以 USB 線連接開發板的 **HSUSBD** 埠到 PC；Windows 會把它列舉為 HID 裝置（VID `0x0416`、PID `0x5020`）。
4. 在 PC 端用 [`hid_gif_spiflash_tool`](../hid_gif_spiflash_tool) 下命令，例如燒錄 GIF：
   ```powershell
   hid_gif_spiflash_tool.exe -base 0x0 -program dance.gif -verify
   ```

執行時 UART 會同步印出 `Writing page N` / `Reading page N` 等進度訊息，可用於除錯。

---

## 6. 重要參數

| 巨集                           | 檔案            | 預設   | 說明                          |
| ------------------------------ | --------------- | ------ | ----------------------------- |
| `EP_MAX_PKT_SIZE`              | `hid_transfer.h`| 1024   | HID 端點 / page 大小（1024 或 512） |
| `FLASH_QSPI_CLK_FREQ`          | `spi_flash.h`   | 48 MHz | QSPI0 時脈                    |
| `FLASH_PAGE_SIZE`              | `spi_flash.h`   | 256    | Flash 程式化頁大小            |
| `FLASH_SECTOR_SIZE`            | `spi_flash.h`   | 4096   | Flash 抹除 sector 大小        |

---

## 7. 注意事項

- 韌體在開機與每次抹除前都會自動呼叫 `SpiFlash_ClearWriteProtect()`。若 Flash 的 `/WP` 腳位被硬體常態拉低而鎖死狀態暫存器，則需從硬體解決，韌體無法單靠軟體解鎖。
- 寫入前必須先抹除對應 sector（PC 工具的 `-program` 會自動處理）。
- `PAGE_SIZE` 必須與 PC 工具的 `-pagesize` 一致（預設皆為 1024）。
