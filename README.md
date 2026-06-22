# 智能联网时钟系统 — HW26-0672

学生版大作业 V1.2 | 学号 20260001 | 姓名 HU ZHENYE

---

## 项目结构

```
hw26-0672/
├── README.md                    ← 本文件
├── CLAUDE.md                    ← 开发辅助文档
├── mcu/
│   ├── main.c                   ← **全部自编代码集中于此**
│   ├── src/                     ← 课程提供模块
│   ├── driverlib/               ← 驱动库
│   ├── Objects/S800.axf         ← 编译输出（可烧写）
│   └── RTE/                     ← 启动文件
└── pc_host/
    ├── virual_twin_panel.py     ← PC上位机（PyQt5 数字孪生面板）
    ├── ntp_helper.py            ← NTP 对时模块
    ├── weather_helper.py        ← 天气获取模块
    ├── requirements.txt         ← Python 依赖
    └── events.csv               ← 事件日志（运行时生成）
```

---

## 编译与运行

### S800 板端（MCU）

1. Keil MDK 打开 `mcu/S800.uvprojx`
2. 编译 → 烧写到 S800 开发板（TM4C1294NCPDT）
3. 烧写后自动运行，无需连接 PC

### PC 端

```bash
cd pc_host
.venv\Scripts\activate
pip install -r requirements.txt
python virual_twin_panel.py
```

---

## S800 板端功能

### §3.1 开机画面 ✅

8位SEG+8位LED全亮→全灭 ≥1次 → 学号 `20260001` 闪烁1次 → 姓名 `HUZHENYE` 闪烁1次 → 版本号 `V1.0` 持续1s → 进入时钟

### §3.2 时钟日期显示 ✅

- 默认 HH.MM.SS → DISP键循环切换: YY.MM.DD → YYYY.MMDD → HH.MM.SS
- 闰年 02/29 → 03/01 正确，月末进位正确
- SysTick 1ms时基，秒不丢
- `*SET:DISPLAY OFF/ON` 熄屏/恢复

### §3.3 流水显示 ✅

- >8字符自动滚动，≤8字符静态2.5秒后自动返回
- FORMAT键 + `*SET:FORMAT` 切换 LEFT/RIGHT
- SPEED键 2级速度 (500ms/250ms)
- FORMAT RIGHT 时小数点跟随反转

### §3.4 闹钟 ✅

- 时分秒匹配触发 → 蜂鸣器 2kHz PWM 节奏响铃
- 响200ms→停200ms→响200ms→停200ms 循环，≤10秒自停
- FUNC键立即停止
- LED1：使能=常亮 / 响铃=快闪
- `*SET:ALARM [slot] HOUR/MIN/SEC/OFF/ON` 远程设定
- **多闹钟调度器**：7槽位（周一~周日）每天独立时间+使能
- `*SET:ALARM MON HOUR 8 MIN 30 SEC 0` 设置周一，`ALL` 全部
- `*GET:ALARM` 返回7槽完整数据
- EXT键 切换当前日闹钟使能/禁用

### §3.5 编辑状态机 ✅

- FUNC 循环: EDIT_DATE → EDIT_TIME → EDIT_ALARM → EDIT_DATE（环形，不自动退出）
- SHIFT 切换字段（长日期年份闪烁4位，短日期闪烁2位）
- ADD 当前字段+1（带范围钳制）
- SAVE / 长按FUNC(≥1s) 保存退出
- 退出方式：仅 SAVE 或 FUNC长按（无超时自动退出）

### §3.6 LED 指示 ✅

| 位 | 名称 | 含义 | 行为 |
|----|------|------|------|
| LED0 (bit0) | ❤️ 心跳 | 系统运行 | 1Hz闪（500ms/500ms） |
| LED1 (bit1) | ⏰ 闹钟 | 使能+响铃 | 使能常亮 / 响铃200ms快闪 |
| LED2 (bit2) | ✏️ 编辑 | 编辑模式 | 编辑中常亮 |
| LED3 (bit3) | 📤 TX | UART发送 | 发送后亮300ms |
| LED4 (bit4) | 📥 RX/🕐 NTP | 接收/NTP同步 | RX=亮300ms / NTP=常亮 |
| LED5 (bit5) | ☀️ 晴天 | 天气SUN | 晴天常亮 |
| LED6 (bit6) | 🌧️ 雨雪 | 天气RAI/SNO | 雨雪时常亮 |
| LED7 (bit7) | 🔥 高温 | ≥30°C | 高温常亮 |

> **接管模式**：`*SET:LED <hex>` 直接控制8位LED，`*SET:LED 00` 或 `*RST` 退出接管恢复自动逻辑。

### §3.7 按键映射 ✅

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
| USER1 | PJ0 | 请求PC NTP对时 | — |
| USER2 | PJ1 | 5秒显示天气+温度 | — |

### 蜂鸣器

- 型号: PS1720P02 (C96061) 无源压电式
- 驱动: PWM0 Gen3 Output7 (PK5), 2kHz / 50% 占空比

### 7段数码管字体

支持 46 个字符：0-9、A-Z（全部26字母）、小写 c/e/h/i/j/l/n/o/r/u（与对应大写有视觉区分）

---

## 串口协议（14命令 + 8事件）

```
波特率: 115200, 8N1, 无流控
行格式: 命令以 \r\n 结束
大小写: 不敏感
缩写:   MINute → MIN/MINU/MINUT/MINUTE（大写必输，小写可选）
空格:   允许多空格及冒号前空格 (*SET : TIME = *SET:TIME)
错误:   ERROR SYNTAX / PARAM / RANGE / LINE TOO LONG
```

### PC→MCU 命令（14条）

| 命令 | 参数 | 说明 |
|------|------|------|
| `*RST` | [DATE/TIME/ALARM] | 复位，退出LED接管 |
| `*SET:DATE` | YEAR/MONTH/DATE val | 设置日期 |
| `*SET:TIME` | HOUR/MINute/SECond/OFF | 设置时间 |
| `*SET:ALARM` | HOUR/MIN/SEC/OFF | 设置闹钟 |
| `*SET:DISPLAY` | ON/OFF | 显示开关 |
| `*SET:FORMAT` | LEFT/RIGHT | 方向设置 |
| `*SET:MSG` | text ≤32字节 | 滚动消息 |
| `*SET:BEEP` | 10-5000ms | 远程蜂鸣 |
| `*SET:LED` | hex2 (00=退出接管) | LED接管 |
| `*SET:KEY` | NAME | 虚拟按键 |
| `*SET:MODE` | DAY/NIGHT | 昼夜模式 |
| `*SET:WEA` | temp code | 天气数据 |
| `*GET` | TIME/DATE/FORMAT/ALARM/DISP/MODE | 查询 |
| `*PING` | — | 心跳 |

### MCU→PC 事件（8种）

| 事件 | 触发 | 说明 |
|------|------|------|
| `*EVT:DISP <8char> <dpHex>` | 1Hz | 显示同步 |
| `*EVT:LED <hex2>` | 1Hz | LED同步 |
| `*EVT:KEY <NAME>` | 按下 | 物理按键 |
| `*EVT:ALARM` | 触发 | 闹钟开始 |
| `*EVT:ALARM_OFF` | 停止 | 闹钟结束 |
| `*EVT:EDIT <TYPE> <VALUE>` | 保存 | 编辑保存 |
| `*EVT:MODE <STATE>` | 切换 | 模式切换 |
| `*PONG <uptime>` | 应答 | PING响应 |

---

## PC 上位机

### P1 串口管理 ✅
自动扫描COM口、波特率选择、连接/断开、状态栏实时显示（连接状态/FORMAT/MODE/uptime）

### P2 控制面板 ✅ — 6个标签页

| 标签页 | 功能 |
|--------|------|
| 📅 日期时间 | SpinBox 年/月/日/时/分/秒，设置/读取/填入PC时间 |
| ⏰ 闹钟调度 | 7天网格(Mon-Sun)，独立时间+使能，应用全部/复制/清除/读取 |
| 🖥️ 显示控制 | 3模式按钮(同步MCU)、方向、速度、昼夜、LED、蜂鸣 |
| 📝 滚动消息 | 文本输入(≤32字)、字数统计、8位预览 |
| ⚡ 快捷操作 | *RST(确认框)、PING、9步演示序列 |
| 🔌 扩展功能 | NTP对时、天气获取、自动昼夜、数据图表 |

### P4 数字孪生 ✅
8位7段数码管 + 8位LED + 10键虚拟按键，1:1镜像MCU状态。支持大小写字母显示。USER1/USER2 自动触发 NTP/天气。

### P5 收发日志 ✅
时间戳、颜色编码（发送蓝/应答绿/事件紫/错误红）、最大1000行、可导出

---

## 扩展功能（E1-E4）✅ 全部完成

| 编号 | 功能 | 分值 | 实现 |
|------|------|------|------|
| **E1** | NTP 网络对时 | 2分 | ntp.aliyun.com → *SET:TIME，USER1自动触发，LED7状态指示 |
| **E2** | 天气获取 | 3分 | wttr.in 免费API → *SET:WEA，LED4-6指示，USER2短显，30分钟自动刷新 |
| **E3** | 自动昼夜 | 2分 | astral 计算上海日出日落 → 自动 *SET:MODE DAY/NIGHT，60秒轮询 |
| **E4** | 数据可视化 | 1分 | events.csv 事件记录 → matplotlib 图表（闹钟分布/每日事件/NTP时间线） |
| **§4.3** | 多闹钟调度器 | 8分 | 7槽位(Mon-Sun)每天独立设置，MCU星期追踪，PC 7行网格管理 |

---

## P4 数字孪生打磨 ✅

| 功能 | 实现 |
|------|------|
| 夜间模式4位限制 | `*EVT:MODE NIGHT` → PC数码管只显示前4位(HH.MM) |
| 编辑高亮镜像 | PC追踪FUNC/SHIFT/SAVE按键状态，同步编辑字段位置 |

---

## 稳定性修复记录

| 问题 | 修复 | 提交 |
|------|------|------|
| I2C总线竞态→幽灵按键 | SysTickIntDisable后加 `while(I2CMasterBusy())` | f476396 |
| 栈溢出→命令解析损坏 | 文件域静态缓冲区替换栈局部变量 | f476396 |
| edit_exit无条件恢复→RST后值被覆盖 | `else if (edit_mode != EDIT_NONE)` | f476396 |
| 闹钟RST后立即触发 | 默认06:00:00 | f476396 |
| 行溢出静默丢弃 | >64字符 → ERROR LINE TOO LONG | f476396 |
| 按键每次触发两次 | i2c_keys取反 | a5d26d4 |
| FORMAT RIGHT小数点不反转 | dp_buf跟随字符串反转 | a5d26d4 |
| 段码位序错误（g→a vs a←g） | 用户字符串反转后计算 | 1f5d807 |
| 蜂鸣器PN1→PK5 + DC→PWM | PWM0 Gen3 PK5 2kHz | 140f130 |

---

## 项目进度

| 阶段 | 任务 | 状态 |
|------|------|------|
| 第1周 | 底层驱动、开机画面、时钟走时 | ✅ |
| 第2周 | 闹钟、编辑FSM、串口协议全集 | ✅ |
| 第3周 | PC数字孪生、P2控制面板、E1-E4扩展 | ✅ |
| 已完成 | §4.3 多闹钟调度器（自定义功能 8分） | ✅ |
| 已完成 | P4 打磨（编辑高亮镜像、夜间4位限定） | ✅ |
