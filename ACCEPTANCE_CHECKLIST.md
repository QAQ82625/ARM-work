# 现场验收清单 — 智能联网时钟系统

验收日期: 2026-06-28 | 学号: 524031910672 | 姓名: 胡臻烨

---

## 一、提交物检查

- [ ] 源代码完整 (mcu/main.c + pc_host/*.py)
- [ ] README.md (含启动命令)
- [ ] requirements.txt (版本锁定)
- [ ] 课程报告 (LaTeX, report/main.tex)
- [ ] 演示视频要点: 双窗口并排 + PC→MCU + MCU→PC + 闹钟弹窗

---

## 二、S800板端验收清单

### 开机画面 (§3.1)
- [ ] 全亮→全灭闪烁≥1次
- [ ] 学号后8位闪烁 (LED跟随)
- [ ] 姓名拼音闪烁 (LED跟随)
- [ ] 版本号持续1.5s → 进入时钟

### 时钟显示 (§3.2)
- [ ] HH.MM.SS → DISP键循环: YY.MM.DD → YYYY.MMDD → HH.MM.SS
- [ ] 闰年 02/29→03/01 正确
- [ ] *SET:DISPLAY OFF/ON 熄屏/恢复
- [ ] 外部25MHz晶振, 走时无明显误差

### 流水显示 (§3.3)
- [ ] *SET:MSG >8字符自动滚动
- [ ] *SET:MSG ≤8字符静态2.5s后返回
- [ ] FORMAT键 LEFT/RIGHT切换
- [ ] SPEED键 500ms/250ms 2级速度
- [ ] 滚动途中按任意键(除编辑键)立即退出

### 闹钟 (§3.4)
- [ ] *SET:ALARM HOUR MIN SEC → 到点响铃
- [ ] 300ms响/300ms停节奏式
- [ ] ≤10秒自停, FUNC键立即停
- [ ] LED1: 使能常亮 / 响铃快闪
- [ ] *SET:ALARM MON HOUR 8 MIN 30 → 设置周一闹钟
- [ ] *SET:ALARM ON/OFF → 使能/禁用当天
- [ ] *GET:ALARM → 7槽完整数据
- [ ] 天气联动: 雨雪多响3声, 高温8LED慢闪

### 编辑状态机 (§3.5)
- [ ] FUNC循环: DATE→TIME→ALARM→DATE
- [ ] SHIFT切换字段 + 闪烁高亮
- [ ] ADD +1带钳制
- [ ] SAVE/长按FUNC保存退出
- [ ] 5s无操作自动退出不保存
- [ ] 编辑备份恢复: FUNC模式切换保留, 退出恢复

### LED指示 (§3.6)
- [ ] LED0: 心跳1Hz (500ms/500ms)
- [ ] LED1: 闹钟使能常亮/响铃快闪
- [ ] LED2: 编辑模式常亮
- [ ] LED3: UART TX+RX亮100ms
- [ ] LED4: SUN晴天常亮
- [ ] LED5: RAI/SNO 1Hz呼吸
- [ ] LED6: ≥30°C高温常亮
- [ ] LED7: NTP SYNCED常亮/DRIFT 1Hz闪/UNSYNCED灭
- [ ] *SET:LED AA → 进入接管 → 心跳停止
- [ ] *SET:LED 00 → 退出接管 → 心跳恢复
- [ ] *RST → 退出LED接管

### 按键映射 (§3.7)
- [ ] K0-ADD: 编辑+1
- [ ] K1-FUNC: 编辑循环/关闹钟
- [ ] K2-SHIFT: 切换字段
- [ ] K3-SPEED: 流水速度
- [ ] K4-SAVE: 保存退出
- [ ] K5-FORMAT: 方向切换
- [ ] K6-DISP: 时间/日期/年份
- [ ] K7-EXT: 闹钟使能切换
- [ ] USER1(PJ0): 请求NTP
- [ ] USER2(PJ1): 5秒显示天气
- [ ] 长按USER1: 显示n.SY.xx 3秒

### 蜂鸣器
- [ ] *SET:BEEP 1000 → 响1秒准确停止
- [ ] *SET:BEEP 500 → 0.5秒
- [ ] *SET:BEEP 2000 → 2秒
- [ ] 闹钟期间BEEP不冲突

---

## 三、串口协议验收

### PC→MCU 命令 (全部15条)
- [ ] *RST [DATE/TIME/ALARM] → OK
- [ ] *SET:DATE YEAR/MONTH/DATE val → OK + 数值正确
- [ ] *SET:TIME HOUR/MIN/SEC → OK
- [ ] *SET:ALARM HOUR/MIN/SEC/OFF/ON → OK
- [ ] *SET:DISPLAY ON/OFF → OK
- [ ] *SET:FORMAT LEFT/RIGHT → OK
- [ ] *SET:MSG text → OK (大小写保留)
- [ ] *SET:BEEP ms → OK
- [ ] *SET:LED hex2 → OK
- [ ] *SET:KEY NAME → OK (不上报*EVT:KEY环回)
- [ ] *SET:MODE DAY/NIGHT → OK + *EVT:MODE
- [ ] *SET:WEA temp code → OK
- [ ] *GET:TIME/DATE/FORMAT/ALARM/DISP/MODE → OK
- [ ] *PING → *PONG <uptime>
- [ ] 大小写不敏感 + 缩写(MIN/SEC) + 多空格容错

### MCU→PC 事件 (全部8种)
- [ ] *EVT:DISP → PC数码管更新
- [ ] *EVT:LED → PC LED更新
- [ ] *EVT:KEY → PC日志显示
- [ ] *EVT:ALARM / ALARM_OFF → PC弹窗/LED闪烁/日志
- [ ] *EVT:EDIT → PC日志
- [ ] *EVT:MODE → PC状态栏
- [ ] *PONG → PC uptime更新

### FORMAT RIGHT 验证
- [ ] *SET:FORMAT RIGHT后数码管逆序显示
- [ ] *GET:TIME返回逆序
- [ ] dpHex重新计算 (不是简单保持)
- [ ] *SET:FORMAT LEFT恢复

---

## 四、PC上位机验收

### P1 串口管理
- [ ] COM自动扫描 + 波特率选择
- [ ] 连接/断开 + 状态灯绿/灰
- [ ] 1Hz自动*PING + 3s超时离线检测
- [ ] 状态栏: 连接/FORMAT/MODE/ALARM

### P2 控制面板 (7标签页)
- [ ] 日期时间: SpinBox + 设置/读取/填PC时间
- [ ] 闹钟调度: 7行网格 + 设置/清除/应用全部
- [ ] 显示控制: ON/OFF + 方向 + 速度 + 昼夜 + LED hex + 蜂鸣ms
- [ ] 滚动消息: 文本输入 + 字数统计
- [ ] 快捷操作: *RST + PING + 演示序列
- [ ] 扩展功能: NTP/天气/昼夜/图表
- [ ] 贪吃蛇: 游戏运行正常

### P4 数字孪生
- [ ] 8位7SEG + 8位LED + 10键按键
- [ ] SEG亮#FF3030/灭#220000
- [ ] 小数点独立占位渲染
- [ ] NIGHT仅4位 + LED仅心跳
- [ ] MCU按USER2 → PC高亮200ms
- [ ] 点击FUNC → MCU进入编辑

### P5 收发日志
- [ ] 时间戳 + 颜色编码(蓝/绿/紫/红)
- [ ] 最大1000行
- [ ] 所有事件可见 (DISP/LED 1Hz心跳也在日志中)

---

## 五、扩展功能验收 (E1-E4)

### E1 NTP对时
- [ ] PC点击 → ntp.aliyun.com → *SET:DATE + *SET:TIME + *NTP
- [ ] USER1触发自动对时
- [ ] 对时后MCU与PC差<1s
- [ ] 断网弹窗提示

### E2 天气
- [ ] 启动5s内显示天气
- [ ] MCU LED更新 (SUN/RAI/高温)
- [ ] USER2键短显温度+天气
- [ ] 30分钟自动刷新

### E3 自动昼夜
- [ ] astral计算日出日落
- [ ] sunset→NIGHT, sunrise→DAY
- [ ] NIGHT: SEG仅4位 + LED仅心跳 + 闹钟静音
- [ ] UI复选框控制 + 日出日落时刻

### E4 数据可视化
- [ ] events.csv可Excel打开
- [ ] ≥3种图表: 闹钟分布 + 每日事件 + NTP时间线
- [ ] 数据少时不崩溃

---

## 六、自定义功能验收 (§4.3)

### 多闹钟调度器
- [ ] *SET:ALARM MON HOUR 7 MIN 30 → 周一独立设置
- [ ] *SET:ALARM ALL → 全部槽位
- [ ] *GET:ALARM → 7槽数据完整
- [ ] PC 7行网格 + 每行独立设置/清除
- [ ] 无DOW默认当天

### 贪吃蛇游戏
- [ ] *SET:GAME START → 数码管显示"--------" + PC棋盘显示
- [ ] MCU按键方向控制: ADD左/FUNC下/SHIFT右/DISP上
- [ ] PC吃食物 +1分 → MCU数码管 "Sc NNN"
- [ ] GAME OVER → MCU LED闪2次 + "End NNN"
- [ ] USER1暂停/继续

---

## 七、演示流程建议

1. **开机画面** → 展示全亮→学号→姓名→版本号
2. **时钟+DISP** → DISP键切换三种显示模式
3. **PC面板双向控制** → PC点设置→MCU响应 + MCU按DISP→PC SEgments同步
4. **闹钟验证** → *SET:ALARM设置30秒后 → 等待触发 → FUNC停止
5. **FORMAT左右** → *SET:FORMAT RIGHT → 展示逆序显示 + dp重新计算
6. **NTP天气** → USER1触发NTP + USER2显示天气
7. **MSG流水** → 发送>8字消息 → 展现滚动 + 按DISP退出
8. **自定义功能1** → 多闹钟调度器 PC界面展示
9. **自定义功能2** → 贪吃蛇: *SET:GAME START → 按键操控 → GAME OVER
10. **LED接管** → *SET:LED 5A → 5秒后 → *SET:LED 00恢复
