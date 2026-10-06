# M3331 GIF 顯示專案

本倉庫是一套以 Nuvoton **M3334KI（Cortex-M33 @ 180 MHz）** 為核心的完整 GIF
播放解決方案：從 PC 端把 GIF 燒錄到板上外接的 **SPI NOR Flash**，再由 MCU 韌體
以 QSPI0 讀入、LZW 解碼，最後把 320×240 RGB565 影格串流到 ILI9341 TFT 面板播放。

```
┌──────────────┐  USB HID  ┌─────────────────────┐  QSPI0  ┌──────────────┐  QSPI0  ┌─────────────────┐  SPI1  ┌──────────────┐
│  PC 燒錄工具  │ ────────► │  HID_SpiFlash_Writer │ ──────► │  SPI NOR     │ ──────► │  GIF_Decode_    │ ─────► │  ILI9341 TFT │
│ (Windows exe)│           │  (MCU 燒錄韌體)       │  讀/寫   │  Flash       │  讀取    │  Player (韌體)   │  影格   │  320x240     │
└──────────────┘           └─────────────────────┘         └──────────────┘         └─────────────────┘        └──────────────┘
      燒錄階段                                                                              播放階段
```

整體流程分兩個階段：

1. **燒錄階段**：PC 工具透過 USB HID 把 `dance.gif` 寫入 SPI Flash（搭配燒錄韌體）。
2. **播放階段**：播放器韌體從 SPI Flash 讀取、解碼並顯示到 TFT 面板。

---

## 目錄結構

| 路徑 | 說明 |
|------|------|
| [hid_gif_spiflash_tool/](hid_gif_spiflash_tool) | **PC 端 Windows HID 工具**：把 GIF／二進位檔燒錄到 SPI Flash，並可回讀／驗證 |
| [M3331_Displaying_GIF/SampleCode/ExampleCode/HID_SpiFlash_Writer/](M3331_Displaying_GIF/SampleCode/ExampleCode/HID_SpiFlash_Writer) | **MCU 燒錄韌體**：USB HID → SPI NOR Flash 寫入／回讀／抹除 |
| [M3331_Displaying_GIF/SampleCode/ExampleCode/GIF_Decode_Player org/](M3331_Displaying_GIF/SampleCode/ExampleCode/GIF_Decode_Player%20org) | **GIF 播放器（原始／通用版）**：序列解碼，相容任何 GIF |
| [M3331_Displaying_GIF/SampleCode/ExampleCode/GIF_Decode_Player gobal palette/](M3331_Displaying_GIF/SampleCode/ExampleCode/GIF_Decode_Player%20gobal%20palette) | **GIF 播放器（全域調色盤版）**：管線化解碼，高 FPS，需單一全域調色盤 |
| [M3331_Displaying_GIF/Library/](M3331_Displaying_GIF/Library) | Nuvoton M3331 BSP（CMSIS、Device、StdDriver） |
| `parse_gif.py` | 分析 GIF 結構（調色盤、幀數、disposal、透明度等）的輔助腳本 |
| `dance.gif` | 驗證過的範例素材（320×240、209 幀、全域調色盤） |

---

## 元件總覽

### 1. hid_gif_spiflash_tool（PC 燒錄工具）

Windows 端 USB HID 工具，透過標準 HID 介面對 M3334KI 開發板下命令，把 GIF
（或任意二進位檔）燒錄到板上外接的 SPI NOR Flash，並可回讀／驗證。

- **USB VID/PID**：`0x0416 / 0x5020`
- **HID Page 大小**：1024 bytes
- **Flash 抹除單位**：4 KB sector；**程式化單位**：256 bytes/page
- **建置**：CMake ≥ 3.15 + Visual Studio 2022 Build Tools（連結 `setupapi`、`hid`）

```powershell
cd "hid_gif_spiflash_tool"
cmake -S . -B build
cmake --build build --config Release
# 燒錄並驗證
.\build\Release\hid_gif_spiflash_tool.exe -base 0x0 -program ..\dance.gif -verify
```

詳見 [hid_gif_spiflash_tool/README.md](hid_gif_spiflash_tool/README.md)。

### 2. HID_SpiFlash_Writer（MCU 燒錄韌體）

Nuvoton M3334KI 韌體：透過 USB HID（HSUSBD，High-Speed）接收 PC 命令，把資料
寫入外接 SPI NOR Flash，並支援回讀與抹除。與 PC 工具使用同一組 20-byte 命令協定。

- **MCU**：M3334KIGAE @ 180 MHz；**USB**：HSUSBD；**Flash 介面**：QSPI0（PC0~PC5，單線）
- **Debug UART**：UART0（PB12/PB13，115200-8n1）
- **建置**：Keil MDK（ARM Compiler 6）＋ Nuvoton M33 DFP，Nu-Link 下載

詳見 [HID_SpiFlash_Writer/README.md](M3331_Displaying_GIF/SampleCode/ExampleCode/HID_SpiFlash_Writer/README.md)。

### 3. GIF_Decode_Player（播放器韌體，兩種版本）

從 SPI Flash 讀入 GIF，以 [AnimatedGIF](https://github.com/bitbank2/AnimatedGIF)
做 LZW 解碼，把 320×240 RGB565 影格透過 SPI1 串流到 ILI9341 面板。提供兩種實作：

| 版本 | 解碼 / 傳輸 | 調色盤前提 | FPS | 適用 |
|------|-------------|-----------|-----|------|
| [org（原始／通用版）](M3331_Displaying_GIF/SampleCode/ExampleCode/GIF_Decode_Player%20org) | 序列（解碼→傳輸→下一幀） | **無限制**，任何 GIF | 較低 | 含區域調色盤或不確定素材時 |
| [gobal palette（全域調色盤版）](M3331_Displaying_GIF/SampleCode/ExampleCode/GIF_Decode_Player%20gobal%20palette) | 管線化重疊（解碼 N+1 幀時傳輸 N 幀） | 需單一全域調色盤 | 較高 | 素材為單一全域調色盤時追求高 FPS |

共同硬體：ILI9341 320×240 TFT（SPI1）、外接 SPI NOR Flash（QSPI0）、LCD
RST=PD14 / DC=PH10 / EN=PH11。Stack 需 **32 KB**（LZW 解碼用量大）。

GIF 須先用上述 HID 工具燒到 Flash 位址 `0x0`，並把實際位元組大小填入
`gif_player.h` 的 `GIF_FILE_SIZE`（預設 `2477900`，對應 `dance.gif`）。

---

## 快速開始

1. **編譯並下載燒錄韌體**：用 Keil 開啟 `HID_SpiFlash_Writer`，Build 後以 Nu-Link
   下載到 M3334KI 開發板。
2. **燒錄 GIF**：編譯 `hid_gif_spiflash_tool`，把開發板以 USB 連上 PC，執行
   `hid_gif_spiflash_tool.exe -base 0x0 -program dance.gif -verify`。
3. **編譯並下載播放器韌體**：依素材特性選擇 `GIF_Decode_Player org`（通用）或
   `GIF_Decode_Player gobal palette`（高 FPS），用 Keil Build 後下載。
4. 開機後面板即開始循環播放 GIF；UART0（115200-8n1）會輸出開機訊息與 FPS/CPU 統計。

---

## 素材需求

- 320×240、RGB565 可顯示的 GIF。
- 若使用 **全域調色盤版** 播放器，GIF 必須：單一全域調色盤（Global Color Table）、
  無任何區域調色盤（Local Color Table）、disposal method 1。可用 `parse_gif.py`
  檢查素材是否符合前提。
- 範例素材 `dance.gif`：320×240、209 幀、全域調色盤 256 色、0 幀使用區域調色盤。
