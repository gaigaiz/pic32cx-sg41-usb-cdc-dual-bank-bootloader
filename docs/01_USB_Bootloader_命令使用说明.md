# USB CDC Bootloader 命令使用说明（BankA / BankB 下载与切换）

> 适用工程：本仓库根目录（USB CDC 虚拟串口通道）
> 芯片：PIC32CX1025SG41128；固件 Bootloader 版本 v3.7；上位机 `Tools\btl_host.py`
> 角色：MCU = USB Device（CDC），PC = Host（虚拟 COM 口，示例 COM5）
> 通道选择：编译期 `BTL_USE_CDC=1` 走 USB CDC；`=0` 回退 SERCOM4 串口（另一套旧工程口径，地址不同，勿混用）

> 注意：早期 SERCOM4 工程为 8KB BL、APP `0x2000`；本文是当前 USB 工程（16KB BL、APP `0x4000`），地址以本文为准。

---

## 1. 一句话模型

- **`-a` 决定把数据写到哪个物理 Flash 地址（固定，不随 Bank 角色变化）。**
- **`-s` 只决定末尾发哪条收尾命令**：不带 `-s` 发 `0xA3` 普通复位（不切 Bank）；带 `-s` 发 `0xA4` Bank 切换复位。
- **复位后实际跑哪个 Bank，由硬件 `STATUS.AFIRST` 决定**，写 Flash 本身不改变活动 Bank。

---

## 2. 当前 Flash 物理布局（1 MB = 两个 512 KB Bank）

页大小 512 B；擦除块 8192 B（0x2000）。

| 物理区域 | 起始 | 结束 | 大小 | 说明 |
|---|---:|---:|---:|---|
| BL_A | `0x00000000` | `0x00003FFF` | 0x4000（16 KB） | 活动 Bank 默认 BL |
| APP_A | `0x00004000` | `0x0007DFFF` | 0x7A000 | BankA APP 段 |
| failsafe_A | `0x0007E000` | `0x0007FFFF` | 0x2000 | BankA CRC/版本/重试计数 |
| BL_B | `0x00080000` | `0x00083FFF` | 0x4000（16 KB） | BankB BL 副本槽（路线甲镜像层铺入） |
| APP_B | `0x00084000` | `0x000FDFFF` | 0x7A000 | BankB APP 段 |
| failsafe_B | `0x000FE000` | `0x000FFFFF` | 0x2000 | BankB CRC/版本/重试计数 |

关键固件常量（`bootloader_common.h`）：`APP_START_ADDRESS=0x4000`、`INACTIVE_BANK_START=0x80000`、`APP_REGION_SIZE=0x7A000`、`FAILSAFE_SECTOR_ADDR=0x7E000`。

---

## 3. 先在上位机目录生成镜像

工作目录：`Tools/`

```bat
python make_merged_bin.py --target all
```

`--target` 可选值与产物（输出到 `Tools\merged_out\`）：

| target | 产物 | 内容与用途 |
|---|---|---|
| `app`（默认） | `app.bin` | 紧凑 APP 镜像，文件偏移 0 = Flash `0x4000`；用于下载 APP_A / APP_B |
| `bl` | `bl.bin` | 固定 16 KB BL 镜像（FF 填充），文件偏移 0 = Flash `0x0` |
| `merged` | `merge.bin` | Bank 相对合并镜像（BL@off0、APP@off0x4000），BankA 整片存档用 |
| `bankb` | `bankb_merged.bin` | 同布局合并镜像，用于 BankB 起点 `0x80000` 预备（BL_B + APP_B） |
| `all` | 以上全部 | 一次生成四个文件 |

当前实测产物：`bl.bin`=16384 B；`app.bin`=2664 B（SP `0x2003FFF8`、Reset Thumb 且落在 `0x4xxx`）；`merge.bin`/`bankb_merged.bin`=24576 B（0x6000）。脚本会校验 Reset 向量 Thumb 位与区间，非法基址直接报错不产出。

---

## 4. `btl_host.py` 参数（常用项）

| 参数 | 含义 | 备注 |
|---|---|---|
| `-i COM5` | 串口/CDC 端口名 | 以设备管理器实际 COM 号为准 |
| `-d pic32cx_sg41` | 设备名 | 设备表 `[ERASE_SIZE=8192, BOOTLOADER_SIZE=16384, DEV_CFG_SUPPORT=True]` |
| `-a ADDR` | **写入起始物理地址（必填）** | `0x4000`=APP_A；`0x84000`=APP_B；`0x80000`=BankB 起点合并镜像 |
| `-f FILE` | 待下载 raw bin | 脚本自动补 FF 到 8 KB 整数倍 |
| `-s` | 末尾发 `0xA4 BKSWAP_RESET` 并切换 Bank | 不带则发 `0xA3 RESET` |
| `-b` | 允许写 BL 保护区（`address < 0x4000` 时才需要） | **不要用于在线写活动 BL**，见第 7.6 节 |
| `-v` | 详细输出 | 当前标准阶段默认打印，`-v` 仅附加 devcfg/调谐细节 |
| `-r 115200` | 波特率 | CDC 虚拟串口仅占位，不影响实际带宽 |

---

## 5. 两条收尾命令的固件行为（核心）

### 5.1 不带 `-s` → `0xA3 RESET`（`bootloader_uart.c`）
1. 回 `0x50`；
2. `btl_io_tx_flush()`；
3. `bootloader_TriggerReset()`（软复位）。
- **不调用 `NVMCTRL_BankSwap()`，`AFIRST` 不变，活动 Bank 不变。**

### 5.2 带 `-s` → `0xA4 BKSWAP_RESET`（`bootloader_uart.c` L320–335）
1. 回 `0x50` 并 flush；
2. `CRCGenerate(0x84000, APP_REGION_SIZE)` 对 **BankB APP 段**现场算 CRC；
3. `RecordAppCrc()` 把该 CRC 写入 **failsafe_B `0xFE000`**（版本+1、清重试计数）；
4. `NVMCTRL_BankSwap()`：发 `BKSWRST`，翻转 `STATUS.AFIRST` 并复位。

> 注意：`0xA4` 第 2 步**固定读 `0x84000`、写 `0xFE000`**，不跟随 `-a` 地址；当前固件只适配“默认 active=BankA，升级并切到 BankB”的单向流程。

### 5.3 复位后如何运行
硬件按 `AFIRST` 把某一 Bank 映射到逻辑 `0x0`；BL 先跑，`initialization.c` 中 `bootloader_Trigger()==false && bootloader_BootCheck()==true` 时 `WDT_Disable()` 后 `run_Application(APP_START_ADDRESS)`。`BootCheck` 用逻辑固定偏移 `0x4000`/`0x7E000`，由硬件重映射指向当前活动 Bank 的内容。

---

## 6. “写入地址”与“运行 Bank”解耦（避免最常见误解）

| 操作 | 写入物理位置 | 不带 `-s` 复位后 | 带 `-s`（默认 active=BankA）复位后 |
|---|---|---|---|
| `-a 0x4000 -f app.bin` | BankA APP `0x4000` | 跑新 BankA（立即生效） | 写入 BankA，却翻转切到 BankB（**错配，不推荐**） |
| `-a 0x84000 -f app.bin` | BankB APP `0x84000` | 仍跑 BankA，LED/行为不变 | 记 BankB CRC 后切到 BankB 运行（**配对正确**） |
| `-a 0x80000 -f bankb_merged.bin` | BankB 起点 BL_B+APP_B | 仍跑 BankA，仅预备 BankB | 切到 BankB；`0xA4` 只校验 `0x84000` APP 段，BL_B 不纳入 CRC |

要点：**`0x4000` 一写就变，是因为它就是当前活动 Bank；`0x84000` 写了不变，是因为当前 CPU 不读它；要让 BankB 生效必须 `-s` 切换。**

---

## 7. 常用命令清单

### 7.1 连通性确认（脚本要求必须给 `-a`；也可用串口助手手动发帧，见第 10 节）
```bat
python btl_host.py -i COM5 -d pic32cx_sg41 -a 0x4000 -f merged_out\app.bin
```
> 仅看版本可直接用串口助手发 READ_VERSION 帧；脚本本身会先读版本再进入下载。

### 7.2 更新并留在 BankA（当前活动 Bank，最常用、安全，不带 `-s`）
```bat
python btl_host.py -i COM5 -d pic32cx_sg41 -a 0x4000 -f merged_out\app.bin
```

### 7.3 预备 BankB 完整镜像（BL_B + APP_B，只写不切，不带 `-s`）
```bat
python btl_host.py -i COM5 -d pic32cx_sg41 -a 0x80000 -f merged_out\bankb_merged.bin
```

### 7.4 更新 BankB APP 并切换到 BankB（配对正确，带 `-s`）
```bat
python btl_host.py -i COM5 -d pic32cx_sg41 -a 0x84000 -f merged_out\app.bin -s
```

### 7.5 完整 BankB 两阶段（先铺 BL_B 预备，再写 APP_B 并切换）
```bat
python btl_host.py -i COM5 -d pic32cx_sg41 -a 0x80000 -f merged_out\bankb_merged.bin
python btl_host.py -i COM5 -d pic32cx_sg41 -a 0x84000 -f merged_out\app.bin -s
```

### 7.6 禁止/不推荐
- 不要 `python btl_host.py -i COM5 -d pic32cx_sg41 -a 0x0 -b -f merge.bin` 在线写活动 BL：无 ramfunc、擦写运行中代码易变砖；合并镜像的 BL 槽只用于离线整片或 inactive Bank。
- 不要用 `-a 0x4000 -s` 期望“更新并启用 BankA”：默认 active=BankA 时它会切去 BankB。

---

## 8. 不带 `-s` 与带 `-s` 对照

| 维度 | 不带 `-s`（`0xA3`） | 带 `-s`（`0xA4`） |
|---|---|---|
| 固件动作 | 软复位 | CRC `0x84000` → 写 `0xFE000` → BankSwap 翻转复位 |
| `AFIRST` | 不变 | 翻转一次 |
| 活动 Bank | 不变 | 默认 BankA → BankB |
| failsafe_B 记录 | 不写 | 写入新 APP_B 的 expected CRC |
| 典型用途 | 更新当前活动 Bank；预备备用 Bank | 升级 BankB 并切到 BankB 运行 |

---

## 9. 如何确认到底跑在哪个 Bank / 是否切成功

1. **串口 READ_VERSION 判别**（复位后先不跑脚本，串口助手发第 10 节 A6 帧）：
   - 回 `50 03 07`：仍停在 BL（可能 BankB `BootCheck` 未过、failsafe CRC 不符、或 BL_B/APP_B 空白无效）。
   - 无响应 / COM 重新枚举 / 出现 APP 行为（LED）：已跳出 BL 进入 APP。
2. **调试器（最可靠）**：读 `NVMCTRL.STATUS` 的 `AFIRST`（位 4），`BankSwap` 后应翻转；halt 后看向量/PC 落点。
3. **现象对照**：APP 预期 LED 行为以 `APP\config.mcc\src\main.c` 为准（LED1=PC21、LED2=PA16，低电平点亮）。

`BootCheck` 留 BL 的常见原因：APP 首字为 `0xFFFFFFFF`（空白）；failsafe expected CRC 非空但与实测不符；WDT 连续失败触发回退。failsafe 扇区为空白（`0xFF`）时跳过 CRC 比对。

---

## 10. 帧格式速查（串口助手手动 HEX 调试用）

- 帧头：`GUARD=0x5048434D`，小端字节 `4D 43 48 50`；帧头 = GUARD(4) + SIZE(4) + CMD(1) = 9 字节。
- 响应：`0x50` OK；`0x51` UNLOCK/DATA 拒绝；`0x52` 未知命令；`0x53` CRC OK；`0x54` CRC FAIL。

| 命令 | 用途 | 手动 HEX 参考 |
|---|---|---|
| `0xA6` READ_VERSION | 读版本，回 `50 03 07` | `4D 43 48 50 00 00 00 00 A6` |
| `0xA0` UNLOCK | 解锁 [begin,end) | SIZE=8；后跟 begin(4)+end(4) |
| `0xA1` DATA | 写一块（8192 B） | SIZE=8196；后跟 addr(4)+数据 |
| `0xA3` RESET | 普通复位，不切 Bank | SIZE=16；后跟 4 个 0 |
| `0xA4` BKSWAP_RESET | 记 BankB CRC + 切换复位 | SIZE=16；后跟 4 个 0 |

---

## 11. 当前临时态与已知限制（验证前必读）

1. 脚本临时跳过 `0xA2 VERIFY`（终端显示 `Verification skipped (USB‑CDC workaround)`）；`0xA3/0xA4` 发出后不等 ACK。因此“进度 100%”只代表帧发出、DATA 块被 ACK，**不代表 CRC 通过或切换成功**，以第 9 节判别为准。
2. `0xA4` 硬编码 `0x84000/0xFE000`，不读 `AFIRST`、不识别对面 Bank；**往返切换（BankB→BankA）当前固件不可靠**，需要后续路线乙（读 AFIRST 自适应对面 Bank 地址、把 BL_B 纳入校验）。
3. BL_B 暂不纳入 `0xA4` CRC；路线甲先保证 APP_B 校验。
4. `BTL_TEMP_FORCE_STAY_IN_BL` 必须为 `0`，否则 `BootCheck` 恒 false、永远留在 BL。
5. 产物 `merged_out\*.bin`、`_build/**`、`out/**` 不纳入 git；提交只精确 `git add` 源码/脚本，禁用 `git add -A`。

---

## 12. 快速排错

| 现象 | 优先排查 |
|---|---|
| 脚本 `no response received, giving up` | CDC 是否枚举为 COM；RX 是否被突发灌爆（见问题记录文档 B 类）；DATA 块是否收到 `0x50` |
| 写 `0x84000` 后 LED 不变 | 是否漏 `-s`；不带 `-s` 本就不切 Bank |
| 带 `-s` 后回到 `50 03 07` | BankB `BootCheck` 失败：APP_B 空白/CRC 不符/BL_B 未就绪；先确认 `0x84000` 有效镜像 |
| 切过去后 APP 行为异常 | 向量基址、`SCB->VTOR`、APP 时钟/LED 初始化；参考开发流程文档第 5、10 节 |
| 设备管理器无 COM / Code43 | 枚举类问题，见《03_问题与解决记录》A 类 |
