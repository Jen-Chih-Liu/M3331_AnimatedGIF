# GIF Decode Player（Global Palette 版）

Nuvoton **M3334KI（Cortex-M33 @ 180 MHz）** 的串流 GIF 播放器。GIF 原始檔預先燒錄
在板上外接 SPI NOR Flash，韌體以 QSPI0 讀入、用 [AnimatedGIF](https://github.com/bitbank2/AnimatedGIF)
library 做 LZW 解碼，並把 320×240 RGB565 影格透過 SPI1 串流到 ILI9341 TFT 面板。

```
SPI flash --(QSPI0 Quad I/O, RX PDMA ch1)--> LZW 解碼 --> 8-bit 索引畫布 g_index
          --(全域調色盤 LUT)--------------> RGB565 傳輸緩衝 g_cooked
          --(SPI1, TX PDMA ch0)-----------> ILI9341 TFT (320x240)
```

解碼第 N+1 幀與 LCD 傳輸第 N 幀「管線化重疊」，把每幀時間從
`解碼 + 傳輸` 降為 `max(解碼, 傳輸) + cook`。

---

## 1. 硬體需求

| 項目 | 說明 |
|------|------|
| MCU  | Nuvoton M3334KIGAE（Cortex-M33，512 KB Flash / 384 KB SRAM） |
| 顯示 | ILI9341 320×240 TFT（SPI 介面） |
| 儲存 | 外接 SPI NOR Flash（容量需 ≥ GIF 檔案大小，`dance.gif` 約 2.36 MB） |
| 偵錯 | Nu-Link / Nu-Link2（SWD） |
| 序列 | UART0，115200 8N1（開機訊息與診斷輸出） |

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
| C++ 來源 | `gif_player.cpp`、`AnimatedGIF/AnimatedGIF.cpp`（需用標準 C 函式庫，非 MicroLIB） |
| 預先 include | `-include ../gif_compat.h`（提供 `millis()`/`delay()`） |

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
（目前預設 `2477900`，對應 `dance.gif`）。填入大於實際檔案的值仍可運作——播放器會
偵測 GIF trailer 並循環播放。

---

## 4. 全域調色盤（Global Palette）—— 重要前提

本播放器的管線化設計**假設整個 GIF 使用單一全域調色盤（Global Color Table）**，
每一幀都共用同一組 256 色 RGB565 調色盤。

### 為什麼需要這個假設

AnimatedGIF library 在 `playFrame()` 內部會把每幀合成到一塊 8-bit 的
「索引畫布」（`g_index`），正確處理幀間透明與 disposal。要顯示到面板上，
必須把索引值經調色盤查表轉成 RGB565（cook）。

為了讓「解碼下一幀」和「傳輸目前這幀」能重疊，cook 這一步被**延後**到
傳輸完成後，由播放器自己對整張索引畫布做一次查表：

```c
for (i = 0; i < W*H; i++)
    g_cooked[i] = g_pPal565[g_index[i]];
```

其中 `g_pPal565` 是從 GIFDRAW callback 擷取的 RGB565 調色盤指標。
**只有在整個動畫共用同一組全域調色盤時，用這個指標 cook 整張畫布才會正確。**

### 如果 GIF 使用區域調色盤（Local Palette）

若某幀帶有自己的區域調色盤（Local Color Table），該幀在合成畫布上的
「舊有區域」仍會沿用先前擷取到的調色盤，顏色可能錯誤。

此情況下，`GIF_Draw_CB()` 會印出一次警告：

```
WARN: GIF frame uses a local palette; pipelined cook assumes a single
      global palette, colours may be wrong
```

看到此警告代表播放用的素材不符合前提，請改用全域調色盤重新輸出 GIF。

### 如何確認你的 GIF 符合前提

全域調色盤 + 無區域調色盤 + disposal method 1，即為最佳相容素材。
用下列方式檢查（Python）：

```python
data = open("your.gif", "rb").read()
# Global Color Table flag = (data[10] >> 7) & 1  -> 應為 1
# 逐一走訪 image descriptor (0x2C)，其 packed byte bit7 (local palette) 應皆為 0
```

目前驗證過的素材 `dance.gif`：
- 320x240、209 幀
- Global Color Table 256 色、**0 幀使用區域調色盤** ✓
- Disposal method 全為 method 1（leave-in-place）✓
- 206 幀含透明像素

---

## 5. 記憶體配置

| 緩衝區 | 大小 | 說明 |
|--------|------|------|
| `g_index`  | ≈ 76.8 KB | 8-bit 索引畫布（+ library 1 行 scratch） |
| `g_cooked` | ≈ 150 KB | RGB565 傳輸緩衝（送往面板） |
| GIFIMAGE   | ≈ 24 KB | AnimatedGIF 內部狀態 |
| Stack      | 32 KB | LZW 解碼需求 |

合計約 226 KB，落在 384 KB SRAM 預算內。採 index-only + 獨立 cook 緩衝的拆分，
在**不增加總記憶體**的前提下達成解碼／傳輸重疊。

---

## 6. 執行流程

1. `SYS_Init()`：核心時脈 180 MHz、QSPI0 / SPI1 腳位與模組時脈、UART0 115200。
2. `QSPI_Flash_Init()` → `SPI_LCD_Init()` → `LCD_Init()`。
3. `GIF_Player_Init()`：重置狀態機、清畫面。
4. 主迴圈反覆呼叫 `GIF_Player_Task()`（非阻塞狀態機）：
   `ST_OPEN → ST_PRIME（解碼+cook+送出 frame0）→ ST_DECODE_NEXT（解碼下一幀，
   同時背景傳輸目前這幀）→ ST_WAIT_TX（傳輸完成後 cook 並送出，循環）`。
5. 每秒透過 UART 印出一次 CPU busy / FPS 統計。

---

## 7. 相關檔案

| 檔案 | 說明 |
|------|------|
| [gif_player.cpp](gif_player.cpp) | 播放器狀態機、callback、index/cook 緩衝、管線化邏輯 |
| [gif_player.h](gif_player.h)   | 公開 API（`GIF_Player_Init` / `GIF_Player_Task`）、`GIF_FILE_SIZE` |
| [qspi_flash.c](qspi_flash.c)   | QSPI0 Quad I/O 讀 SPI flash（0xEB、RX PDMA ch1、2 KB block cache） |
| [lcd.c](lcd.c)          | SPI1 ILI9341 驅動、整幀背景傳輸（TX PDMA ch0，IRQ 串接雙半段） |
| [gif_compat.h](gif_compat.h)   | 提供 AnimatedGIF 所需的 `millis()`/`delay()`（TIMER0，100 µs tick） |
| [main.c](main.c)         | 系統初始化、TIMER0 ms tick、PDMA0 IRQ handler、HardFault 診斷 |
| [AnimatedGIF/](AnimatedGIF/)   | bitbank2 AnimatedGIF library（含本專案的小幅 patch） |

### Library patch 註記

`AnimatedGIF/gif.inl` 的 `DecodeLZW()` 內，disposal method 2 的 cooked 寫入
已加上 `pfnDraw == NULL` 保護：本播放器以 callback 模式運行（索引畫布模式），
該模式下索引畫布後方不存在完整 cooked 畫布，若寫入會越界。索引 memset
保留，全幀模式（無 callback）行為不變。

另外 `gif.inl` 的 NEON 判斷式改為同時檢查 `__ARM_NEON`，讓 Cortex-M33
（ARMv8-M，無 NEON）正確退回到泛用 C 的透明合成路徑。

---

## 8. 疑難排解

| 症狀 | 可能原因 / 處置 |
|------|----------------|
| 畫面全黑、UART 有開機訊息 | GIF 未燒入 Flash，或 `GIF_FILE_SIZE` 太小導致提早截斷 |
| 顏色錯誤、出現 local palette 警告 | 素材含區域調色盤，請改用全域調色盤重新輸出 |
| 細節畫面出現斜線 / 撕裂 | SPI 時脈過高，`lcd.h` 把 `LCD_SPI_CLK_FREQ` 調回 30 MHz |
| 解碼時 HardFault（CFSR bit20 STKOF） | Stack 太小，確認 scatter 保留 32 KB Stack |
