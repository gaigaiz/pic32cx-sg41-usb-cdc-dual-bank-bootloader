# PIC32CX1025SG41128 USB CDC Dual-Bank Bootloader

## 1. 项目解决什么问题

本工程在 Microchip PIC32CX1025SG41128（Cortex-M4F，1 MB Flash）上实现一个 Fail-Safe 双 Bank Bootloader，并把升级通道由传统 SERCOM4 UART 改为 **USB CDC 虚拟串口**：

- MCU 作为 USB Device（CDC ACM），PC 作为 Host，枚举为一个 COM 口；
- PC 端通过 `Tools/btl_host.py` 按既有帧协议下载 raw bin 镜像；
- 双 Bank（BankA / BankB）互为备份，先写备用 Bank、校验后再 Bank Swap，写入中途断电不会破坏正在运行的旧固件；
- 编译期通过 `BTL_USE_CDC` 在 USB CDC 与 SERCOM4 串口之间二选一，串口路径保留为回退基线。

## 2. 主要功能

- **USB CDC 升级通道**：单功能 CDC，VID `0x04D8` / PID `0x000A`，Bulk IN/OUT + 通知端点；轮询方式服务 USB FS 驱动栈。
- **通道抽象**：帧解析、命令分派、Flash 擦写、CRC、双 Bank 切换、`run_Application` 等后端只保留一份；收发前端抽象为 `btl_io_rx_ready / rx_byte / tx_byte / tx_flush` 与 `btl_cdc_service / initialize`。
- **双 Bank 安全升级**：每个 Bank 开头 16 KB BL 槽，其后为 APP 段；failsafe 扇区记录 expected CRC / 版本 / 重试计数；WDT 连续失败可回退一次。
- **RAM 触发**：APP 写 RAM 触发魔数并复位，即可回到 BL 升级模式。
- **上位机与打包脚本**：
  - `Tools/btl_host.py`：组包发送、块切分、进度条、UNLOCK/DATA/VERIFY/RESET/BKSWAP/READ_VERSION；
  - `Tools/make_merged_bin.py`：从 BL/APP HEX 生成 `bl.bin / app.bin / merge.bin / bankb_merged.bin`，并做向量 Thumb 位与区间安全校验。

### Flash 布局（当前 USB 工程）

| 区域 | 起始 | 结束 | 大小 |
|---|---:|---:|---:|
| BL_A | `0x00000000` | `0x00003FFF` | 16 KB |
| APP_A | `0x00004000` | `0x0007DFFF` | 0x7A000 |
| failsafe_A | `0x0007E000` | `0x0007FFFF` | 0x2000 |
| BL_B | `0x00080000` | `0x00083FFF` | 16 KB |
| APP_B | `0x00084000` | `0x000FDFFF` | 0x7A000 |
| failsafe_B | `0x000FE000` | `0x000FFFFF` | 0x2000 |

## 3. 安装方法

### 3.1 工具链与依赖
- MPLAB X / MPLAB VSCode 插件 + XC32 工具链（v5.10 系）；
- PIC32CX-SG41 DFP 与 Harmony/MCC Melody 组件（NVMCTRL、USB Device Layer、USB FS Driver、CDC Function Driver、SERCOM4 回退）；
- Python 3，安装 `pyserial`：
  ```bat
  pip install pyserial
  ```

### 3.2 工程结构
```
USB/
├── BL/        Bootloader 工程（USB CDC 前端 + 共享后端）
├── APP/       应用固件工程（链接起点 0x4000）
├── Tools/     btl_host.py 上位机 + make_merged_bin.py 打包脚本
└── docs/      使用说明、开发流程、问题与解决记录
```

### 3.3 关键配置（MCC 图形化）
- 时钟：DFLL 48 MHz → GCLK0 / CPU 48 MHz；USB 使用 48 MHz 时钟源；DPLL0 不启用。
- USB 引脚：`USB_DM=PA24`、`USB_DP=PA25`。
- CDC Instance0：2 个接口，Interrupt EP1，Bulk IN/OUT EP2；单功能 CDC 不使用 IAD。
- 详细步骤见 `docs/02_USB_CDC_Bootloader_开发流程.md`。

## 4. 使用方法

### 4.1 生成 Flash 镜像
在 `Tools/` 目录：
```bat
python make_merged_bin.py --target all
```
产物输出到 `Tools/merged_out/`：`bl.bin`、`app.bin`、`merge.bin`、`bankb_merged.bin`。

### 4.2 下载到板子
> 端口号以设备管理器实际 COM 为准（示例 COM5）。

更新并留在 BankA（当前活动 Bank，不切换）：
```bat
python btl_host.py -i COM5 -d pic32cx_sg41 -a 0x4000 -f merged_out\app.bin
```

预备完整 BankB 镜像（只写不切）：
```bat
python btl_host.py -i COM5 -d pic32cx_sg41 -a 0x80000 -f merged_out\bankb_merged.bin
```

更新 BankB APP 并切换到 BankB：
```bat
python btl_host.py -i COM5 -d pic32cx_sg41 -a 0x84000 -f merged_out\app.bin -s
```

### 4.3 命令与双 Bank 语义
- `-a`：写入起始**物理地址**（`0x4000`=APP_A、`0x84000`=APP_B、`0x80000`=BankB 起点合并镜像）。
- 不带 `-s`：末尾发 `0xA3` 普通复位，不切换 Bank；
- 带 `-s`：末尾发 `0xA4`，记录 BankB APP CRC 后 `NVMCTRL_BankSwap()` 翻转并复位。
- 完整命令清单、避坑与判别方法见 `docs/01_USB_Bootloader_命令使用说明.md`。

## 5. 输入输出示例

命令（更新 BankA APP）：
```bat
python btl_host.py -i COM5 -d pic32cx_sg41 -a 0x4000 -f merged_out\app.bin
```

预期输出（标准阶段）：
```
Reading Bootloader Version

Bootloader version : v3.7

Unlocking

Programming: |||||||||||||||||||||||||||||||||||||||||||||||||||| 100.0% Complete

Verification skipped (USB‑CDC workaround)

Rebooting

Reset command sent. MCU will reboot, USB will disconnect, skip waiting ACK.
```

手动读取版本（串口助手 HEX）：
```
发送: 4D 43 48 50 00 00 00 00 A6
回包: 50 03 07
```

## 6. 文档索引

- `docs/01_USB_Bootloader_命令使用说明.md`：下载命令、地址模型、`-s` 语义与切换判别。
- `docs/02_USB_CDC_Bootloader_开发流程.md`：从 MCC 图形化配置到代码接线、验证流水线。
- `docs/03_USB_CDC_Bootloader_问题与解决记录.md`：枚举、通信、镜像、双 Bank、跳转等问题与处置状态。

## 7. 当前状态与已知限制

- `0xA2 VERIFY` 主机校验当前临时跳过；`0xA3/0xA4` 发出后暂不等待 ACK。
- `0xA4` 当前固定对 BankB（`0x84000` / `0xFE000`）操作，适配“默认 BankA → 切到 BankB”的单向流程；可靠往返切换（读 `AFIRST` 自适应对面 Bank）尚未实现。
- APP 跳转与 LED 现场闭环、DFLL/USB 时钟锁定复核为待验证项，详见问题记录文档。
- 构建产物（`_build/`、`out/`）、DFP 包（`packs/`）与本地调试配置不纳入版本库。

## 8. 版权说明

脚本与固件框架基于 Microchip Harmony/MPLAB 样例代码，版权归 Microchip Technology Inc. 所有；衍生代码仅可用于 Microchip 产品。
