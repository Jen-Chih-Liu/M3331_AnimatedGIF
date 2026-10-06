# GIF Decode Player（原始 / 通用版）

Nuvoton **M3334KI（Cortex-M33 @ 180 MHz）** 的串流 GIF 播放器。GIF 原始檔預先燒錄
在板上外接 SPI NOR Flash，韌體以 QSPI0 讀入、用 [AnimatedGIF](https://github.com/bitbank2/AnimatedGIF)
library 做 LZW 解碼，並把 320×240 RGB565 影格透過 SPI1 串流到 ILI9341 TFT 面板。

```
SPI flash --(QSPI0 Quad I/O, RX PDMA ch1, 2KB cache)--> LZW 解碼（逐行）
          --(COOKED RGB565，由 library 合成整張畫布)--> g_cooked（320x240）
          --(SPI1, TX PDMA ch0，整幀雙半段)-----------> ILI9341 TFT
```

這是 **serial（序列）版本**：先完整解碼一幀到 library 自己的 cooked 畫布，再整幀
送往面板，完成後才解碼下一幀。流程單純、相容性最高。

---

## 與 Global Palette 版的差異

本資料夾（`GIF_Decode_Player org`）是**原始、通用**的實作；
相鄰的 `GIF_Decode_Player gobal palette` 則是追求高 FPS 的管線化特化版。

| 面向 | 本版（org） | global palette 版 |
|------|-------------|-------------------|
| 解碼 / 傳輸 | **序列**（解碼完整幀 → 傳輸 → 下一幀） | **管線化重疊**（解碼 N+1 幀時同時傳輸 N 幀） |
| cook（索引→RGB565） | 由 library 在 `playFrame()` 內直接合成 cooked 畫布 | 延後由播放器對整張索引畫布查表 |
| 調色盤前提 | **無限制**，任何 GIF 皆可 | 假設單一全域調色盤 |
| 區域調色盤 / 各種 disposal / interlace | **全部正確** | 區域調色盤可能顏色錯誤 |
| FPS | 較低（`解碼 + 傳輸` 相加） | 較高（`max(解碼, 傳輸)`） |

> **若你的 GIF 含區域調色盤、或不確定素材特性，請用本版**，它直接採用 AnimatedGIF
> library 合成好的畫布，正確處理幀間透明、disposal（restore-to-background /
> restore-to-previous）與交錯（interlace）。

---

## 1. 硬體需求

| 項目 | 說明 |
|------|------|
| MCU  | Nuvoton M3334KIGAE（Cortex-M33，512 KB Flash / 384 KB SRAM） |
| 顯示 | ILI9341 320×240 TFT（SPI 介面） |
| 儲存 | 外接 SPI NOR Flash（容量需 ≥ GIF 檔案大小，`dance.gif` 約 2.36 MB） |
| 偵錯 | Nu-Link / Nu-Link2（SWD） |
| 序列 | UART0，115200 8N1（開機訊息、FPS/CPU 統計） |

### 腳位對應

| 功能 | 接腳 | 備註 |
|------|------|------|
| QSPI0（Flash） | PC0~PC5 | 與 Display_Animation 範例相同接法 |
| SPI1 SCK/MOSI（LCD） | 由 `SYS_Init()` 設定 | 48 MHz（見 `lcd.h`） |
| LCD RST | PD14 | `lcd.h` → `LCD_RST` |
| LCD DC  | PH10 | `lcd.h` → `LCD_DC` |
| LCD EN（背光） | PH11 | `lcd.h` → `LCD_EN` |

---

## 2. 建置與燒錄（Keil MDK）

1. 用 Keil MDK（ARM Compiler 6 / armclang）開啟
   [KEIL/GIF_Decode_Player.uvprojx](KEIL/GIF_Decode_Player.uvprojx)。
2. 需安裝 **Nuvoton NuMicro M33 DFP** Device Family Pack。
3. 連接 Nu-Link，**Build（F7）** 後 **Download（F8）** 把韌體燒進 MCU 內部 Flash。

| 設定 | 值 |
|------|----|
| Device | `M3334KIGAE` |
| Flash 演算法 | `m3331_AP_1M.FLM`（1 MB 內部 Flash） |
| Scatter file | [KEIL/m3331_gif.sct](KEIL/m3331_gif.sct)（SRAM 0x60000、Stack 0x8000） |
| C++ 來源 | `gif_player.cpp`、`AnimatedGIF/AnimatedGIF.cpp`（需用標準 C 函式庫，`useUlib=0`） |
| 預先 include | `-include ../gif_compat.h`（提供 `millis()`/`delay()`） |
| Include 路徑 | `..\AnimatedGIF` + BSP 的 Device/CMSIS/StdDriver |

> **注意**：Stack 需 32 KB（AnimatedGIF LZW 解碼用量大）。若自行調整 scatter，
> 務必保留足夠 Stack，否則解碼時會 Stack overflow（HardFault，CFSR bit20 STKOF）。

---

## 3. 把 GIF 燒進 SPI Flash

韌體只負責「讀取 + 解碼 + 顯示」，GIF 原始位元組必須事先寫入外接 SPI Flash 的
位址 `0x0`（見 `qspi_flash.h` 的 `GIF_FLASH_BASE_ADDR`）。使用同倉庫的
USB HID 工具完成燒錄：

- PC 端工具：[`hid_gif_spiflash_tool`](../../../../hid_gif_spiflash_tool)
- 搭配 MCU 韌體：`HID_SpiFlash_Writer`

寫入後，把 GIF 的實際位元組大小填到 [gif_player.h](gif_player.h) 的 `GIF_FILE_SIZE`
（目前預設 `2477900`）。填入大於實際檔案的值仍可運作——播放器會偵測 GIF trailer
並循環播放。

---

## 4. 設計細節：Frame-buffer-only COOKED 模式

播放器**不安裝 GIFDRAW callback**（`gif.open(...)` 第 6 個參數為 `NULL`）。此模式下
AnimatedGIF library 會把每一幀合成進它自己的畫布：

- `g_libFB` 單一緩衝區，佈局如下：
  - `[0 .. W*H-1]`：8-bit 調色盤索引畫布（library 內部狀態）
  - `[W*H .. ]`：RGB565 cooked 畫布（即 `g_cooked`，我們直接送往面板）
  - 尾端多留幾行 scratch，並 4-byte 對齊讓 RGB565 區段保持 halfword 對齊。

因為整張畫布永遠是一幀完整、最新的影像，每次 `playFrame()` 後直接把 `g_cooked`
blit 到面板即可，透明與 disposal 皆由 library 正確維護。

---

## 5. 記憶體配置

| 緩衝區 | 大小 | 說明 |
|--------|------|------|
| `g_libFB` 索引區 | ≈ 76.8 KB | 8-bit 索引畫布（library 狀態） |
| `g_libFB` cooked 區（`g_cooked`） | ≈ 150 KB | RGB565 畫布（送往面板） |
| GIFIMAGE | ≈ 24 KB | AnimatedGIF 內部狀態 |
| Stack | 32 KB | LZW 解碼需求 |

合計約 250 KB，落在 384 KB SRAM 預算內。

---

## 6. 執行流程（非阻塞狀態機）

1. `SYS_Init()`：核心時脈 180 MHz、QSPI0 / SPI1 腳位與模組時脈、UART0 115200。
2. `QSPI_Flash_Init()` → `SPI_LCD_Init()` → `LCD_Init()`。
3. `GIF_Player_Init()`：重置狀態機、清畫面。
4. 主迴圈反覆呼叫 `GIF_Player_Task()`：

   ```
   ST_OPEN  → 開檔（讀 flash header，gif.open）
   ST_DECODE→ gif.playFrame() 完整解碼一幀到 g_cooked，啟動整幀傳輸
   ST_BLIT  → 等 LCD_SendFrameDone()；最後一幀則 gif.reset()，回 ST_DECODE
   ST_ERROR → 重試開檔
   ```

5. 每秒透過 UART 印出一次 `FPS=.. CPU=..%`（CPU busy = `playFrame()` 內解碼 +
   阻塞式 flash 讀取的時間）。

---

## 7. 相關檔案

| 檔案 | 說明 |
|------|------|
| [gif_player.cpp](gif_player.cpp) | 播放器狀態機、AnimatedGIF callback、cooked 畫布、FPS/CPU 統計 |
| [gif_player.h](gif_player.h)   | 公開 API（`GIF_Player_Init` / `GIF_Player_Task`）、`GIF_FILE_SIZE` |
| [qspi_flash.c](qspi_flash.c)   | QSPI0 Quad I/O 讀 SPI flash（0xEB、RX PDMA ch1、2 KB block cache） |
| [lcd.c](lcd.c)          | SPI1 ILI9341 驅動、整幀背景傳輸（TX PDMA ch0，IRQ 串接雙半段） |
| [gif_compat.h](gif_compat.h)   | 提供 AnimatedGIF 所需的 `millis()`/`delay()`（TIMER0，100 µs tick） |
| [main.c](main.c)         | 系統初始化、TIMER0 ms tick、PDMA0 IRQ handler、HardFault 診斷 |
| [AnimatedGIF/](AnimatedGIF/)   | bitbank2 AnimatedGIF library |

---

## 8. 疑難排解

| 症狀 | 可能原因 / 處置 |
|------|----------------|
| 畫面全黑、UART 有開機訊息 | GIF 未燒入 Flash，或 `GIF_FILE_SIZE` 太小導致提早截斷 |
| 細節畫面出現斜線 / 撕裂 | SPI 時脈過高，`lcd.h` 把 `LCD_SPI_CLK_FREQ` 調回 30 MHz |
| 解碼時 HardFault（CFSR bit20 STKOF） | Stack 太小，確認 scatter 保留 32 KB Stack |
| FPS 偏低 | 本版為序列設計；若素材為單一全域調色盤，可改用 `GIF_Decode_Player gobal palette` 管線化版 |
