# 智能联网时钟系统 — HW26-0672

学生版大作业 V1.3 | 学号 20260001 | 姓名 HU ZHENYE

---

## 项目结构

```
hw26-0672/
├── README.md                    ← 本文件
├── CLAUDE.md                    ← 开发辅助文档
├── SPEC_COMPLIANCE_CHECKLIST.md ← 规格对照清单
├── mcu/
│   ├── main.c                   ← **全部自编代码集中于此** (~2400行)
│   ├── src/                     ← 课程提供模块
│   ├── driverlib/               ← 驱动库
│   ├── Objects/S800.axf         ← 编译输出（可烧写）
│   └── RTE/                     ← 启动文件
└── pc_host/
    ├── virual_twin_panel.py     ← PC上位机（PyQt5 数字孪生面板）
    ├── test_runner.py           ← 串口协议自动测试+评分器
    ├── testcmd.txt              ← 测试命令集(100条)
    ├── snake_game.py            ← 贪吃蛇游戏 (MCU按键操控+PC渲染)
    ├── ntp_helper.py            ← NTP 对时模块
    ├── weather_helper.py        ← 天气获取模块
    ├── requirements.txt         ← Python 依赖
    └── events.csv               ← 事件日志（运行时生成）
```

---

## 编译与运行

### S800 板端（MCU）

1. Keil MDK 打开 `mcu/S800.uvprojx`
2. Project → Rebuild all target files
3. F8 烧写到 S800 开发板（TM4C1294NCPDT）
4. 烧写后自动运行，无需连接 PC

### PC 端

```bash
cd pc_host
.venv\Scripts\activate
pip install -r requirements.txt
python virual_twin_panel.py
```

### 自动化测试

```bash
python test_runner.py --student-id 20260001 --port COM9
```

---

## S800 板端功能

### §3.1 开机画面 ✅

8位SEG+8位LED全亮→全灭 ≥1次 → 学号 `20260001` 闪烁1次 (LED跟随闪烁) → 姓名 `HUZHENYE` 闪烁1次 (LED跟随闪烁) → 版本号 `V1.0` 持续1.5s → 进入时钟

### §3.2 时钟日期显示 ✅

- 时钟源: 外部 25MHz 晶振 (SYSCTL_XTAL_25MHZ | SYSCTL_OSC_MAIN)，UART 115200 波特率精准
- 默认 HH.MM.SS → DISP键循环切换: YY.MM.DD → YYYY.MMDD → HH.MM.SS
- 闰年 02/29 → 03/01 正确，月末进位正确
- SysTick 1ms时基，秒不丢
- `*SET:DISPLAY OFF/ON` 熄屏/恢复
- 消鬼影三段式刷新 (全灭→数据→选通)，显示无残影

### §3.3 流水显示 ✅

- >8字符自动滚动，≤8字符静态显示2.5秒后自动返回
- 所有MSG路径共用 `scroll_buf`，memset 清空防残留
- FORMAT键 + `*SET:FORMAT` 切换 LEFT/RIGHT
- SPEED键 2级速度 (500ms/250ms)，滚动途中可切换
- FORMAT RIGHT 时小数点跟随反转

### §3.4 闹钟 ✅

- PK5 M0PWM7 PWM硬件驱动 2kHz/50%占空比
- 300ms 响 / 300ms 停 节奏式响铃，≤10秒自停
- FUNC键立即停止
- 天气联动: 雨雪多响3声，高温 8LED 慢闪
- LED1：使能=常亮 / 响铃=快闪
- *SET:ALARM [MON-SUN] HOUR/MIN/SEC 设置指定星期
- *SET:ALARM ON/OFF 使能/禁用当天
- *GET:ALARM 返回7槽完整数据
- 无DOW参数默认当天，PC每行独立设置+单日清除按钮

### §3.5 编辑状态机 ✅

- FUNC 循环: EDIT_DATE → EDIT_TIME → EDIT_ALARM → EDIT_DATE（环形, 不自动退出）
- SHIFT 切换字段（长日期年份闪烁4位，短日期闪烁2位）
- ADD 当前字段+1（带范围钳制）
- SAVE / 长按FUNC(≥1s) 保存退出
- 退出方式：仅 SAVE 或 FUNC长按（无超时自动退出）
- 编辑备份/恢复: 进入时备份，退出时恢复

### §3.6 LED 指示 ✅

| 位 | 名称 | 含义 | 行为 |
|----|------|------|------|
| LED0 (0x01) | ❤️ 心跳 | 系统运行 | 1Hz闪 |
| LED1 (0x02) | ⏰ 闹钟 | 使能+响铃 | 使能常亮 / 响铃快闪 |
| LED2 (0x04) | ✏️ 编辑 | 编辑模式 | 编辑中常亮 |
| LED3 (0x08) | 📤📥 UART | TX+RX合并 | 活动后亮100ms |
| LED4 (0x10) | ☀️ 晴天 | 天气SUN | 晴天常亮 |
| LED5 (0x20) | 🌧️ 雨雪 | RAI/SNO | 1Hz呼吸 |
| LED6 (0x40) | 🔥 高温 | ≥30°C | 高温常亮 |
| LED7 (0x80) | 🕐 NTP | 同步状态 | SYNCED=常亮/DRIFT=1Hz闪/UNSYNCED=灭 |

> 接管模式: `*SET:LED <hex>` 直接控制，00 或 *RST 退出，10s 超时自动退出。

### §3.7 按键映射 ✅

消抖 20ms，长按 ≥800ms。USER1/USER2 统一状态机消抖+长按。

| 按键 | 位置 | 短按 | 长按 |
|------|------|------|------|
| ADD | K0 | 当前字段+1 | — |
| FUNC | K1 | 编辑模式循环 / 关闹钟 | 保存并退出 |
| SHIFT | K2 | 切换编辑字段 | — |
| SPEED | K3 | 流水速度切换 | — |
| SAVE | K4 | 保存退出 | — |
| FORMAT | K5 | 方向切换 LEFT/RIGHT | — |
| DISP | K6 | 时间/短日期/长日期切换 | — |
| EXT | K7 | 闹钟使能/禁用切换 | — |
| USER1 | PJ0 | 请求PC NTP对时 | NTP状态查询(n.SY.xx) |
| USER2 | PJ1 | 5秒显示天气+温度 | — |

### 蜂鸣器

- 型号: PS1720P02 (C96061) 无源压电式
- 驱动: **PK5 M0PWM7**，PWM0 Gen3 Out7 硬件 2kHz/50% 占空比
- 闹钟: 300ms 响 / 300ms 停 对称切换 (PWMOutputState 零延迟开关)
- 远程: `*SET:BEEP 10-5000` (ms)，超时后持久静音 watchdog
- 持久静音: `beep_force_off` 每 100ms 反复 Beep_Off，*RST 后不取消

### 7段数码管字体

支持 0-9、A-Z、小写 c/e/h/i/j/l/n/o/r/u、`-` `_` `.` `°` 共 50 个字符

---

## 命令解析架构

### ExtractLine — 行整理

非 MSG 命令丢弃所有空格，命令转大写。MSG 保留大小写和空格。

```
*SET:TIME HOUR MIN SEC 14 30 00 → *SET:TIMEHOURMINSEC143000
```

### ProcessCommand — 2-Pass 内联解析

DATE/TIME/ALARM 三个 handler 采用 **2-pass + cmd_match 前缀匹配**:

```
Pass 1: char *q 副本指针扫关键词 → cmd_match匹配 → 建 kmap bitmask
Pass 2: char *p 原指针逐字符扫数字 → v=v*10+(*p++-'0')内联解析 → vals[]
Map:    按 kmap 顺序 vals → 目标变量 + 默认值填充
```

所有数字解析使用内联 `while(*p>='0'){v=v*10+(*p++-'0')}`，**零次 strtol 调用**，根除 ARMCC5 C89 `strtol(t, &t, 10)` 寄存器缓存截断缺陷。

其他 SET handler (DISP/FORMAT/MSG/BEEP/LED/KEY/MODE/WEA/GAME) 用单 pass cmd_match。

### 缩写支持

`cmd_match` 前缀匹配 + `skip_kw_rest` 跳过缩写后缀:
- MIN → MINUTE (skip "UTE")
- SEC → SECOND (skip "OND")
- DISP → DISPLAY (skip "LAY")
- WEA → WEATHER (skip "THER")

非法缩写自动报错: MONT ≠ MONTH, MI < MIN → ERROR SYNTAX

---

## 串口协议（15命令 + 8事件）

```
波特率: 115200, 8N1, 无流控
行格式: 命令以 \r\n 结束，大小写不敏感
缩写:   cmd_match 前缀匹配 + skip_kw_rest 后缀跳过
空格:   允许多空格及冒号前空格
错误:   ERROR SYNTAX / PARAM / RANGE / LINE TOO LONG
编译:   #pragma O0 文件级禁用优化 + volatile 关键变量
```

### PC→MCU 命令（15条）

| 命令 | 参数 | 说明 |
|------|------|------|
| `*RST` | [DATE/TIME/ALARM] | 复位，退出LED接管 |
| `*SET:DATE` | YEAR/MONTH/DATE val | 设置日期 (2-pass解析) |
| `*SET:TIME` | HOUR/MIN/SEC/OFF | 设置时间 (2-pass解析) |
| `*SET:ALARM` | [DOW] HOUR/MIN/SEC/ON/OFF | 闹钟设置 (2-pass解析, 无DOW=当天) |
| `*SET:DISPLAY` | ON/OFF | 显示开关 |
| `*SET:FORMAT` | LEFT/RIGHT | 方向设置 |
| `*SET:MSG` | text ≤32字节 | 滚动消息 (保留大小写) |
| `*SET:BEEP` | 10-5000ms | 远程蜂鸣 (持久静音watchdog) |
| `*SET:LED` | hex2 (00=退出接管) | LED接管 (10s自动退出) |
| `*SET:KEY` | NAME | 虚拟按键 |
| `*SET:MODE` | DAY/NIGHT | 昼夜模式 |
| `*SET:WEA` | temp code | 天气数据 |
| `*SET:GAME` | START/SCORE n/OVER n/QUIT | 贪吃蛇游戏控制 |
| `*GET` | TIME/DATE/FORMAT/ALARM/DISP/MODE | 查询 |
| `*PING` | — | 心跳 |

### MCU→PC 事件（8种）

| 事件 | 触发 | 说明 |
|------|------|------|
| `*EVT:DISP <8char> <dpHex>` | 1Hz | 显示同步 |
| `*EVT:LED <hex2>` | 1Hz | LED同步 |
| `*EVT:KEY <NAME>` | 按下 | 物理按键 (*SET:KEY不环回) |
| `*EVT:ALARM` / `*EVT:ALARM_OFF` | 触发/停止 | 闹钟事件 |
| `*EVT:EDIT <TYPE> <VALUE>` | 保存 | 编辑保存 |
| `*EVT:MODE <STATE>` | 切换 | 模式切换 |
| `*PONG <uptime>` | 应答 | PING响应 |

---

## PC 上位机

### P1 串口管理 ✅
自动扫描COM口、波特率选择、1Hz 自动 *PING、3s 超时离线检测+自动重连。状态栏四项: 连接/FORMAT/MODE/ALARM使能。

### P2 控制面板 ✅ — 7个标签页

| 标签页 | 功能 |
|--------|------|
| 📅 日期时间 | SpinBox + 参数组合下拉框(YEAR/MONTH/DATE) + 缩写演示按钮 |
| ⏰ 闹钟调度 | 7天网格(Mon-Sun)，独立时间+使能，每行设置/清除，应用全部/复制/清除 |
| 🖥️ 显示控制 | 3模式按钮、方向、速度、昼夜、LED hex、蜂鸣ms |
| 📝 滚动消息 | 文本输入(≤32字)、字数统计、8位预览 |
| ⚡ 快捷操作 | *RST(确认框)、PING、演示序列 |
| 🔌 扩展功能 | NTP对时、天气获取、自动昼夜、数据图表 |
| 🐍 贪吃蛇 | 20×15网格，MCU按键操控，PC渲染 |

### P4 数字孪生 ✅
8位7段数码管 + 8位LED + 10键虚拟按键，1:1镜像MCU。NIGHT模式仅显4位。小数点独立占位渲染。USER1/USER2 自动触发 NTP/天气。

### P5 收发日志 ✅
时间戳、颜色编码（发送蓝/应答绿/事件紫/错误红）、最大1000行、可导出

---

## 扩展功能（E1-E4）✅ 全部完成

| 编号 | 功能 | 分值 | 实现 |
|------|------|------|------|
| **E1** | NTP 网络对时 | 2分 | ntp.aliyun.com → *SET:TIME，USER1自动触发 |
| **E2** | 天气获取 | 3分 | wttr.in 免费API → *SET:WEA，LED4-6指示，USER2短显 |
| **E3** | 自动昼夜 | 2分 | astral 日出日落 → *SET:MODE DAY/NIGHT，60秒轮询 |
| **E4** | 数据可视化 | 1分 | events.csv → matplotlib 图表 |
| **§4.3** | 多闹钟调度器 | 8分 | 7槽位(Mon-Sun)独立设置+PC网格管理 |

---

## P4 数字孪生打磨 ✅

| 功能 | 实现 |
|------|------|
| 夜间模式4位限制 | `*EVT:MODE NIGHT` → PC SEG仅前4位 |
| 编辑高亮镜像 | PC追踪FUNC/SHIFT/SAVE按键 |
| 小数点独立占位 | '.' 段码 0x80，dp圆点渲染 |

---

## 🐍 贪吃蛇游戏 ✅

MCU 物理按键操控 + PC PyQt5 渲染 + MCU 7-SEG 分数同步。

| 按键 | 游戏功能 |
|------|---------|
| ADD | 蛇左转 |
| FUNC | 蛇下转 |
| SHIFT | 蛇右转 |
| DISP | 蛇上转 |
| USER1 | 暂停/继续 |

---

## ARMCC5 C89 编译器优化缺陷修复 (2026-06-27)

ARM Compiler 5 V5.06 C89 模式下，优化器在多路径解析函数中产生寄存器缓存缺陷。修复历程:

| 尝试 | 方案 | 结果 |
|------|------|------|
| 1 | volatile `char *t` 局部变量 | 局部 volatile 被 ARMCC5 忽略 |
| 2 | `for + strlen` 替换 `while(*ptr)` | 引入 `strlen` 调用改变栈布局 → 新缺陷 |
| 3 | 文件域 `volatile char *g_pp` | 不够强，仍有缓存 |
| 4 | `#pragma O0` 文件级 | **无优化即无缺陷** ✅ |
| 5 | `strtol(t, &t, 10)` → 内联 `while(*p>='0'){v=v*10+(*p++-'0')}` | **根除 `&t` 写回缺陷** ✅ |

**最终方案**: `#pragma O0` 文件级禁用优化 + 2-pass 内联数字解析 (零次 strtol)。稳定、可预测、无缓存缺陷。

### 峰鸣器稳定性修复系列

| 问题 | 根因 | 修复 |
|------|------|------|
| 闹钟声音高低不稳 | PWMGenDisable 停在高电平→引脚锁HIGH | Beep_Off 先 PWMOutputState(false) 再 Disable |
| BEEP 超时不停 | `remote_beep_end_ms` 非 volatile→缓存 | 改为 volatile + 双检查点 |
| *RST 打断静音 | `beep_force_off=0` 杀 watchdog | 改为 `Beep_Off()` + `beep_force_off=1` |
| 持久静音 | 偶尔单次 Beep_Off 不够 | `beep_force_off` 每 100ms 循环 Beep_Off |

---

## 稳定性修复记录

| 问题 | 修复 | 提交 |
|------|------|------|
| I2C总线竞态→幽灵按键 | SysTickIntDisable + while(I2CMasterBusy) | f476396 |
| 栈溢出→命令损坏 | 文件域静态缓冲区 | f476396 |
| edit_exit 无条件恢复 | `else if (edit_mode != EDIT_NONE)` | f476396 |
| 闹钟 *RST 后立即触发 | 默认 06:00:00 | f476396 |
| 行溢出静默丢弃 | ERROR LINE TOO LONG | f476396 |
| 按键双触发 | i2c_keys 取反 | a5d26d4 |
| FORMAT RIGHT dp 不反转 | dp_buf 跟随字符串反转 | a5d26d4 |
| 段码位序错误 | 反转后计算 | 1f5d807 |
| 蜂鸣器 PK5 PWM | PWM0 Gen3 Out7 2kHz | 140f130 |
| 按键扫描幻影 KEV_UP | 统一状态机 + KS_DEBOUNCE_UP | 69a6fe5 |
| 滚动中 SPEED 误退出 | `ke == KEV_DOWN` 守卫 | 6305de6 |
| 命令解析 2-pass + 内联 | cmd_match + 去空格 ExtractLine + 零 strtol | 54b3cd1 |
| BEEP 持久静音 watchdog | beep_force_off 循环 | e98e636 |

---

## 项目进度

| 阶段 | 任务 | 状态 |
|------|------|------|
| 第1周 | 底层驱动、开机画面、时钟走时 | ✅ |
| 第2周 | 闹钟、编辑FSM、串口协议全集 | ✅ |
| 第3周 | PC数字孪生、P2控制面板、E1-E4扩展 | ✅ |
| 已完成 | §4.3 多闹钟调度器 | ✅ |
| 已完成 | P4 打磨 (编辑高亮镜像、夜间4位) | ✅ |
| 已完成 | 贪吃蛇游戏 | ✅ |
| 已完成 | 2-pass 内联解析 + ARMCC5 编译器适配 | ✅ |
| 已完成 | 蜂鸣器 PK5 PWM + 持久静音 watchdog | ✅ |
| 已完成 | 按键统一状态机 + 消抖释放 | ✅ |
