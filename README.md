# 智能联网时钟系统 — HW26-0672

学生版大作业 V1.2 | 学号 20260001 | 姓名 HU ZHENYE

---

## 项目结构

```
hw26-0672/
├── README.md              ← 本文件
├── mcu/
│   ├── main.c             ← **全部自编代码集中于此** (1814行)
│   ├── src/               ← 其他模块文件
│   ├── obj/xxx.axf        ← 编译输出（可烧写）
│   └── driverlib/         ← 驱动库（保留原样）
├── pc_host/
│   ├── virual_twin_panel.py   ← PC上位机（PyQt5 数字孪生面板）
│   └── requirements.txt       ← Python依赖
└── docs/
    └── 大作业_20260001_胡振业.pdf  ← 简介文档
```

## 编译与运行

### S800 板端（MCU）
1. 用 Keil MDK 打开 `mcu/S800.uvprojx`
2. 编译 → 烧写到 S800 开发板
3. 烧写后自动运行，无需连接 PC

### PC 端
```bash
cd pc_host
.venv\Scripts\python virual_twin_panel.py
```

---

## S800 板端功能实现状态

### §3.1 开机画面 — ✅ 已实现

| 要求 | 说明 |
|------|------|
| 8位SEG+8位LED全亮→全灭闪烁1次 | `ShowString("88888888", ...)` → 全灭 |
| 学号后8位闪烁1次 + LED同步 | `"20260001"` 闪两次 |
| 姓名拼音8字符闪烁1次 | `"HUZHENYE"` 闪两次 |
| 软件版本号显示后进入时钟 | `"V1.0"` 显示约2秒 |

### §3.2 时钟与日期显示 — ✅ 已实现

| 要求 | 说明 |
|------|------|
| 默认 HH.MM.SS 显示 | `disp_mode = DISP_MODE_TIME` |
| 按键切换 YY.MM.DD / YYYY.MMDD | DISP键循环切换 |
| 闰年、月末进位 (28/29/30/31) | `is_leap_year()` + `get_days_in_month()` |
| 1ms 系统时基，秒不丢/不抖 | SysTick 1000Hz → 100ms×10=1s |
| `*SET:DISPLAY OFF` 熄屏/ON 恢复 | `cmd_set_display()` |
| 小数点位置 (HH.MM.SS, YY.MM.DD) | `dp_buf` 位图控制 |

### §3.3 流水显示 — ✅ 已实现

| 要求 | 说明 |
|------|------|
| >8字符自动滚动 | `scroll_active` / `scroll_pos`，100ms级推进 |
| 方向切换 (LEFT/RIGHT) | `*SET:FORMAT` + FORMAT 按键 |
| 2级速度可调 | SPEED 按键切换 slow(500ms)/fast(200ms) |
| 小数点跟随方向 | FORMAT RIGHT 时字符串反转，小数点位置不变 |

### §3.4 闹钟 — ✅ 已实现

| 要求 | 说明 |
|------|------|
| 时分秒相等触发 | `alarm_check()` 每秒比较 |
| 蜂鸣器节奏响铃 (响200ms-停200ms-响200ms) | `alarm_beep_phase` 状态机 |
| 持续10秒自动停 | `alarm_total_timer` 100ms×100 = 10s |
| FUNC 中途停止 | 闹钟响铃中按 FUNC → `alarm_stop()` |
| LED 闹钟指示 | `LED_ALARM` (bit 1) |

### §3.5 按键编辑状态机 — ✅ 已实现

| 要求 | 说明 |
|------|------|
| FUNC 循环切换模式 (日期→时间→闹钟→退出) | `edit_enter()` / `edit_exit()` |
| SHIFT 高亮闪烁当前字段 | `edit_blink` 300ms周期，通过 `update_display()` 消隐 |
| ADD 循环加1 + 进位钳制 | `key_action(0)` 年月日时分秒范围校验 |
| SAVE / 长按FUNC保存退出 | `key_action(4)` + 长按 FUNC (hold≥1s) |
| 5秒无操作自动退出不保存 | `edit_timeout` ≥ 500×10ms |
| 取消时恢复原值 | `backup_*` 变量 restore |

### §3.6 LED 辅助指示 — ✅ 已实现

| 位 | 含义 |
|----|------|
| bit 0 | ❤️ 系统心跳 (500ms翻转) |
| bit 1 | ⏰ 闹钟状态 (响铃时亮) |
| bit 2 | ✏️ 编辑模式 (编辑中亮) |
| bit 3 | 📤 TX 发送活动 (亮300ms) |
| bit 4 | 📥 RX 接收活动 (亮300ms) |
| bit 5 | 🕐 NTP 同步 (扩展功能预留) |
| bit 6-7 | 预留 |

### §3.7 按键完整映射 — ✅ 已实现

| 按键 | 位置 | 短按功能 | 长按功能 |
|------|------|----------|----------|
| ADD | K0 (I2C) | 当前字段+1 | — |
| FUNC | K1 (I2C) | 编辑模式切换 / 关闹钟 | 保存并退出 |
| SHIFT | K2 (I2C) | 切换高亮字段 | — |
| SPEED | K3 (I2C) | 流水速度2级切换 | — |
| SAVE | K4 (I2C) | 保存并退出编辑 | — |
| FORMAT | K5 (I2C) | 流水方向切换 | — |
| DISP | K6 (I2C) | 时间/日期/年份切换 | — |
| EXT | K7 (I2C) | 预留 | — |
| USER1 | PJ0 (GPIO) | 请求PC对时 | — |
| USER2 | PJ1 (GPIO) | 请求天气显示 | — |

### §5.1 协议容错 — ✅ 已实现

| 规则 | 实现 |
|------|------|
| 115200 8N1 | `UARTConfigSetExpClk` |
| ASCII, 行结束 \\r\\n | `UART0_Handler` 以 \\n 为行结束 |
| 大小写不敏感 | `toupper()` 转换后匹配 |
| 空格容错 | 前后 trim，参数间多空格兼容 |
| 空格在冒号前容错 | `*SET : TIME ...` → 自动重构为 `*SET:TIME` |
| 缩写规则 | `match_abbrev()` — 大写=必输，小写=可选 |
| ERROR 应答 | ERROR SYNTAX / PARAM / RANGE |

### §5.2 PC→S800 命令 — ✅ 已实现 (12/12)

| 命令 | 子命令/参数 | 状态 |
|------|------------|------|
| `*RST` | DATE/YEAR/MONTH/TIME/ALARM 任意组合 | ✅ |
| `*SET:DATE` | YEAR/MONTH/DATE + 值 | ✅ |
| `*SET:TIME` | HOUR/MINute/SECond + 值 | ✅ |
| `*SET:DISPLAY` | ON/OFF | ✅ |
| `*SET:FORMAT` | LEFT/RIGHT | ✅ |
| `*SET:MSG` | \<最多32字节文本\> | ✅ |
| `*SET:BEEP` | 10-5000 (ms) | ✅ |
| `*SET:LED` | \<hex2\> | ✅ |
| `*SET:KEY` | FUNC/SHIFT/ADD/SAVE/DISP/SPEED/FORMAT/EXT/USER1/USER2 | ✅ |
| `*SET:MODE` | DAY/NIGHT | ✅ |
| `*GET` | DATE/TIME/FORMAT (或省略) | ✅ |
| `*PING` | — | ✅ |

### §5.3 S800→PC 报文 — ✅ 已实现 (7/7)

| 报文 | 触发条件 |
|------|----------|
| `*EVT:DISP <8char> <dpHex>` | **每秒心跳** + 显示变更 |
| `*EVT:LED <hex2>` | **每秒心跳** + LED变更 |
| `*EVT:KEY <NAME>` | 物理按键按下 |
| `*EVT:ALARM` / `*EVT:ALARM_OFF` | 闹钟启停 |
| `*EVT:EDIT <TYPE> <VALUE>` | 编辑保存 |
| `*EVT:MODE <STATE>` | 昼夜模式切换 |
| `*PONG <uptime>` | PING 应答 |

---

## ❌ 尚未实现

### S800 板端

| 功能 | 说明 | 优先级 |
|------|------|--------|
| 扩展功能 E1-E4 | NTP对时、天气获取、昼夜自动模式、数据可视化 | 需PC端配合 |
| 闹钟使能/禁用切换 | 当前闹钟始终使能，无独立开关 | 低 |
| 编辑选择闪烁镜面到PC | 当前闪烁是纯本地行为（符合§6.2不强制要求） | — |

### PC 上位机（§4.1）

| 功能 | 状态 | 说明 |
|------|------|------|
| P1 串口管理与状态栏 | ⚠️ 基本实现 | 连接/断开/状态栏，缺少FORMAT/MODE状态实时显示 |
| P2 控制面板 | ⚠️ 仅快捷按钮 | 缺少参数组合下拉、天气按钮、演示快捷键 |
| P4 数字孪生面板 | ⚠️ 部分实现 | SEG/LED/按键/事件同步已通，缺少编辑态字段高亮镜像、NIGHT 模式 4位限定 |
| P5 收发日志 | ✅ 已实现 | 时间戳、方向、颜色编码、可导出 |

### 扩展功能（§4.2）

| 功能 | 分值 | 状态 |
|------|------|------|
| E1 网络NTP对时 | 2分 | ❌ |
| E2 天气获取 | 3分 | ❌ |
| E3 自动昼夜模式 | 2分 | ❌ |
| E4 数据可视化 | 1分 | ❌ |

### 自主增加功能（§4.3）

| 状态 | 说明 |
|------|------|
| ❌ | 8分，高分的必要条件 |

---

## 串口协议快速参考

```
波特率: 115200, 8N1, 无流控
行格式: 命令以 \r\n 结束
大小写: 不敏感
缩写:   MINute 可写为 MIN/MINU/MINUT/MINUTE
空格:   命令与参数间允许多空格

常用命令:
  *PING                    → *PONG <uptime>
  *GET:TIME                → OK 12 34 56
  *SET:TIME HOUR 12 MIN 34 → OK
  *SET:DISPLAY OFF         → OK
  *SET:FORMAT RIGHT        → OK
  *SET:MSG Hello           → OK
  *RST                     → OK
```

---

## 3周进度

| 周 | 任务 | 状态 |
|----|------|------|
| 第1周 | 底层驱动、开机画面、时钟走时 | ✅ 完成 |
| 第2周 | 闹钟、编辑状态机、串口指令全集 | ✅ 完成 |
| 第3周 | PC上位机、数字孪生、扩展功能、视频/文档 | 🔄 进行中 |
