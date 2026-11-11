# USB CDC Bootloader 开发流程（图形化配置 → 代码接线 → 验证）

> 工程：本仓库根目录；芯片 PIC32CX1025SG41128（Cortex-M4F）
> 目标：把升级通道从 SERCOM4 UART 改为单路 USB CDC 虚拟串口（PC 识别为 1 个 COM），复用既有帧协议、双 Bank、RAM 触发与 `run_Application`。
> 分工边界：助手负责代码/脚本修改与语法核对；MCC 图形生成、编译、烧录、硬件现象由开发者完成。

---

## 1. 总体架构

```
PC (Host)                          MCU (Device)
 btl_host.py  ── CDC bulk/intr ──► USB Device Layer ──► CDC Function(Instance0)
        ▲                              ▲  FS Driver(Device)
        └──────── virtual COM ◄────────┘
                 │ 帧字节流
                 ▼
        btl_io_rx_ready/rx_byte/tx_byte/tx_flush   ← 编译期前端抽象
                 │
        共享后端：帧解析 input_task → command_task(0xA0/A1/A2/A3/A4/A6)
                 → flash_task(擦写) → CRC / BankSwap / BootCheck / run_Application
```

编译期二选一（`bootloader/btl_channel.h`）：

| `BTL_USE_CDC` | 前端 | 说明 |
|---|---|---|
| `1` | `btl_cdc.c`（USB CDC） | 当前 USB 通道 |
| `0` | SERCOM4 USART 内联原语 | 串口回退基线，保证改动可回归 |

共享后端（`bootloader_common.c`、`bootloader_uart.c` 的帧解析/命令/擦写/CRC/跳转）**只保留一份**，不随通道复制。

### 前端抽象原语（`btl_channel.h`）
- `bool btl_io_rx_ready(void)`
- `uint8_t btl_io_rx_byte(void)`
- `void btl_io_tx_byte(uint8_t b)`
- `void btl_io_tx_flush(void)`
- `void btl_cdc_service(void)`（两种构建都存在；UART 模式为空操作）
- `void btl_cdc_initialize(void)`（CDC 模式）

---

## 2. 工程目录与关键文件

```
USB\
├── BL\config.mcc\src\
│   ├── main.c                         SYS_Initialize → btl_cdc_initialize → while(bootloader_UART_Tasks())
│   ├── config\default\
│   │   ├── initialization.c           启动期 Trigger/BootCheck 判断 → run_Application(0x4000)
│   │   ├── btl.ld                     BL 链接：ROM 0x0/16KB，RAM 0x20000010/0x3FFF0
│   │   └── bootloader\
│   │       ├── btl_channel.h          BTL_USE_CDC 选择 + 前端原语声明/内联
│   │       ├── btl_cdc.c               CDC 前端（枚举、环形缓冲、轮询服务、原语实现）
│   │       ├── bootloader_common.h/.c   分区常量、CRC、BootCheck、RecordAppCrc、run_Application
│   │       └── bootloader_uart.c       帧解析、command_task、flash_task、0xA3/0xA4
├── APP\config.mcc\src\main.c          应用：LED、banner、WDT、fail-safe 启动标记
└── Tools\
    ├── btl_host.py                     PC 上位机（帧组包/发送/进度条/收尾命令）
    └── make_merged_bin.py             BL/APP/合并/bin 打包与向量安全校验
```

---

## 3. 阶段 0：Flash 规划与链接对齐（先定分区，再开通道）

| 项 | 值 | 落点 |
|---|---|---|
| BL 槽 | `0x00000000–0x00003FFF`，16 KB（0x4000） | `btl.ld` ROM_START=0、ROM_SIZE=16384；`BOOTLOADER_SIZE=16384` |
| APP 起点 | `0x4000` | `APP_START_ADDRESS=0x4000`；APP 工程构建宏 `ROM_ORIGIN=0x4000` |
| APP 段大小 | `0x7A000`（`0x4000–0x7DFFF`） | `APP_REGION_SIZE` |
| failsafe_A | `0x7E000–0x7FFF`；failsafe_B `0xFE000–0xFFFF` | CRC/版本/重试计数 |
| RAM 保留 | `0x20000000` 起 16 B 给触发魔数 | BL/RAM 起点 `0x20000010`；APP 同样预留 |
| BL 占用实测 | ROM 0x35F0（≈84.3% of 16 KB） | 余量约 2.5 KB，勿再大量加代码 |

> 历史教训：BL 曾为 8 KB、APP 起点 `0x2000`；切换到 USB 前端后必须同步扩到 16 KB、APP 起点 `0x4000`，否则向量/链接基址错误，复位不进正确镜像。

---

## 4. 阶段 1：MCC 图形化配置（USB 相关）

> 以下为已验证采用的配置口径；具体菜单名称以 MCC Melody 当前版本与生成代码为准，生成后以 `plib_*.c`/描述符数据复核。

### 4.1 时钟（USB 必须 48 MHz）
- 使用 DFLL 产生 **48 MHz**；`GCLK0 = DFLL ÷ 1 = CPU 48 MHz`；USB 挂到 48 MHz 时钟源。
- DPLL0 不启用；`GCLK2 = OSCULP32K 32.768 kHz` 供 RTC/WDT 类慢时钟。
- 建议：调试器 halt 确认 DFLL 锁定位有效、USB 时钟选择生效（遗留待实测项），避免时钟未就绪导致枚举失败。

### 4.2 USB 引脚
| 信号 | 引脚 | 配置 |
|---|---|---|
| USB_DM | PA24 | USB D- |
| USB_DP | PA25 | USB D+ |
| VBUS_DET / ID / VBUS_EN | PC11 / PB17 / PC20 | 初版曾勾选，最终按当前固件关闭外部 VBUS 检测/使能引脚（`VBUS_SENSE=false`），由固件 Open 成功后直接 Attach |

### 4.3 USB Device Layer
- Functions = 1；Endpoints = 2（EP0 控制 + CDC 数据/通知端点）。
- EP0 = 64 字节。
- VID = `0x04D8`；PID = `0x000A`；Self-Powered；MaxPower 按板卡填写。
- **不勾 IAD（接口关联描述符）**：单功能 CDC 不需要 IAD；IAD 是多功能复合设备才用。

### 4.4 CDC Function Driver（Instance 0）
- Start Interface = 0；Interface Count = 2（CDC 控制接口 + 数据接口）。
- Queue Size = 1；Interrupt Endpoint = EP1；Bulk IN = EP2、Bulk OUT = EP2（具体端点号以生成描述符为准，IN/OUT 成对）。
- 不使用自定义 Vendor 端点方案（已否决：增加复杂度且非必要）。

### 4.5 模块依赖（Project Graph）
- USB Device Layer、USB FS Driver（Device 模式）、CDC Function Driver；
- NVMCTRL（擦写 Flash）；
- SERCOM4 USART 仅在 `BTL_USE_CDC=0` 回退时使用，USB 构建可不依赖其收发。

---

## 5. 阶段 2：抽象层改造思路

1. 在 `btl_channel.h` 定义 `BTL_USE_CDC` 与 5 个字节级原语；`=0` 时用 SERCOM4 内联实现，`=1` 时声明外部函数（`btl_cdc.c` 实现）。
2. 把后端所有直接收发替换为 `btl_io_*`：
   - 收：`while (btl_io_rx_ready()) btl_io_rx_byte()`；
   - 发：`btl_io_tx_byte(...)` + 必要时 `btl_io_tx_flush()`。
3. `btl_cdc_service()` 在主循环无条件调用，UART 构建为空函数，保证循环代码统一。
4. `=0` 先做一次回归提交，确认串口升级行为不变，再切 `=1`，便于回退。

---

## 6. 阶段 3：CDC 前端 `btl_cdc.c` 实现要点

### 6.1 缓冲与状态
- `rxSlot[64]`（对齐 16B）：USB 单次接收槽；`rxRing[1024]`：主机侧应用读取前的环形队列。
- `txRing[256]` + `txSlot[64]`；`txBusy` 标志 + `TX_BUSY_STALL_LIMIT=32` 兜底轮询次数，防止 TX 卡死误判。
- 状态：`usbdHandle`、`usbdConfigured`、`cdcRegistered`、`btlAttached`；行编码 `cdcLineCodingGet/Set` 保留。

### 6.2 初始化 `btl_cdc_initialize()`
1. `DRV_USBFSV1_Initialize(INDEX_0, ...)`；
2. `USB_DEVICE_Initialize(INDEX_0, ...)`；
3. 注册 USB Device 事件回调与 CDC 实例事件回调；
4. **禁用 MCC 生成的四条 USB NVIC 中断提交**（本工程采用轮询模式，避免中断提交时机/ISR 序列导致描述符阶段异常）；
5. Open 成功后 `btl_attach_once()` → `USB_DEVICE_Attach(usbdHandle)`（D+ 上拉，最多一次；POWER_REMOVED 后复位允许重挂）。

### 6.3 轮询服务 `btl_cdc_service()`（主循环反复调用）
- 调用 `DRV_USBFSV1_Tasks_ISR(sysObj.drvUSBFSV1Object)` 处理复位/枚举/EP0 SETUP；
- 处理配置/挂起/恢复/拔插事件；接收完成把槽数据搬入 `rxRing` 并重新武装 RX；
- `btl_service_tx()` 推进 TX 环形队列；`btl_io_tx_flush()` 触发一次发送推进。

### 6.4 对外原语
- `btl_io_rx_ready()`：`rxHead != rxTail`；
- `btl_io_rx_byte()`：从 `rxRing` 取 1 字节；
- `btl_io_tx_byte()`：入 `txRing`；
- `btl_io_tx_flush()`：推进 TX 提交，确保 ACK 在 Flash 操作前被送出主机。

### 6.5 枚举修复关键经验（已验证方向，详见问题记录文档）
- 主循环**轮询** `DRV_USBFSV1_Tasks_ISR()`；
- 关闭无效 USB NVIC IRQ；
- Open 成功后一次性 Attach；
- 单功能 CDC 不加 IAD、不加 Vendor 端点。

---

## 7. 阶段 4：共享后端接线与稳帧

### 7.1 帧格式
- `GUARD=0x5048434D`（小端 `4D 43 48 50`）；帧头 = GUARD(4)+SIZE(4)+CMD(1) = 9 B；`DATA_SIZE=8192`；`PAGE_SIZE=512`；`ERASE_BLOCK=8192`。

### 7.2 命令分派（`command_task` / `bootloader_uart.c`）
| 命令 | 行为 |
|---|---|
| `0xA0` UNLOCK | 校验 `end>begin && end<=0x100000`，置 `unlock_begin/end`，回 `0x50` |
| `0xA1` DATA | `flash_addr` 须在解锁区间；拷 8192 字、`flash_data_ready=true`、回 `0x50` |
| `0xA2` VERIFY | 对解锁区间 CRC（脚本当前临时跳过） |
| `0xA3` RESET | flush 后 `bootloader_TriggerReset()`，不切 Bank |
| `0xA4` BKSWAP_RESET | CRC `0x84000` → 写 failsafe_B `0xFE000` → `NVMCTRL_BankSwap()` |
| `0xA6` READ_VERSION | 回 `50 03 07`（v3.7）；未知命令回 `0x52` |

### 7.3 接收侧修复（DATA 大块关键）
- `input_task()` 改为 **`while (btl_io_rx_ready())` 连续排空**，收满一帧（`packet_received=true`）即 `break`；
- `RX_RING_SIZE` 由 256 提升到 1024，吸收 USB bulk 64 B 突发；
- UNLOCK/DATA/VERIFY 响应后调用 `btl_io_tx_flush()`，先把 ACK 交给主机，再进入 `flash_task` 擦写。

### 7.4 擦写 `flash_task()`
`NVMCTRL_RegionUnlock(addr)` → `NVMCTRL_BlockErase(addr)` → 按 512 B `NVMCTRL_PageWrite`；擦写等待间隙继续 `input_task()` + `btl_cdc_service()` 接收。写 inactive Bank（`0x80000` 起）不触碰活动 `0x0` 运行代码，安全；在线写活动 BL 区不安全，禁止。

---

## 8. 阶段 5：APP 工程对齐 `0x4000`

1. 链接：APP 构建宏 `ROM_ORIGIN=0x4000`；`RAM_ORIGIN=0x20000010` 跳过触发保留区。
2. 启动文件 `startup_xc32.c` 的 Reset_Handler 设置 `SCB->VTOR = &__svectors & ~SCB_VTOR_TBLOFF_Msk`，保证非零起点向量表生效。
3. 向量头校验：首字 = 初始 SP（`0x2003FFF8`），第二字 = Thumb Reset（位0=1）且落在 `[0x4000,0x7E000)`。
4. 打包脚本 `make_merged_bin.py` 解析 HEX type00/type04 记录、过滤 `0x804000` 配置位、校验基址，非法即中止。

---

## 9. 阶段 6：产物打包与版本管理

- `python make_merged_bin.py --target all` 产出 `bl.bin/app.bin/merge.bin/bankb_merged.bin`。
- 合并镜像为 Bank 相对布局：off0=BL 槽、off `0x4000`=APP；`bankb_merged.bin` 供 `-a 0x80000` 预备 BankB。
- 版本/提交纪律：源码与脚本精确 `git add`；`_build/**`、`out/**`、`merged_out/*.bin` 不入库；MCC regenerate 前备份人工修改文件（`btl_channel.h`、`btl_cdc.c`、`main.c`、脚本），防止被覆盖。

---

## 10. 阶段 7：分阶段验证流水线

1. **编译**：BL ROM 占用不超 16 KB；无链接错误（`_sdata/_edata` 缺失多为链接脚本被 MCC 重置，需核对 `btl.ld`）。
2. **枚举**：插上后设备管理器出现 COM（示例 COM5，`VID_04D8&PID_000A`，usbser 驱动加载）；无 Code43 / `DEVICE_DESCRIPTOR_FAILURE`。
3. **版本握手**：脚本或手动 A6 帧回 `50 03 07`；UNLOCK 回 `0x50`。
4. **大块下载**：DATA 块收到 `0x50` ACK、进度条到 100%；无 `no response received, retrying`。
5. **复位行为**：按 `BTL_TEMP_FORCE_STAY_IN_BL=0` 与 APP 有效性验证跳转；判别方法见命令文档第 9 节。
6. **双 Bank**：先 `-a 0x4000`（不带 -s）验证活动 Bank；再 `-a 0x84000 -s` 验证 BankB 切换；`-a 0x80000` 预备完整 BankB。
7. **=0 回归**：`BTL_USE_CDC=0` 编译烧录，确认 SERCOM4 串口升级路径未被抽象改造破坏。

---

## 11. 量产前开关与待办（TODO）

| 项 | 当前状态 | 要求 |
|---|---|---|
| `BTL_TEMP_FORCE_STAY_IN_BL` | `0` | 量产保持 0；=1 会强制留 BL |
| `0xA2 VERIFY` 主机校验 | 脚本临时跳过 | 量产恢复 CRC 应答等待 |
| `0xA3/0xA4` 末尾 ACK 等待 | 当前直接发送不等 | 量产补响应判定与重试 |
| Bank 往返切换 | 0xA4 硬编码 BankB | 路线乙：读 AFIRST、对面 Bank 地址自适应 |
| BL_B 纳入 CRC | 未纳入 | 升级 BL 本体前补校验与 failsafe 记录 |
| APP LED / 跳转闭环 | 待现场判别 | 复位后 A6 留 BL vs 无响应；检查 VTOR/时钟/LED 初始化 |
| DFLL/USB 时钟锁定 | 建议调试器确认 | 排除静默时钟问题 |

---

## 12. 常见工程约束回顾

- 不要在线擦写活动 BL（`0x0–0x3FFF`）；无 ramfunc/缓存清理易变砖。
- 帧长度以固件 `DATA_SIZE=8192` 为准；主机切块、补丁长度必须匹配解锁/擦除粒度。
- CDC 虚拟串口波特率仅软件占位，实际受 USB 全速/高速枚举约束，不必纠结 115200 寄存器。
- 修改后先 `python -m py_compile Tools\*.py` 做语法核对；固件改动由开发者编译验证。
