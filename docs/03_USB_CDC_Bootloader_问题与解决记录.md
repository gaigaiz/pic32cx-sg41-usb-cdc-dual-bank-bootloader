# USB CDC Bootloader 开发问题与解决记录

> 工程：本仓库根目录；固件 BL v3.7；通道 USB CDC（COM5，`VID_04D8&PID_000A`）
> 状态图例：**已解决** = 已验证闭环；**临时规避** = 可工作但待恢复正式实现；**待实测** = 线索明确、需硬件确认；**已否决** = 方向错误，勿重复尝试。

---

## A. USB 枚举类（设备管理器 / 描述符 / 驱动）

### A1 设备管理器 Code43：请求 USB 设备描述符失败（`DEVICE_DESCRIPTOR_FAILURE`）
- **现象**：插入后未知设备黄叹号，无 COM；设备 ID `USB\VID_0000&PID_0002`，问题码 `0x2B`，匹配段 `BADDEVICE.Dev.NT`，驱动 `usb.inf`；父集线器 `USB\VID_05E3&PID_0610`（Genesys Hub）；常规页提示“请求 USB 设备描述符失败”。按板载复位键可刷新设备列表但仍无法枚举。
- **根因（排查后定位）**：设备协议栈未被及时服务，描述符阶段（EP0 SETUP/GET_DESCRIPTOR）未响应；早期还叠加了无效 USB NVIC 中断提交与 Attach 时机不当。
- **处置（已采用方向）**：
  1. 主循环轮询 `DRV_USBFSV1_Tasks_ISR(sysObj.drvUSBFSV1Object)`；
  2. 在 `btl_cdc_initialize()` 关闭 MCC 生成的四条 USB NVIC IRQ，纯轮询服务；
  3. `USB_DEVICE_Open` 成功后一次性 `USB_DEVICE_Attach()`（`btl_attach_once`），POWER_REMOVED 后才允许重挂；
  4. 时钟保证 USB 48 MHz（DFLL48 → GCLK0/USB）。
- **结果**：枚举为 COM5，`usbser` 驱动加载（事件日志“已添加驱动程序服务 usbser”状态 0）。
- **状态**：**已解决**（枚举层面）；DFLL 锁定建议调试器复核（见 E4）。
- **关联**：`btl_cdc.c`（`btl_cdc_initialize`/`btl_cdc_service`/`btl_attach_once`）、`main.c` 主循环。

### A2 “缺少 IAD 描述符导致只出 COM 占位、usbser 未就绪”的猜测
- **现象**：一度怀疑单功能 CDC 缺 IAD（接口关联描述符）。
- **结论**：**已否决**。IAD 用于多功能复合设备（一个设备暴露多个功能集合）；本工程是单功能 CDC（1 个控制接口 + 1 个数据接口），不需要 IAD，勾选 IAD 反而引入错误分组。
- **状态**：**已否决**。

### A3 Vendor/自定义端点加大队列的尝试
- **结论**：**已否决**。问题不在端点队列容量；CDC Instance0 Queue=1、Int EP1、Bulk IN/OUT EP2 成对即可。盲目加自定义 Vendor 端点增加复杂度且无收益。
- **状态**：**已否决**。

### A4 VBUS 检测引脚与 Attach 时机
- **现象**：早期 USB FS Driver 曾勾选 Enable VBUS Sense；相关引脚 PC11/PB17/PC20 配置反复调整。
- **处置**：当前固件 `VBUS_SENSE=false`，不依赖外部 VBUS 检测引脚；Open 成功后由固件主动 Attach，简化上电枚举。
- **状态**：**临时规避**（若产品需要低功耗/拔插感知，再补 VBUS 检测）。

### A5 枚举成功但串口助手看不到/打不开 COM
- **判别**：先确认设备管理器 COM5 与 `usbser` 服务；若仅占位条目，回到 A1 检查协议栈轮询与 Attach。固件存活的旁证：复位后 READ_VERSION 可回 `50 03 07`、端口可打开。
- **状态**：随 A1 一并解决；打开失败多为端口被占用（另一串口助手/脚本未退出）。

---

## B. 下载通信类（帧收发 / ACK / 超时）

### B1 唯一 DATA 块超时：UNLOCK 秒回 `0x50`，DATA 三次 `read 0 byte(s)`
- **现象**：`Reading Bootloader Version`、`Unlocking` 正常，进度条 100% 后 `Warning: no response received, retrying 1/2/3` → `giving up`；手动判别：UNLOCK 秒回 `0x50`，DATA 无回包，复位后 READ_VERSION 可回、COM5 可开（固件未死）。
- **最初误判（已证伪）**：以为是 TX `txBusy` 单点阻塞；证据不支持（小命令能回、端口存活）。
- **根因（定位为 RX 侧）**：旧 `input_task()` 每轮主循环只取 1 字节，`RX_RING_SIZE=256`；USB bulk 以 64 B 突发灌入，环形缓冲被写满后丢字节，帧解析器攒不到 `size=8196` 的完整 DATA 帧，因此不产生 `0x50`。
- **处置**：
  1. `input_task()` 内 `while (btl_io_rx_ready())` 连续排空，收满一帧 `packet_received=true` 即 `break`；
  2. `RX_RING_SIZE` 提升到 1024；
  3. UNLOCK/DATA/VERIFY 响应补 `btl_io_tx_flush()`，先交付 ACK 再进入擦写。
- **结果**：DATA `accepted resp=0x50`，无 retry。
- **状态**：**已解决**。
- **关联**：`bootloader_uart.c`（`input_task`/命令响应）、`btl_cdc.c`（`rxRing`/`btl_io_tx_flush`）、`btl_host.py`（块切分与重试日志）。

### B2 ACK 滞后于 Flash 擦写
- **现象**：CDC 异步提交下，ACK 可能被 Flash 操作延后，主机先超时。
- **处置**：关键命令回 `0x50` 后立即 `btl_io_tx_flush()` 推进 TX；`btl_service_tx()` 用 stall 计数兜底，避免 `txBusy` 永久挂起。
- **状态**：**已解决**（发送侧）；`0xA3/0xA4` 末尾 ACK 等待仍见 B5。

### B3 设备表 Bootloader 尺寸不匹配
- **现象**：旧表 `BOOTLOADER_SIZE=8192` 与新 16 KB BL 不一致，影响解锁/布局假设。
- **处置**：`devices["PIC32CX_SG41"]=[8192,16384,True]`（ERASE_SIZE=8192、BOOTLOADER_SIZE=16384、DEV_CFG_SUPPORT=True）。
- **状态**：**已解决**。

### B4 `0xA2 VERIFY` 临时跳过
- **现象**：USB 通道调试期先打通下载链路，未启用主机 CRC 应答等待。
- **处置**：脚本打印 `Verification skipped (USB‑CDC workaround)`；固件 `0xA2` 仍具备 `CRCGenerate` 能力。
- **状态**：**临时规避**；量产前恢复正式 VERIFY 与应答等待。

### B5 `0xA3/0xA4` 发出后不等 ACK
- **现象**：复位/切换命令直接拼帧写入，不等待响应（复位会断链，等待意义有限，但缺少失败判定）。
- **状态**：**临时规避**；待补“命令被接受”判定与重试策略。

### B6 脚本调试日志的增删纪律
- **过程**：为定位 B1 曾加入 `[TX]/[RX]/[BLK]/[DBG]` 详细行；定位完成后按要求只保留标准阶段输出（版本/解锁/进度/跳过校验/复位提示），删除临时诊断，不改变控制流与帧参数。
- **状态**：**已解决**；标准阶段默认 `print`，devcfg/调谐细节仍由 `-v` 门控。

---

## C. 镜像 / 地址 / 打包类

### C1 旧 APP bin 基址错误（Reset `0x2401`、向量落在 `0x2xxx`）
- **现象**：旧 `app.bin` 来自 8 KB BL / APP `0x2000` 时代；Reset PC=`0x2401`、向量集中 `0x2xxx`，与新布局 `0x4000` 不符，跳转不可靠。
- **处置**：`make_merged_bin.py` 重写为 USB 工程 `BL_SIZE=0x4000`、`APP_START=0x4000`；`app.bin` 紧凑输出“文件偏移 0 = Flash `0x4000`”；解析 HEX type00/type04，过滤 `0x804000` 配置位；对 Reset 向量做 Thumb 位与区间校验，错误基址直接中止。
- **实测**：新 APP hex 区间 `0x4000–0x4A68`；`app.bin` SP=`0x2003FFF8`、Reset raw=`0x00004405`（entry `0x4404`，Thumb）。
- **状态**：**已解决**。

### C2 BL 8 KB → 16 KB、APP `0x2000` → `0x4000` 对齐
- **处置**：BL `btl.ld` ROM_START=0/ROM_SIZE=16384、RAM_START=`0x20000010`/RAM_SIZE=`0x3FFF0`；公共宏 `APP_START_ADDRESS=0x4000`；APP 构建宏 `ROM_ORIGIN=0x4000`。
- **实测**：最新 ROM `0x35F0` ≈ 84.3% of `0x4000`，余量有限。
- **状态**：**已解决**；新增代码需关注余量。

### C3 BankB 合并镜像（路线甲）
- **处置**：`--target bankb` 产出 `bankb_merged.bin`（off0=BL_B 16 KB 槽、off `0x4000`=APP_B），下载基址 `0x80000`；脚本对 BL_B `0x80000–0x83FFF`、APP_B 不越 `0xFE000`、总长不越 `0x100000` 做边界断言；`merge.bin` 与 `bankb_merged.bin` 布局相同（哈希一致），区别仅用途/下载基址。
- **状态**：**已解决（脚本层）**；含 BL_B 的完整 BankB 带 `-s` 启动尚未硬件闭环（见 D4/E 类）。

### C4 在线写活动 BL 区被否决
- **结论**：**已否决**。`-a 0x0 -b -f merge.bin` 在线擦写运行中 BL，无 ramfunc、擦写运行代码易产生半成品变砖；BL 更新走离线整片或 inactive Bank 预备。
- **状态**：**已否决**。

### C5 产物入库纪律
- **约定**：`_build/**`、`out/**`、`Tools\merged_out\**` 不入库；提交只精确 `git add` 指定源码/脚本，禁用 `git add -A`；避免把调试二进制与中间文件混入版本库。
- **状态**：流程约定，持续遵守。

---

## D. 双 Bank 行为类（`-a` / `-s` / AFIRST）

### D1 写 `0x84000`/`0x80000` 不带 `-s`，现象（LED）不变
- **根因**：不带 `-s` 发 `0xA3` 软复位，**不翻转 AFIRST、不切 Bank**；数据写入 inactive Bank，当前活动 BankA 继续运行。
- **结论**：写 inactive ≠ 启用 inactive；切换必须 `-s`。
- **状态**：**已澄清**（行为符合设计，非缺陷）。

### D2 `0xA4` 硬编码 BankB 侧，`-a 0x4000 -s` 属错配
- **事实**：`0xA4` 固定 `CRCGenerate(0x84000,0x7A000)` → 写 failsafe_B `0xFE000` → `NVMCTRL_BankSwap()`；不读 AFIRST、不跟随 `-a`。
- **错配示例**：`-a 0x4000 -f app.bin -s` 会更新 BankA，却给 BankB 记 CRC 并翻转切到 BankB，方向相反。
- **正确配对**：更新并留 BankA → `-a 0x4000`（不带 -s）；更新并切 BankB → `-a 0x84000 -s`。
- **状态**：**已澄清**；自适应改造见 D3。

### D3 往返切换（BankB → BankA）当前不可靠
- **限制**：`0xA4` 不识别当前活动 Bank，切到 BankB 后再用现有命令切回会语义错位。
- **待办（路线乙）**：读 `STATUS.AFIRST`，动态选择对面 Bank 的 APP/failsafe 地址；把 BL_B 纳入 CRC；支持可靠 A/B 往返。
- **状态**：**待实测/待开发**（未实施）。

### D4 BankB 空白或 CRC 不符时留在 BL
- **机制**：`BootCheck()` 先查 APP 首字，`0xFFFFFFFF` 直接 false；failsafe expected CRC 非空且不匹配也 false；expected 为空白 `0xFF` 时跳过 CRC。WDT 复位累加 retry，`retry_cnt>=3` 且 `bankswapped==0` 才写 failsafe 并 `BankSwap()` 一次；两侧都失败则留 BL。
- **排查含义**：带 `-s` 后回到 `50 03 07`，多为 APP_B 空白/无效或 failsafe_B CRC 不符，而非“运行了旧 APP_B”。
- **状态**：**机制已澄清**；具体现场需按命令文档第 9 节判别。

### D5 “先写 `0x4000` 再写 `0x84000` 就正常”的真因
- **结论**：让板子行为变化的是 `-a 0x4000`（更新活动 BankA）；`0x84000` 不带 `-s` 只写备用 Bank，对当前 LED 无贡献，易被误判为其生效。
- **状态**：**已澄清**。

---

## E. 跳转 / LED / 启动校验类（未完全闭环，如实记录）

### E1 APP 下载后 LED 不亮 / 现象不符
- **已知线索**：
  1. `run_Application(addr)` 只取 `msp=*(uint32_t*)addr`、`SYS_DeInitialize`、`__set_MSP`、跳转；**未显式设置 VTOR、未校验 reset vector 有效性**；APP 侧 `startup_xc32.c` Reset_Handler 自行设 `SCB->VTOR`，需确认先于中断使用。
  2. `BootCheck` 不通过会留 BL（BL 无 APP LED 行为）。
  3. `APP\config.mcc\src\main.c` 中 `LED2_Clear()` 曾被注释（1 行调试改动，未提交）；LED1=PC21、LED2=PA16，低电平有效（`Clear` 亮、`Set` 灭）。
- **判别方法**：复位后发 A6 帧：回 `50 03 07` = 留 BL；无响应且有 APP 行为 = 已跳出；调试器 halt 看 PC/向量落点与 `AFIRST`。
- **状态**：**待实测闭环**；不作为已解决项。

### E2 `BTL_TEMP_FORCE_STAY_IN_BL` 开关含义
- **事实**：=1 时 `BootCheck()` 恒返回 false、复位后强制留 BL；**不影响 BL 内接收升级帧**，便于纯 USB 链路验证；量产/正常跳 APP 前必须置 `0`（当前已为 0）。
- **状态**：**已澄清**；值见 `bootloader_common.c` L62。

### E3 复位后是否真正跳到 APP 的最小判别
- **建议序列**：
  1. `BTL_TEMP_FORCE_STAY_IN_BL=0` 且 APP 向量有效；
  2. 下载 `-a 0x4000 app.bin`（不带 -s）普通复位；
  3. 串口助手发 `4D 43 48 50 00 00 00 00 A6`；
  4. 有 `50 03 07` = 仍在 BL（查 BootCheck/E1）；无响应 = 已交 APP。
- **状态**：**待执行验证**。

### E4 时钟静默风险提示
- **经验**：MCC Clock Configurator 图形界面不报错时，生成代码也可能存在未锁定/空初始化；USB 枚举与通信异常需排查 DFLL48 锁定、USB 时钟源选择。本工程采用 DFLL48/GCLK0=48M/USB 48M、DPLL0 关闭；锁定位建议调试器确认。
- **状态**：**待实测**（低优先遗留项）。

---

## F. 工具链 / 工程管理类（历史经验提示，非本次新增缺陷）

### F1 非 0 地址 Flash 段静默漏写（VSCode/IPE 经验）
- **现象提示**：MPLAB VSCode 插件或 IPE Smart Programming 可能漏写非 0 地址 APP 段、只写 User Config；表现为编译烧录成功但跳转/行为异常。
- **绕行提示**：关键固件以 `btl_host.py` 脚本下载，或 IPE 明确指定编程范围；烧录后用脚本 READ_VERSION 与回读校验镜像一致性。
- **状态**：经验性提示，结合 F2 使用。

### F2 IPE 全片擦除覆盖 BL 的风险
- **提示**：避免 IPE 默认全片擦除反复整片烧录覆盖 BL；日常升级走脚本/inactive Bank，保护活动 BL。
- **状态**：流程约定。

### F3 MCC regenerate 覆盖人工修改
- **风险**：图形化重新生成可能覆盖人工编写文件。
- **处置**：生成前备份人工文件：`btl_channel.h`、`btl_cdc.c`、`bootloader_uart.c`、`main.c`、`initialization.c`、`Tools\*.py`；生成后 diff 核对并恢复被覆盖逻辑。
- **状态**：流程约定。

### F4 Python 脚本语法核对
- **约定**：改动脚本后 `python -m py_compile btl_host.py make_merged_bin.py`；固件 C 改动由开发者在 MPLAB 编译验证，助手不代编译。
- **状态**：流程约定。

---

## G. 待办清单（汇总）

1. 恢复 `0xA2 VERIFY` 主机 CRC 校验与应答等待（B4）。
2. `0xA3/0xA4` 补 ACK/失败判定（B5）。
3. 路线乙：`AFIRST` 自适应、对面 Bank 动态地址、BL_B 纳入 CRC、可靠往返切换（D3）。
4. 现场闭环 APP 跳转与 LED：按 E3 序列判别，必要时补 `run_Application` 的 VTOR/向量校验（E1）。
5. 调试器确认 DFLL/USB 时钟锁定（E4）。
6. 完整 BankB（含 BL_B）镜像 `-a 0x80000` 预备 + `-a 0x84000 -s` 切换的硬件实测（C3/D4）。

## H. 常用复现命令（在 `Tools` 目录）

```bat
:: 生成全部产物
python make_merged_bin.py --target all

:: 更新并留在 BankA（不切换）
python btl_host.py -i COM5 -d pic32cx_sg41 -a 0x4000 -f merged_out\app.bin

:: 预备完整 BankB 镜像（不切换）
python btl_host.py -i COM5 -d pic32cx_sg41 -a 0x80000 -f merged_out\bankb_merged.bin

:: 更新 BankB APP 并切换到 BankB
python btl_host.py -i COM5 -d pic32cx_sg41 -a 0x84000 -f merged_out\app.bin -s

:: 详细输出排查
python btl_host.py -v -i COM5 -d pic32cx_sg41 -a 0x4000 -f merged_out\app.bin
```
