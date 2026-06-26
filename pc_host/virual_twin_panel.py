#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
智能联网时钟系统 - 数字孪生面板（P2控制面板完整版）
"""

import sys
import os
import csv
import time
import serial
import serial.tools.list_ports
from datetime import datetime
from PyQt5.QtWidgets import *
from PyQt5.QtCore import *
from PyQt5.QtGui import *
from ntp_helper import fetch_ntp_commands, fetch_ntp_time, format_time
from weather_helper import fetch_weather_command, fetch_weather
from snake_game import SnakeGameWidget


# ============================================================
# SimpleSevenSegment — 7段数码管控件
# ============================================================
class SimpleSevenSegment(QWidget):
    """简单可靠的7段数码管"""
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setFixedSize(80, 120)
        self.digit = '8'
        self.dp = False

    def set_digit(self, digit, dp=False):
        s = str(digit)[0] if digit != '_' else ' '
        if s == '.':
            self.digit = ' '
            self.dp = True   # dot char: blank 7-seg, dp lit
        else:
            self.digit = s
            self.dp = dp
        self.update()

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing)

        w, h = self.width(), self.height()

        # 绘制外框
        painter.setPen(QPen(QColor(80, 80, 80), 2))
        painter.setBrush(QColor(20, 20, 20))
        painter.drawRoundedRect(5, 5, w - 10, h - 10, 8, 8)

        painter.setPen(Qt.NoPen)
        thick = 8
        gap = 5

        seg_coords = {
            'a': (gap, gap, w - gap, gap + thick, True),
            'b': (w - gap - thick, gap, w - gap, h // 2 - thick // 2, False),
            'c': (w - gap - thick, h // 2 + thick // 2, w - gap, h - gap, False),
            'd': (gap, h - gap - thick, w - gap, h - gap, True),
            'e': (gap, h // 2 + thick // 2, gap + thick, h - gap, False),
            'f': (gap, gap, gap + thick, h // 2 - thick // 2, False),
            'g': (gap + thick // 2, h // 2 - thick // 2, w - gap - thick // 2, h // 2 + thick // 2, True)
        }

        seg_pattern = {
        '0': ['a', 'b', 'c', 'd', 'e', 'f'],
        '1': ['b', 'c'],
        '2': ['a', 'b', 'd', 'e', 'g'],
        '3': ['a', 'b', 'c', 'd', 'g'],
        '4': ['b', 'c', 'f', 'g'],
        '5': ['a', 'c', 'd', 'f', 'g'],
        '6': ['a', 'c', 'd', 'e', 'f', 'g'],
        '7': ['a', 'b', 'c'],
        '8': ['a', 'b', 'c', 'd', 'e', 'f', 'g'],
        '9': ['a', 'b', 'c', 'd', 'f', 'g'],
        'A': ['a', 'b', 'c', 'e', 'f', 'g'],
        'B': ['c', 'd', 'e', 'f', 'g'],
        'C': ['a', 'd', 'e', 'f'],
        'D': ['b', 'c', 'd', 'e', 'g'],
        'E': ['a', 'd', 'e', 'f', 'g'],
        'F': ['a', 'e', 'f', 'g'],
        'G': ['a', 'c', 'd', 'e', 'f'],
        'H': ['b', 'c', 'e', 'f', 'g'],
        'I': ['e', 'f'],
        'J': ['b', 'c', 'd', 'e'],
        'K': ['b', 'd', 'e', 'f', 'g'],
        'L': ['c', 'd', 'e', 'f'],
        'M': ['a', 'c', 'e', 'g'],
        'N': ['a', 'b', 'c', 'e', 'f'],
        'O': ['a', 'b', 'c', 'd', 'e', 'f'],
        'P': ['a', 'b', 'e', 'f', 'g'],
        'Q': ['a', 'b', 'c', 'f', 'g'],
        'R': ['e', 'f', 'g'],
        'S': ['a', 'c', 'd', 'f', 'g'],
        'T': ['d', 'e', 'f', 'g'],
        'U': ['b', 'c', 'd', 'e', 'f'],
        'V': ['b', 'c', 'd', 'e', 'f', 'g'],
        'W': ['b', 'd', 'f', 'g'],
        'X': ['b', 'c', 'e', 'f'],
        'Y': ['b', 'c', 'd', 'f', 'g'],
        'Z': ['a', 'd', 'g'],
        # Lowercase — distinct from uppercase (per datasheet table)
        'c': ['d', 'e', 'g'],
        'e': ['a', 'b', 'd', 'e', 'f', 'g'],
        'h': ['c', 'e', 'f', 'g'],
        'i': ['e'],
        'j': ['b', 'c', 'd'],
        'l': ['d', 'e', 'f'],
        'n': ['c', 'e', 'g'],
        'o': ['c', 'd', 'e', 'g'],
        'r': ['e', 'g'],
        'u': ['c', 'd', 'e'],
        '-': ['g'],
        '_': ['d'],
        '.': [],   # dot char: no 7-seg, dp drawn below
        ' ': []
        }

        # Exact match first (for lowercase), then uppercase fallback
        ch = self.digit if self.digit in seg_pattern else self.digit.upper()
        pattern = seg_pattern.get(ch, seg_pattern['8'])

        for seg_name in ['a', 'b', 'c', 'd', 'e', 'f', 'g']:
            if seg_name in pattern:
                painter.setBrush(QColor(255, 69, 0))
            else:
                painter.setBrush(QColor(40, 40, 40))

            x1, y1, x2, y2, is_horiz = seg_coords[seg_name]
            if is_horiz:
                painter.drawRect(x1, y1, x2 - x1, thick)
            else:
                painter.drawRect(x1, y1, thick, y2 - y1)

        if self.dp:
            painter.setBrush(QColor(255, 69, 0))
            painter.drawEllipse(w - 15, h - 15, 8, 8)


# ============================================================
# SimpleLED — LED指示灯控件
# ============================================================
class SimpleLED(QWidget):
    """简单可靠的LED指示灯"""
    def __init__(self, label="LED", parent=None):
        super().__init__(parent)
        self.setFixedSize(60, 60)
        self.label = label
        self.on = False

    def set_state(self, on):
        self.on = on
        self.update()

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing)

        painter.setPen(QPen(QColor(100, 100, 100), 2))
        painter.setBrush(QColor(30, 30, 30))
        painter.drawRoundedRect(5, 5, 50, 40, 5, 5)

        if self.on:
            painter.setBrush(QColor(0, 255, 0))
            painter.setPen(QPen(QColor(0, 255, 0, 100), 2))
        else:
            painter.setBrush(QColor(20, 20, 20))
            painter.setPen(Qt.NoPen)

        painter.setBrush(QColor(0, 255, 0) if self.on else QColor(20, 20, 20))
        painter.setPen(Qt.NoPen)
        painter.drawEllipse(12, 8, 30, 30)

        painter.setPen(QPen(QColor(200, 200, 200), 1))
        painter.setFont(QFont("Microsoft YaHei", 9))
        painter.drawText(QRect(0, 35, 60, 25), Qt.AlignHCenter | Qt.AlignTop, self.label)


# ============================================================
# SerialThread — 串口通信线程
# ============================================================
class SerialThread(QThread):
    data_received = pyqtSignal(str)

    def __init__(self):
        super().__init__()
        self.serial = None
        self.running = False

    def connect(self, port, baudrate=115200):
        try:
            self.serial = serial.Serial(port, baudrate, timeout=1)
            self.running = True
            self.start()
            return True, "连接成功"
        except Exception as e:
            return False, str(e)

    def disconnect(self):
        self.running = False
        if self.serial and self.serial.is_open:
            self.serial.close()

    def send_command(self, cmd):
        if self.serial and self.serial.is_open:
            try:
                self.serial.write((cmd + "\r\n").encode())
                return True
            except:
                return False
        return False

    def run(self):
        while self.running and self.serial and self.serial.is_open:
            try:
                if self.serial.in_waiting:
                    line = self.serial.readline().decode(errors='replace').strip()
                    if line:
                        self.data_received.emit(line)
            except:
                pass
            self.msleep(10)


# ============================================================
# NetworkWorker — 异步网络请求（不阻塞 UI）
# ============================================================
class NtpWorker(QObject):
    """后台线程执行 NTP 请求，结果通过信号发回主线程"""
    finished = pyqtSignal(object)  # None=失败, (y,mo,d,h,mi,s)=成功

    @pyqtSlot()
    def run(self):
        try:
            t = fetch_ntp_time()
            self.finished.emit(t)
        except Exception:
            self.finished.emit(None)


class WeatherWorker(QObject):
    """后台线程执行天气请求，结果通过信号发回主线程"""
    finished = pyqtSignal(object)  # None=失败, (cmd,desc)=成功

    @pyqtSlot()
    def run(self):
        try:
            result = fetch_weather_command()
            self.finished.emit(result)
        except Exception:
            self.finished.emit(None)


# ============================================================
# VirtualTwinPanel — 主窗口
# ============================================================
class VirtualTwinPanel(QMainWindow):
    def __init__(self):
        super().__init__()
        self.serial_thread = SerialThread()

        # 状态追踪
        self._current_mode = "DAY"
        self._current_format = "LEFT"
        self._last_uptime = 0
        self._alarm_ringing = False
        self._alarm_enabled = False
        self._alarm_time = "06:00:00"
        self._disp_mode_idx = 0   # 0=TIME, 1=DATE_SHORT, 2=DATE_LONG
        self._edit_mode = 0       # 0=none, 1=date, 2=time, 3=alarm
        self._edit_field = 0      # current edit field index
        self._weather_temp = -99  # cached weather data for PC mirror display
        self._weather_cond = "---"
        self._weather_desc = ""
        self._show_weather_seg = False  # True=PC is showing weather, block *EVT:DISP

        # Alarm LED blink timer
        self._alarm_blink = QTimer()
        self._alarm_blink.timeout.connect(self._blink_alarm_led)
        self._alarm_blink_on = False

        self.init_ui()

    # ── 样式定义 ─────────────────────────────
    STYLE_DARK = """
        QMainWindow { background-color: #2b2b2b; }
        QGroupBox {
            color: white; font-weight: bold;
            border: 2px solid #555; border-radius: 8px;
            margin-top: 10px; padding-top: 15px;
            background-color: #3c3c3c;
        }
        QGroupBox::title {
            subcontrol-origin: margin; left: 10px; padding: 0 10px;
        }
        QPushButton {
            background-color: #4a4a4a; border: 1px solid #666;
            border-radius: 5px; padding: 8px 14px; color: white;
            font-family: "Microsoft YaHei";
        }
        QPushButton:hover { background-color: #5a5a5a; border-color: #0078d7; }
        QPushButton:pressed { background-color: #0078d7; }
        QLabel { color: white; font-family: "Microsoft YaHei"; }
        QCheckBox { color: white; font-family: "Microsoft YaHei"; }
        QComboBox {
            background-color: #4a4a4a; border: 1px solid #666;
            border-radius: 4px; padding: 4px 8px; color: white;
            font-family: "Microsoft YaHei";
        }
        QComboBox::drop-down { border: none; }
        QComboBox QAbstractItemView {
            background-color: #4a4a4a; color: white;
            selection-background-color: #0078d7;
        }
        QSpinBox {
            background-color: #4a4a4a; border: 1px solid #666;
            border-radius: 4px; padding: 4px; color: white;
            font-family: "Consolas", "Microsoft YaHei"; font-size: 14px;
        }
        QSpinBox::up-button, QSpinBox::down-button {
            background-color: #555; border: none; width: 16px;
        }
        QRadioButton { color: white; font-family: "Microsoft YaHei"; }
        QTabWidget::pane {
            border: 2px solid #555; border-radius: 5px;
            background-color: #3c3c3c;
        }
        QTabWidget::tab-bar { alignment: left; }
        QTabBar::tab {
            background-color: #4a4a4a; color: #ccc;
            border: 1px solid #555; border-bottom: none;
            padding: 6px 16px; margin-right: 2px;
            border-top-left-radius: 5px; border-top-right-radius: 5px;
        }
        QTabBar::tab:selected {
            background-color: #3c3c3c; color: white;
            border-color: #0078d7; border-bottom: 2px solid #0078d7;
        }
        QTabBar::tab:hover { background-color: #5a5a5a; }
        QLineEdit {
            background-color: #4a4a4a; border: 1px solid #666;
            border-radius: 4px; padding: 4px 8px; color: white;
            font-family: "Microsoft YaHei";
        }
    """

    # ── 主界面构建 ───────────────────────────
    def init_ui(self):
        self.setWindowTitle("智能联网时钟 - 数字孪生面板 (P2完整版)")
        self.setMinimumSize(1000, 800)
        self.setStyleSheet(self.STYLE_DARK)

        central = QWidget()
        self.setCentralWidget(central)
        main_layout = QVBoxLayout(central)
        main_layout.setSpacing(10)

        # ── 1. 串口控制栏 ──
        main_layout.addWidget(self._create_serial_bar())

        # ── 状态栏 ──
        self.status_bar = QStatusBar()
        self.setStatusBar(self.status_bar)
        self._init_status_indicators()

        # ── 2. 数码管 + LED/按键 水平布局 ──
        display_row = QHBoxLayout()
        display_row.addWidget(self._create_segment_group(), 3)
        display_row.addWidget(self._create_led_key_group(), 2)
        main_layout.addLayout(display_row)

        # ── 3. 控制面板 QTabWidget ──
        self.tab_widget = QTabWidget()
        self.tab_widget.addTab(self._create_tab_datetime(),  "📅 日期时间")
        self.tab_widget.addTab(self._create_tab_alarm(),     "⏰ 闹钟管理")
        self.tab_widget.addTab(self._create_tab_display(),   "🖥️ 显示控制")
        self.tab_widget.addTab(self._create_tab_message(),   "📝 滚动消息")
        self.tab_widget.addTab(self._create_tab_quick(),     "⚡ 快捷操作")
        self.tab_widget.addTab(self._create_tab_extensions(), "🔌 扩展功能")
        self.game_widget = SnakeGameWidget(self.send_cmd)
        self.tab_widget.addTab(self.game_widget, "🐍 贪吃蛇")
        main_layout.addWidget(self.tab_widget)

        # ── 4. 通信日志 ──
        main_layout.addWidget(self._create_log_group())

        # 初始化显示
        self._init_default_display()

        # 信号连接
        self.serial_thread.data_received.connect(self.handle_data)

    # ── 串口控制栏 ─────────────────────────
    def _create_serial_bar(self):
        group = QGroupBox("串口控制")
        layout = QHBoxLayout()

        layout.addWidget(QLabel("串口:"))
        self.com_combo = QComboBox()
        self.com_combo.setMinimumWidth(200)
        layout.addWidget(self.com_combo)

        self.refresh_btn = QPushButton("刷新")
        self.refresh_btn.clicked.connect(self.refresh_ports)
        layout.addWidget(self.refresh_btn)

        self.connect_btn = QPushButton("连接")
        self.connect_btn.clicked.connect(self.toggle_serial)
        layout.addWidget(self.connect_btn)

        layout.addWidget(QLabel("波特率:"))
        self.baud_combo = QComboBox()
        self.baud_combo.addItems(["115200", "9600", "38400", "57600"])
        self.baud_combo.setCurrentText("115200")
        layout.addWidget(self.baud_combo)

        layout.addStretch()

        self.auto_refresh_check = QCheckBox("启动时自动刷新串口")
        self.auto_refresh_check.setChecked(True)
        layout.addWidget(self.auto_refresh_check)

        group.setLayout(layout)
        return group

    # ── 状态栏指示器 ────────────────────────
    def _init_status_indicators(self):
        self.status_bar.showMessage("未连接")

        self.conn_indicator = QLabel("● 未连接")
        self.conn_indicator.setStyleSheet("color: #888; font-weight: bold;")
        self.status_bar.addPermanentWidget(self.conn_indicator)

        self.mode_indicator = QLabel("☀️ 日间")
        self.mode_indicator.setStyleSheet("color: #FFE66D; font-weight: bold;")
        self.status_bar.addPermanentWidget(self.mode_indicator)

        self.fmt_indicator = QLabel("← 左")
        self.fmt_indicator.setStyleSheet("color: #4ECDC4; font-weight: bold;")
        self.status_bar.addPermanentWidget(self.fmt_indicator)

        self.alarm_indicator = QLabel("⏰ 闹钟: --")
        self.alarm_indicator.setStyleSheet("color: #888; font-weight: bold;")
        self.status_bar.addPermanentWidget(self.alarm_indicator)

        self.uptime_indicator = QLabel("")
        self.uptime_indicator.setStyleSheet("color: #888;")
        self.status_bar.addPermanentWidget(self.uptime_indicator)

        # 1Hz PING 定时器 + 3s 超时检测
        self._ping_timer = QTimer(self)
        self._ping_timer.timeout.connect(self._ping_tick)
        self._last_pong_time = 0.0
        self._ping_offline = False

    # ── 状态栏更新方法 ──────────────────────
    def _set_connection_status(self, connected, port=""):
        if connected:
            self.conn_indicator.setText(f"● {port} 已连接")
            self.conn_indicator.setStyleSheet("color: #95E77E; font-weight: bold;")
        else:
            self.conn_indicator.setText("● 未连接")
            self.conn_indicator.setStyleSheet("color: #888; font-weight: bold;")

    def _set_mode_indicator(self, mode):
        self._current_mode = mode
        if mode == "NIGHT":
            self.mode_indicator.setText("🌙 夜间")
            self.mode_indicator.setStyleSheet("color: #4A90D9; font-weight: bold;")
        else:
            self.mode_indicator.setText("☀️ 日间")
            self.mode_indicator.setStyleSheet("color: #FFE66D; font-weight: bold;")

    def _set_format_indicator(self, fmt):
        self._current_format = fmt
        self.fmt_indicator.setText("→ 右" if fmt == "RIGHT" else "← 左")

    def _set_uptime_display(self, seconds):
        self._last_uptime = seconds
        h, m, s = seconds // 3600, (seconds % 3600) // 60, seconds % 60
        self.uptime_indicator.setText(f"⏱ {h:02d}:{m:02d}:{s:02d}")

    # ── 数码管区域 ──────────────────────────
    def _create_segment_group(self):
        group = QGroupBox("8位数码管显示")
        layout = QHBoxLayout()
        layout.setAlignment(Qt.AlignCenter)

        self.segments = []
        for i in range(8):
            seg = SimpleSevenSegment()
            layout.addWidget(seg)
            self.segments.append(seg)

        group.setLayout(layout)
        return group

    # ── LED + 虚拟按键区域 ─────────────────
    def _create_led_key_group(self):
        outer = QVBoxLayout()

        # LED 区
        led_group = QGroupBox("LED状态指示灯")
        led_layout = QVBoxLayout()
        led_layout.setSpacing(6)

        row1 = QHBoxLayout()
        row1.setAlignment(Qt.AlignCenter)
        led1_labels = ['❤️ 心跳', '⏰ 闹钟', '✏️ 编辑', '📤📥 UART']
        self.leds1 = []
        for label in led1_labels:
            led = SimpleLED(label)
            row1.addWidget(led)
            self.leds1.append(led)
        row1.addStretch()

        row2 = QHBoxLayout()
        row2.setAlignment(Qt.AlignCenter)
        led2_labels = ['☀️ 晴天', '🌧️ 雨雪', '🔥 高温', '🕐 NTP']
        self.leds2 = []
        for label in led2_labels:
            led = SimpleLED(label)
            row2.addWidget(led)
            self.leds2.append(led)
        row2.addStretch()

        led_layout.addLayout(row1)
        led_layout.addLayout(row2)
        led_group.setLayout(led_layout)
        outer.addWidget(led_group)

        # 虚拟按键区
        key_group = QGroupBox("虚拟按键")
        key_layout = QGridLayout()
        key_layout.setSpacing(6)

        keys = [
            ('FUNC', 0, 0), ('SHIFT', 0, 1), ('ADD', 0, 2), ('SAVE', 0, 3),
            ('DISP', 1, 0), ('SPEED', 1, 1), ('FORMAT', 1, 2), ('EXT', 1, 3),
            ('USER1', 2, 0), ('USER2', 2, 1)
        ]

        self.key_btns = {}
        for name, row, col in keys:
            btn = QPushButton(name)
            btn.setMinimumWidth(80)
            btn.setMinimumHeight(36)
            # Use default argument capture with lambda
            btn.clicked.connect(lambda checked, n=name: self._on_key_click(n))
            key_layout.addWidget(btn, row, col)
            self.key_btns[name] = btn

        key_group.setLayout(key_layout)
        outer.addWidget(key_group)

        container = QWidget()
        container.setLayout(outer)
        return container

    # ── Tab 1: 日期时间 ─────────────────────
    def _create_tab_datetime(self):
        w = QWidget()
        layout = QVBoxLayout()
        layout.setSpacing(12)

        # 日期行
        date_row = QHBoxLayout()
        date_row.addWidget(QLabel("日期:"))
        self.sp_year = QSpinBox()
        self.sp_year.setRange(2025, 2099); self.sp_year.setValue(2026)
        self.sp_year.setPrefix("年 "); self.sp_year.setSuffix("  ")
        date_row.addWidget(self.sp_year)
        self.sp_month = QSpinBox()
        self.sp_month.setRange(1, 12); self.sp_month.setValue(6)
        self.sp_month.setPrefix("月 "); self.sp_month.setSuffix("  ")
        date_row.addWidget(self.sp_month)
        self.sp_day = QSpinBox()
        self.sp_day.setRange(1, 31); self.sp_day.setValue(8)
        self.sp_day.setPrefix("日 ")
        date_row.addWidget(self.sp_day)
        date_row.addStretch()
        btn_set_date = QPushButton("设置日期")
        btn_set_date.clicked.connect(self._on_set_date)
        date_row.addWidget(btn_set_date)
        layout.addLayout(date_row)

        # 时间行
        time_row = QHBoxLayout()
        time_row.addWidget(QLabel("时间:"))
        self.sp_hour = QSpinBox()
        self.sp_hour.setRange(0, 23); self.sp_hour.setValue(12)
        self.sp_hour.setPrefix("时 "); self.sp_hour.setSuffix("  ")
        time_row.addWidget(self.sp_hour)
        self.sp_min = QSpinBox()
        self.sp_min.setRange(0, 59); self.sp_min.setValue(34)
        self.sp_min.setPrefix("分 "); self.sp_min.setSuffix("  ")
        time_row.addWidget(self.sp_min)
        self.sp_sec = QSpinBox()
        self.sp_sec.setRange(0, 59); self.sp_sec.setValue(56)
        self.sp_sec.setPrefix("秒 ")
        time_row.addWidget(self.sp_sec)
        time_row.addStretch()
        btn_set_time = QPushButton("设置时间")
        btn_set_time.clicked.connect(self._on_set_time)
        time_row.addWidget(btn_set_time)
        layout.addLayout(time_row)

        # 操作行
        action_row = QHBoxLayout()
        btn_get = QPushButton("从MCU读取当前值")
        btn_get.clicked.connect(self._on_get_time_date)
        action_row.addWidget(btn_get)
        btn_now = QPushButton("填入电脑当前时间")
        btn_now.clicked.connect(self._on_fill_pc_time)
        action_row.addWidget(btn_now)
        action_row.addStretch()
        self.lbl_time_status = QLabel("")
        self.lbl_time_status.setStyleSheet("color: #888;")
        action_row.addWidget(self.lbl_time_status)
        layout.addLayout(action_row)

        # 实时显示
        rt_row = QHBoxLayout()
        rt_row.addWidget(QLabel("MCU当前:"))
        self.lbl_mcu_time = QLabel("--:--:--")
        self.lbl_mcu_time.setStyleSheet(
            "color: #FFE66D; font-size: 18px; font-family: Consolas;"
        )
        rt_row.addWidget(self.lbl_mcu_time)
        self.lbl_mcu_date = QLabel("----/--/--")
        self.lbl_mcu_date.setStyleSheet(
            "color: #4ECDC4; font-size: 14px; font-family: Consolas;"
        )
        rt_row.addWidget(self.lbl_mcu_date)
        rt_row.addStretch()
        layout.addLayout(rt_row)

        # 多参数组合 + 缩写演示
        combo_row = QHBoxLayout()
        combo_row.addWidget(QLabel("参数组合:"))
        self.combo_date_fmt = QComboBox()
        self.combo_date_fmt.addItems([
            "YEAR MONTH DATE",
            "YEAR DATE",
            "MONTH DATE",
            "YEAR MONTH",
        ])
        self.combo_date_fmt.setToolTip("选择参数组合后点击 [演示发送]")
        combo_row.addWidget(self.combo_date_fmt)
        btn_demo_abbrev = QPushButton("演示缩写发送")
        btn_demo_abbrev.setToolTip("发送: *SET:DATE y 2026 mon 6 d 8 (小写可省略)")
        btn_demo_abbrev.clicked.connect(self._on_demo_abbrev)
        combo_row.addWidget(btn_demo_abbrev)
        combo_row.addStretch()
        layout.addLayout(combo_row)

        layout.addStretch()
        w.setLayout(layout)
        return w

    # ── Tab 2: 多闹钟调度器 (§4.3) ────────
    def _create_tab_alarm(self):
        w = QWidget()
        layout = QVBoxLayout()
        layout.setSpacing(8)

        # 7-row grid: Mon-Sun
        grid = QGridLayout()
        grid.setSpacing(4)
        days = ["周一 Mon", "周二 Tue", "周三 Wed", "周四 Thu", "周五 Fri", "周六 Sat", "周日 Sun"]
        self.alarm_spins = []  # [(h_spin, m_spin, s_spin, enabled_cb)]
        for i, label in enumerate(days):
            row = QHBoxLayout()
            row.addWidget(QLabel(label))
            row.addSpacing(10)
            hs = QSpinBox(); hs.setRange(0, 23); hs.setValue(6); hs.setPrefix("时"); hs.setMaximumWidth(70)
            ms = QSpinBox(); ms.setRange(0, 59); ms.setValue(0); ms.setPrefix("分"); ms.setMaximumWidth(70)
            ss = QSpinBox(); ss.setRange(0, 59); ss.setValue(0); ss.setPrefix("秒"); ss.setMaximumWidth(70)
            cb = QCheckBox("启用"); cb.setChecked(i < 5)
            row.addWidget(hs); row.addWidget(ms); row.addWidget(ss); row.addWidget(cb)
            row.addStretch()
            layout.addLayout(row)
            self.alarm_spins.append((hs, ms, ss, cb))

        # Buttons
        btn_row = QHBoxLayout()
        btn_apply = QPushButton("应用全部")
        btn_apply.clicked.connect(self._on_alarm_apply_all)
        btn_row.addWidget(btn_apply)
        btn_copy = QPushButton("复制到工作日")
        btn_copy.clicked.connect(self._on_alarm_copy_workdays)
        btn_row.addWidget(btn_copy)
        btn_clear = QPushButton("清除全部")
        btn_clear.clicked.connect(self._on_alarm_clear_all)
        btn_row.addWidget(btn_clear)
        btn_read = QPushButton("从MCU读取")
        btn_read.clicked.connect(lambda: self.send_cmd("*GET:ALARM"))
        btn_row.addWidget(btn_read)
        btn_stop = QPushButton("停止闹钟")
        btn_stop.setStyleSheet("background-color: #8B0000;")
        btn_stop.clicked.connect(self._on_stop_alarm)
        btn_row.addWidget(btn_stop)
        btn_row.addStretch()
        layout.addLayout(btn_row)

        self.lbl_alarm_status = QLabel("")
        self.lbl_alarm_status.setStyleSheet("color: #888; font-size: 13px;")
        layout.addWidget(self.lbl_alarm_status)

        hint = QLabel(
            "§4.3 多闹钟调度器: 每天可独立设置闹钟时间。\n"
            "MCU按 EXT 可切换当天闹钟使能。响铃按 FUNC 停止。"
        )
        hint.setStyleSheet("color: #666; font-size: 10px;")
        hint.setWordWrap(True)
        layout.addWidget(hint)
        layout.addStretch()
        w.setLayout(layout)
        return w

    # ── Tab 3: 显示控制 ─────────────────────
    def _create_tab_display(self):
        w = QWidget()
        layout = QVBoxLayout()
        layout.setSpacing(14)

        # 显示开关
        disp_row = QHBoxLayout()
        disp_row.addWidget(QLabel("显示开关:"))
        self.radio_disp_on = QRadioButton("开"); self.radio_disp_on.setChecked(True)
        self.radio_disp_off = QRadioButton("关")
        self.radio_disp_on.toggled.connect(lambda v: v and self.send_cmd("*SET:DISPLAY ON"))
        self.radio_disp_off.toggled.connect(lambda v: v and self.send_cmd("*SET:DISPLAY OFF"))
        disp_row.addWidget(self.radio_disp_on)
        disp_row.addWidget(self.radio_disp_off)
        disp_row.addStretch()
        layout.addLayout(disp_row)

        # 显示模式 — 3 toggle buttons (MCU DISP is cyclic, PC tracks state)
        mode_row = QHBoxLayout()
        mode_row.addWidget(QLabel("显示模式:"))
        self.btn_mode_time = QPushButton("HH.MM.SS 时间"); self.btn_mode_time.setCheckable(True); self.btn_mode_time.setChecked(True)
        self.btn_mode_date_s = QPushButton("YY.MM.DD 短日期"); self.btn_mode_date_s.setCheckable(True)
        self.btn_mode_date_l = QPushButton("YYYY.MMDD 长日期"); self.btn_mode_date_l.setCheckable(True)
        self.btn_mode_time.clicked.connect(lambda: self._on_disp_mode_set(0))
        self.btn_mode_date_s.clicked.connect(lambda: self._on_disp_mode_set(1))
        self.btn_mode_date_l.clicked.connect(lambda: self._on_disp_mode_set(2))
        self._disp_btns = [self.btn_mode_time, self.btn_mode_date_s, self.btn_mode_date_l]
        for b in self._disp_btns:
            b.setStyleSheet(
                "QPushButton { background-color: #4a4a4a; border: 2px solid #666;"
                "border-radius: 4px; padding: 6px 12px; color: white; }"
                "QPushButton:checked { background-color: #0078d7; border-color: #0078d7; }"
            )
        mode_row.addWidget(self.btn_mode_time)
        mode_row.addWidget(self.btn_mode_date_s)
        mode_row.addWidget(self.btn_mode_date_l)
        mode_row.addStretch()
        layout.addLayout(mode_row)

        # 格式方向
        fmt_row = QHBoxLayout()
        fmt_row.addWidget(QLabel("格式方向:"))
        self.radio_fmt_left = QRadioButton("左 →"); self.radio_fmt_left.setChecked(True)
        self.radio_fmt_right = QRadioButton("→ 右")
        self.radio_fmt_left.toggled.connect(
            lambda v: v and self.send_cmd("*SET:FORMAT LEFT"))
        self.radio_fmt_right.toggled.connect(
            lambda v: v and self.send_cmd("*SET:FORMAT RIGHT"))
        fmt_row.addWidget(self.radio_fmt_left)
        fmt_row.addWidget(self.radio_fmt_right)
        fmt_row.addStretch()
        layout.addLayout(fmt_row)

        # 滚动速度
        spd_row = QHBoxLayout()
        spd_row.addWidget(QLabel("滚动速度:"))
        self.radio_spd_slow = QRadioButton("慢 (500ms)")
        self.radio_spd_fast = QRadioButton("快 (250ms)")
        self._spd_updating = False
        self.radio_spd_slow.setChecked(True)
        self.radio_spd_slow.toggled.connect(self._on_speed_toggled)
        self.radio_spd_fast.toggled.connect(self._on_speed_toggled)
        spd_row.addWidget(self.radio_spd_slow)
        spd_row.addWidget(self.radio_spd_fast)
        spd_row.addStretch()
        layout.addLayout(spd_row)

        # 昼夜模式
        mode_switch_row = QHBoxLayout()
        mode_switch_row.addWidget(QLabel("昼夜模式:"))
        btn_day = QPushButton("☀️ 日间模式")
        btn_day.clicked.connect(lambda: self.send_cmd("*SET:MODE DAY"))
        mode_switch_row.addWidget(btn_day)
        btn_night = QPushButton("🌙 夜间模式")
        btn_night.clicked.connect(lambda: self.send_cmd("*SET:MODE NIGHT"))
        mode_switch_row.addWidget(btn_night)
        mode_switch_row.addStretch()
        layout.addLayout(mode_switch_row)

        # LED 直接控制
        led_row = QHBoxLayout()
        led_row.addWidget(QLabel("LED控制:"))
        btn_led_on = QPushButton("全亮")
        btn_led_on.clicked.connect(lambda: self.send_cmd("*SET:LED FF"))
        led_row.addWidget(btn_led_on)
        btn_led_off = QPushButton("全灭")
        btn_led_off.clicked.connect(lambda: self.send_cmd("*SET:LED 00"))
        led_row.addWidget(btn_led_off)
        led_row.addStretch()
        layout.addLayout(led_row)

        # 蜂鸣器
        beep_row = QHBoxLayout()
        beep_row.addWidget(QLabel("蜂鸣器:"))
        btn_beep_short = QPushButton("短鸣 (200ms)")
        btn_beep_short.clicked.connect(lambda: self.send_cmd("*SET:BEEP 200"))
        beep_row.addWidget(btn_beep_short)
        btn_beep_long = QPushButton("长鸣 (1000ms)")
        btn_beep_long.clicked.connect(lambda: self.send_cmd("*SET:BEEP 1000"))
        beep_row.addWidget(btn_beep_long)
        beep_row.addStretch()
        layout.addLayout(beep_row)

        layout.addStretch()
        w.setLayout(layout)
        return w

    # ── Tab 4: 滚动消息 ─────────────────────
    def _create_tab_message(self):
        w = QWidget()
        layout = QVBoxLayout()
        layout.setSpacing(12)

        input_row = QHBoxLayout()
        input_row.addWidget(QLabel("消息内容:"))
        self.msg_input = QLineEdit()
        self.msg_input.setMaxLength(32)
        self.msg_input.setPlaceholderText("最多32个字符，超过8个将自动滚动...")
        self.msg_input.textChanged.connect(self._on_msg_text_changed)
        self.msg_input.returnPressed.connect(self._on_send_msg)
        input_row.addWidget(self.msg_input)
        layout.addLayout(input_row)

        info_row = QHBoxLayout()
        self.lbl_msg_count = QLabel("剩余: 32 字")
        self.lbl_msg_count.setStyleSheet("color: #888;")
        info_row.addWidget(self.lbl_msg_count)
        info_row.addStretch()
        btn_send = QPushButton("发送消息")
        btn_send.clicked.connect(self._on_send_msg)
        info_row.addWidget(btn_send)
        btn_clear = QPushButton("清除消息")
        btn_clear.clicked.connect(self._on_clear_msg)
        info_row.addWidget(btn_clear)
        layout.addLayout(info_row)

        # 预览
        preview_group = QGroupBox("消息预览")
        preview_layout = QVBoxLayout()
        self.lbl_msg_preview = QLabel(' '.join(['_'] * 8))
        self.lbl_msg_preview.setStyleSheet(
            "color: #FFE66D; font-size: 22px; font-family: Consolas;"
        )
        self.lbl_msg_preview.setAlignment(Qt.AlignCenter)
        preview_layout.addWidget(self.lbl_msg_preview)
        preview_group.setLayout(preview_layout)
        layout.addWidget(preview_group)

        layout.addStretch()
        w.setLayout(layout)
        return w

    # ── Tab 5: 快捷操作 ─────────────────────
    def _create_tab_quick(self):
        w = QWidget()
        layout = QVBoxLayout()
        layout.setSpacing(10)

        # 系统操作
        sys_group = QGroupBox("系统操作")
        sys_row = QHBoxLayout()
        btn_rst = QPushButton("系统复位 *RST")
        btn_rst.setStyleSheet("background-color: #8B0000;")
        btn_rst.clicked.connect(self._on_reset)
        sys_row.addWidget(btn_rst)
        btn_ping = QPushButton("PING 测试")
        btn_ping.clicked.connect(lambda: self.send_cmd("*PING"))
        sys_row.addWidget(btn_ping)
        sys_row.addStretch()
        sys_group.setLayout(sys_row)
        layout.addWidget(sys_group)

        # 演示序列
        demo_group = QGroupBox("演示序列")
        demo_row = QHBoxLayout()
        self.btn_demo = QPushButton("▶ 执行演示序列")
        self.btn_demo.clicked.connect(self._on_demo_sequence)
        self.btn_demo.setStyleSheet(
            "background-color: #006400; font-size: 14px; padding: 12px;"
        )
        demo_row.addWidget(self.btn_demo)
        demo_group.setLayout(demo_row)
        layout.addWidget(demo_group)

        # 演示说明
        demo_desc = QLabel(
            "演示序列将依次执行:\n"
            "  1. *RST — 复位系统\n"
            "  2. *SET:TIME — 设置时间 12:34:56\n"
            "  3. *SET:DATE — 设置日期 2026.06.08\n"
            "  4. *SET:MSG Hello World — 发送滚动消息\n"
            "  5. *SET:DISPLAY OFF → ON — 熄屏再亮屏\n"
            "  6. *SET:FORMAT RIGHT → LEFT — 切换方向\n"
            "  7. *GET:TIME — 读取时间确认"
        )
        demo_desc.setStyleSheet("color: #666; font-size: 11px;")
        layout.addWidget(demo_desc)

        layout.addStretch()
        w.setLayout(layout)
        return w

    # ── Tab 6: 扩展功能 (E1-E4) ────────────
    def _create_tab_extensions(self):
        w = QWidget()
        layout = QVBoxLayout()
        layout.setSpacing(10)

        # ── E1: NTP 对时 ──
        ntp_group = QGroupBox("E1: NTP 网络对时")
        ntp_layout = QVBoxLayout()
        ntp_row = QHBoxLayout()
        btn_ntp = QPushButton("🕐 同步时间 (NTP)")
        btn_ntp.clicked.connect(self._on_ntp_sync)
        ntp_row.addWidget(btn_ntp)
        self.lbl_ntp_status = QLabel("未同步")
        self.lbl_ntp_status.setStyleSheet("color: #888;")
        ntp_row.addWidget(self.lbl_ntp_status)
        ntp_row.addStretch()
        ntp_layout.addLayout(ntp_row)
        ntp_hint = QLabel("USER1 键自动触发，从 ntp.aliyun.com 获取标准时间。")
        ntp_hint.setStyleSheet("color: #666; font-size: 10px;")
        ntp_layout.addWidget(ntp_hint)
        ntp_group.setLayout(ntp_layout)
        layout.addWidget(ntp_group)

        # ── E2: 天气 ──
        wea_group = QGroupBox("E2: 天气获取")
        wea_layout = QVBoxLayout()
        wea_row = QHBoxLayout()
        btn_wea = QPushButton("🌤 获取天气 (wttr.in)")
        btn_wea.clicked.connect(self._on_weather_fetch)
        wea_row.addWidget(btn_wea)
        self.lbl_weather_status = QLabel("未获取")
        self.lbl_weather_status.setStyleSheet("color: #888;")
        wea_row.addWidget(self.lbl_weather_status)
        wea_row.addStretch()
        wea_layout.addLayout(wea_row)
        wea_row2 = QHBoxLayout()
        self.wea_auto_check = QCheckBox("30分钟自动刷新")
        self.wea_auto_check.toggled.connect(self._on_weather_auto_toggle)
        wea_row2.addWidget(self.wea_auto_check)
        wea_row2.addStretch()
        wea_layout.addLayout(wea_row2)
        wea_hint = QLabel("USER2 键在MCU端短显温度+天气。wttr.in 免费API，无需Key。")
        wea_hint.setStyleSheet("color: #666; font-size: 10px;")
        wea_layout.addWidget(wea_hint)
        wea_group.setLayout(wea_layout)
        layout.addWidget(wea_group)

        # ── E3: 自动昼夜 ──
        day_group = QGroupBox("E3: 自动昼夜模式")
        day_layout = QVBoxLayout()
        day_row = QHBoxLayout()
        self.day_auto_check = QCheckBox("启用自动昼夜切换")
        self.day_auto_check.toggled.connect(self._on_auto_day_toggle)
        day_row.addWidget(self.day_auto_check)
        self.lbl_day_status = QLabel("日出/日落: --")
        self.lbl_day_status.setStyleSheet("color: #888;")
        day_row.addWidget(self.lbl_day_status)
        day_row.addStretch()
        day_layout.addLayout(day_row)
        btn_day_now = QPushButton("☀️ 立即日间")
        btn_day_now.clicked.connect(lambda: self.send_cmd("*SET:MODE DAY"))
        day_layout2 = QHBoxLayout()
        day_layout2.addWidget(btn_day_now)
        btn_night_now = QPushButton("🌙 立即夜间")
        btn_night_now.clicked.connect(lambda: self.send_cmd("*SET:MODE NIGHT"))
        day_layout2.addWidget(btn_night_now)
        day_layout2.addStretch()
        day_layout.addLayout(day_layout2)
        day_group.setLayout(day_layout)
        layout.addWidget(day_group)

        # ── E4: 可视化 ──
        viz_group = QGroupBox("E4: 数据可视化")
        viz_row = QHBoxLayout()
        btn_viz = QPushButton("📊 查看图表")
        btn_viz.clicked.connect(self._on_show_charts)
        viz_row.addWidget(btn_viz)
        self.chart_type = QComboBox()
        self.chart_type.addItem("闹钟触发分布")
        self.chart_type.addItem("每日事件计数")
        self.chart_type.addItem("NTP 同步记录")
        viz_row.addWidget(self.chart_type)
        viz_row.addStretch()
        viz_group.setLayout(viz_row)
        layout.addWidget(viz_group)

        # CSV log file
        self.csv_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "events.csv")

        layout.addStretch()
        w.setLayout(layout)
        return w

    # ── 日志区域 ────────────────────────────
    def _create_log_group(self):
        group = QGroupBox("通信日志")
        layout = QVBoxLayout()

        # 手动命令输入栏
        cmd_row = QHBoxLayout()
        cmd_row.addWidget(QLabel("手动命令:"))
        self.cmd_input = QLineEdit()
        self.cmd_input.setPlaceholderText(
            "输入命令后按回车... 如 *PING, *GET:TIME, *SET:DATE YEAR 2026 MONTH 6 DATE 8"
        )
        self.cmd_input.returnPressed.connect(self._on_send_custom)
        cmd_row.addWidget(self.cmd_input)
        btn_send_custom = QPushButton("发送")
        btn_send_custom.clicked.connect(self._on_send_custom)
        cmd_row.addWidget(btn_send_custom)
        layout.addLayout(cmd_row)

        self.log_text = QTextEdit()
        self.log_text.setMaximumHeight(150)
        self.log_text.setReadOnly(True)
        self.log_text.setFont(QFont("Consolas", 10))
        layout.addWidget(self.log_text)

        btn_row = QHBoxLayout()
        btn_clear = QPushButton("清空")
        btn_clear.clicked.connect(lambda: self.log_text.clear())
        btn_row.addWidget(btn_clear)
        btn_export = QPushButton("导出")
        btn_export.clicked.connect(self.export_log)
        btn_row.addWidget(btn_export)
        btn_row.addStretch()

        self.show_send = QCheckBox("显示发送"); self.show_send.setChecked(True)
        self.show_recv = QCheckBox("显示接收"); self.show_recv.setChecked(True)
        btn_row.addWidget(self.show_send)
        btn_row.addWidget(self.show_recv)

        layout.addLayout(btn_row)
        group.setLayout(layout)
        return group

    # ── 初始化显示 ──────────────────────────
    def _init_default_display(self):
        for i in range(8):
            self.segments[i].set_digit(str(i + 1), False)
        self.log("初始化: 显示 1-8", "info")

    # ================================================================
    # 串口操作方法
    # ================================================================
    def refresh_ports(self):
        self.com_combo.clear()
        ports = serial.tools.list_ports.comports()
        for port in ports:
            self.com_combo.addItem(f"{port.device} - {port.description}")
        if not ports:
            self.com_combo.addItem("无可用串口")

    def toggle_serial(self):
        if self.serial_thread.running:
            self.serial_thread.disconnect()
            self.connect_btn.setText("连接")
            self._set_connection_status(False)
            self._ping_timer.stop()
            self._ping_offline = False
            self.log("串口已断开", "info")
        else:
            text = self.com_combo.currentText()
            if text and "无可用" not in text:
                port = text.split(' - ')[0]
                baud = int(self.baud_combo.currentText())
                ok, msg = self.serial_thread.connect(port, baud)
                if ok:
                    self.connect_btn.setText("断开")
                    self._set_connection_status(True, port)
                    self.log(f"连接成功: {port} @ {baud}", "success")
                    # 连接后立即查询状态，重置模式追踪
                    self._disp_mode_idx = 0
                    self._update_disp_btns(0)
                    QTimer.singleShot(500, lambda: self.send_cmd("*GET:ALARM"))
                    QTimer.singleShot(700, lambda: self.send_cmd("*GET:MODE"))
                    # 启动 1Hz PING
                    self._last_pong_time = time.time()
                    self._ping_offline = False
                    self._ping_timer.start(1000)
                else:
                    self.log(f"连接失败: {msg}", "error")
                    QMessageBox.critical(self, "错误", msg)
            else:
                QMessageBox.warning(self, "警告", "请选择串口")

    def send_cmd(self, cmd):
        if self.serial_thread.send_command(cmd):
            if self.show_send.isChecked():
                self.log(f"{cmd}", "send")
            self.leds1[3].set_state(True)  # TX LED
            QTimer.singleShot(100, lambda: self.leds1[3].set_state(False))
            return True
        else:
            self.log(f"发送失败: {cmd}", "error")
            return False

    def _ping_tick(self):
        """1Hz PING + 3s 超时离线检测"""
        now = time.time()
        if self._ping_offline:
            # 重试: 发 PING, 若收到 PONG 则恢复
            self.send_cmd("*PING")
            return
        if now - self._last_pong_time > 3.0:
            # 3s 无 PONG → 标记离线
            self._ping_offline = True
            self._set_connection_status(False)
            self.status_bar.showMessage("⚠ 心跳超时 (离线)")
            self.log("心跳超时: MCU 离线", "error")
            return
        self.send_cmd("*PING")

    def _set_alarm_indicator(self, enabled):
        """更新状态栏 ALARM 使能指示"""
        if enabled:
            self.alarm_indicator.setText("⏰ 闹钟: ✓")
            self.alarm_indicator.setStyleSheet("color: #FF6B6B; font-weight: bold;")
        else:
            self.alarm_indicator.setText("⏰ 闹钟: ✗")
            self.alarm_indicator.setStyleSheet("color: #888; font-weight: bold;")

    def _on_speed_toggled(self, checked):
        """滚动速度 radio 切换 → 发送 *SET:KEY SPEED 到 MCU"""
        if self._spd_updating or not checked:
            return
        self.send_cmd("*SET:KEY SPEED")

    # ================================================================
    # 标签页事件处理
    # ================================================================

    # ── Tab 1: 日期时间 ─────────────────────
    def _on_set_date(self):
        y = self.sp_year.value()
        m = self.sp_month.value()
        d = self.sp_day.value()
        self.send_cmd(f"*SET:DATE YEAR {y} MONTH {m} DATE {d}")

    def _on_set_time(self):
        h = self.sp_hour.value()
        mi = self.sp_min.value()
        s = self.sp_sec.value()
        self.send_cmd(f"*SET:TIME HOUR {h} MIN {mi} SEC {s}")

    def _on_get_time_date(self):
        self.send_cmd("*GET:TIME")
        QTimer.singleShot(200, lambda: self.send_cmd("*GET:DATE"))

    def _on_demo_abbrev(self):
        """演示缩写语法: 仅大写字母必输, 小写可省略"""
        y = self.sp_year.value()
        m = self.sp_month.value()
        d = self.sp_day.value()
        fmt = self.combo_date_fmt.currentText()
        # 构造缩写命令: 仅大写字母必输
        abbrev_map = {
            "YEAR MONTH DATE": f"*SET:DATE y {y} mon {m} d {d}",
            "YEAR DATE":       f"*SET:DATE y {y} d {d}",
            "MONTH DATE":      f"*SET:DATE mon {m} d {d}",
            "YEAR MONTH":      f"*SET:DATE y {y} mon {m}",
        }
        cmd = abbrev_map.get(fmt, f"*SET:DATE Y {y} MON {m} D {d}")
        self.log(f"缩写演示 [{fmt}]: {cmd}", "event")
        self.send_cmd(cmd)

    def _on_fill_pc_time(self):
        from datetime import datetime
        now = datetime.now()
        self.sp_year.setValue(now.year)
        self.sp_month.setValue(now.month)
        self.sp_day.setValue(now.day)
        self.sp_hour.setValue(now.hour)
        self.sp_min.setValue(now.minute)
        self.sp_sec.setValue(now.second)
        self.log("已填入电脑当前时间", "info")

    # ── Tab 2: 闹钟 ─────────────────────────
    # ── Tab 2: 多闹钟 ───────────────────────
    def _on_alarm_apply_all(self):
        """Send all 7 alarm slots to MCU"""
        days = ["MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN"]
        for i, (day, (hs, ms, ss, cb)) in enumerate(zip(days, self.alarm_spins)):
            cmd = f"*SET:ALARM {day} HOUR {hs.value()} MIN {ms.value()} SEC {ss.value()}"
            delay = i * 150
            QTimer.singleShot(delay, lambda c=cmd: self.send_cmd(c))
            enabled_cmd = f"*SET:ALARM {day} {'ON' if cb.isChecked() else 'OFF'}"
            QTimer.singleShot(delay + 80, lambda c=enabled_cmd: self.send_cmd(c))
        self.lbl_alarm_status.setText("已全部应用")
        self.lbl_alarm_status.setStyleSheet("color: #95E77E; font-size: 13px;")
        self._log_csv("ALARM_SET", "ALL_7_SLOTS")

    def _on_alarm_copy_workdays(self):
        """Copy row 0 (Monday) values to Mon-Fri"""
        h0, m0, s0, c0 = self.alarm_spins[0]
        for i in range(5):
            hs, ms, ss, cb = self.alarm_spins[i]
            hs.setValue(h0.value()); ms.setValue(m0.value()); ss.setValue(s0.value())
            cb.setChecked(True)
        self.lbl_alarm_status.setText("已复制到周一至周五")

    def _on_alarm_clear_all(self):
        """Disable all alarms"""
        days = ["MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN"]
        for i, day in enumerate(days):
            QTimer.singleShot(i * 80, lambda d=day: self.send_cmd(f"*SET:ALARM {d} OFF"))
        self.lbl_alarm_status.setText("已全部禁用")
        self._update_alarm_ui()

    def _on_set_alarm(self):
        h = self.sp_alarm_h.value()
        m = self.sp_alarm_m.value()
        s = self.sp_alarm_s.value()
        self.send_cmd(f"*SET:ALARM HOUR {h} MIN {m} SEC {s}")

    def _on_toggle_alarm(self):
        if self._alarm_enabled:
            self.send_cmd("*SET:ALARM OFF")
        else:
            h = self.sp_alarm_h.value()
            m = self.sp_alarm_m.value()
            s = self.sp_alarm_s.value()
            self.send_cmd(f"*SET:ALARM HOUR {h} MIN {m} SEC {s}")

    def _on_stop_alarm(self):
        self.send_cmd("*SET:KEY FUNC")

    def _on_send_custom(self):
        """手动命令输入发送"""
        cmd = self.cmd_input.text().strip()
        if cmd:
            self.send_cmd(cmd)
            self.cmd_input.clear()

    # ── Tab 3: 显示控制 ─────────────────────
    def _on_disp_mode_set(self, target):
        """Sync to target mode by sending forward DISP keys.
        MCU cycles TIME(0)->SHORT(1)->LONG(2)->TIME(0).
        Calculate forward distance and send that many DISP keys."""
        # Always keep the button checked (prevent unchecked on re-click)
        self._update_disp_btns(target)
        steps = (target - self._disp_mode_idx + 3) % 3
        self._disp_mode_idx = target
        if steps == 0:
            return  # already at target
        self.log(f"显示模式: 发送 {steps} 次 DISP 键")
        # Send multiple DISP keys with 120ms delay between each
        for i in range(steps):
            QTimer.singleShot(i * 120, lambda: self.send_cmd("*SET:KEY DISP"))

    def _update_disp_btns(self, cur):
        """Update button checked states for display mode."""
        for i, btn in enumerate(self._disp_btns):
            btn.setChecked(i == cur)

    # ── Tab 4: 消息 ─────────────────────────
    def _on_msg_text_changed(self, text):
        remaining = 32 - len(text)
        self.lbl_msg_count.setText(f"剩余: {remaining} 字")
        preview = list(text[:8] if text else '_' * 8)
        while len(preview) < 8:
            preview.append('_')
        self.lbl_msg_preview.setText(' '.join(preview))

    def _on_send_msg(self):
        msg = self.msg_input.text().strip()
        if msg:
            self.send_cmd(f"*SET:MSG {msg}")

    def _on_clear_msg(self):
        self.send_cmd("*SET:MSG")
        self.msg_input.clear()

    # ── Tab 5: 快捷操作 ─────────────────────
    def _on_reset(self):
        reply = QMessageBox.question(
            self, "确认复位",
            "确定要复位MCU系统吗？\n这将重置时钟、闹钟和显示设置。",
            QMessageBox.Yes | QMessageBox.No, QMessageBox.No
        )
        if reply == QMessageBox.Yes:
            self.send_cmd("*RST")
            self._disp_mode_idx = 0
            self._update_disp_btns(0)

    def _on_demo_sequence(self):
        """执行演示序列"""
        self.btn_demo.setEnabled(False)
        self.btn_demo.setText("⏳ 演示中...")
        self.log("=== 开始演示序列 ===", "event")

        seq = [
            ("*RST", 0),
            ("*SET:TIME HOUR 12 MIN 34 SEC 56", 500),
            ("*SET:DATE YEAR 2026 MONTH 6 DATE 8", 1000),
            ("*SET:MSG Hello World", 1500),
            ("*SET:DISPLAY OFF", 2200),
            ("*SET:DISPLAY ON", 2700),
            ("*SET:FORMAT RIGHT", 3200),
            ("*SET:FORMAT LEFT", 3700),
            ("*GET:TIME", 4200),
        ]

        for cmd, delay in seq:
            QTimer.singleShot(delay, lambda c=cmd: self.send_cmd(c))

        QTimer.singleShot(5200, self._on_demo_done)

    def _on_demo_done(self):
        self.btn_demo.setEnabled(True)
        self.btn_demo.setText("▶ 执行演示序列")
        self.log("=== 演示序列完成 ===", "event")

    # ── 虚拟按键 ────────────────────────────
    def _on_key_click(self, name):
        self.send_cmd(f"*SET:KEY {name}")
        self.log(f"按键: {name}", "event")
        # USER1/USER2 also trigger PC-side actions directly
        if name == "USER1":
            QTimer.singleShot(200, self._on_ntp_sync)
        elif name == "USER2":
            QTimer.singleShot(200, self._on_weather_fetch)
        btn = self.key_btns.get(name)
        if btn:
            btn.setStyleSheet(
                "background-color: #0078d7; color: white;"
                "border: 1px solid #0078d7; border-radius: 5px; padding: 8px;"
            )
            QTimer.singleShot(150, lambda b=btn: b.setStyleSheet(""))

    # ================================================================
    # 扩展功能处理
    # ================================================================

    def _on_ntp_sync(self):
        """E1: NTP time sync (QThread, non-blocking)"""
        self.lbl_ntp_status.setText("同步中...")
        self.lbl_ntp_status.setStyleSheet("color: #FFE66D;")
        self._ntp_thread = QThread(self)          # parent=self 防止 GC 过早销毁
        self._ntp_worker = NtpWorker()
        self._ntp_worker.moveToThread(self._ntp_thread)
        self._ntp_worker.finished.connect(self._on_ntp_result)
        self._ntp_worker.finished.connect(self._ntp_thread.quit)
        self._ntp_thread.started.connect(self._ntp_worker.run)
        self._ntp_thread.finished.connect(self._ntp_worker.deleteLater)
        self._ntp_thread.finished.connect(self._ntp_thread.deleteLater)
        self._ntp_thread.start()

    def _on_ntp_result(self, t):
        """MAIN thread callback after NTP fetch"""
        if t is None:
            self.lbl_ntp_status.setText("失败 (网络?)")
            self.lbl_ntp_status.setStyleSheet("color: #FF6B6B;")
            self.log("NTP同步失败", "error")
            return
        y, mo, d, h, mi, s = t
        cmds = [
            f"*SET:DATE YEAR {y} MONTH {mo} DATE {d}",
            f"*SET:TIME HOUR {h} MIN {mi} SEC {s}",
            "*NTP",
        ]
        for i, cmd in enumerate(cmds):
            QTimer.singleShot(i * 200, lambda c=cmd: self.send_cmd(c))
        ts = format_time(*t)
        self.lbl_ntp_status.setText(f"已同步: {ts}")
        self.lbl_ntp_status.setStyleSheet("color: #95E77E;")
        self.log(f"NTP同步: {ts}", "success")
        self._log_csv("NTP_SYNC", ts)

    def _on_weather_fetch(self):
        """E2: Fetch weather from wttr.in (QThread, non-blocking)"""
        self.lbl_weather_status.setText("获取中...")
        self.lbl_weather_status.setStyleSheet("color: #FFE66D;")
        self._wea_thread = QThread(self)          # parent=self 防止 GC 过早销毁
        self._wea_worker = WeatherWorker()
        self._wea_worker.moveToThread(self._wea_thread)
        self._wea_worker.finished.connect(self._on_weather_result)
        self._wea_worker.finished.connect(self._wea_thread.quit)
        self._wea_thread.started.connect(self._wea_worker.run)
        self._wea_thread.finished.connect(self._wea_worker.deleteLater)
        self._wea_thread.finished.connect(self._wea_thread.deleteLater)
        self._wea_thread.start()

    def _on_weather_result(self, result):
        """MAIN thread callback after weather fetch"""
        if result is None:
            self.lbl_weather_status.setText("失败 (网络?)")
            self.lbl_weather_status.setStyleSheet("color: #FF6B6B;")
            self.log("天气获取失败", "error")
            return
        cmd, desc = result
        self.send_cmd(cmd)          # *SET:WEA <T> <COND> → MCU
        # 解析 "*SET:WEA 23 RAI" (3 部分: 前缀/温度/天气码)
        parts = cmd.split()
        if len(parts) >= 3:
            try:
                self._weather_temp = int(parts[1])
            except ValueError:
                self._weather_temp = -99
            self._weather_cond = parts[2][:3]
        else:
            self._weather_temp = -99
            self._weather_cond = "---"
        self._weather_desc = desc
        self.lbl_weather_status.setText(desc)
        self.lbl_weather_status.setStyleSheet("color: #95E77E;")
        self.log(f"天气: {desc}", "success")
        self._log_csv("WEATHER", desc)
        # PC SEG 直接显示天气 3s
        self._show_weather_on_seg()

    def _on_weather_auto_toggle(self, checked):
        """E2: Auto-refresh weather every 30 minutes"""
        if checked:
            self._on_weather_fetch()
            self._wea_timer = QTimer()
            self._wea_timer.timeout.connect(self._on_weather_fetch)
            self._wea_timer.start(30 * 60 * 1000)
        else:
            if hasattr(self, '_wea_timer') and self._wea_timer:
                self._wea_timer.stop()

    def _on_auto_day_toggle(self, checked):
        """E3: Enable/disable auto day/night mode"""
        if checked:
            self._check_day_night()
            self._day_timer = QTimer()
            self._day_timer.timeout.connect(self._check_day_night)
            self._day_timer.start(60000)  # Check every 60s
            self.lbl_day_status.setStyleSheet("color: #95E77E;")
            self.log("自动昼夜模式已启用", "info")
        else:
            if hasattr(self, '_day_timer') and self._day_timer:
                self._day_timer.stop()
            self.lbl_day_status.setText("日出/日落: --")
            self.lbl_day_status.setStyleSheet("color: #888;")

    def _check_day_night(self):
        """E3: Check current time against sunrise/sunset and send MODE command"""
        try:
            from astral import LocationInfo
            from astral.sun import sun
            city = LocationInfo("Shanghai", "China", "Asia/Shanghai", 31.23, 121.47)
            s = sun(city.observer, date=datetime.now())
            sunrise = s['sunrise']
            sunset = s['sunset']
            now = datetime.now(sunrise.tzinfo)
            self.lbl_day_status.setText(
                f"日出 {sunrise.strftime('%H:%M')} 日落 {sunset.strftime('%H:%M')}")
            if sunrise <= now < sunset:
                if self._current_mode != "DAY":
                    self.send_cmd("*SET:MODE DAY")
            else:
                if self._current_mode != "NIGHT":
                    self.send_cmd("*SET:MODE NIGHT")
        except Exception:
            self.lbl_day_status.setText("计算失败")
            self.lbl_day_status.setStyleSheet("color: #FF6B6B;")

    def _on_show_charts(self):
        """E4: Show matplotlib charts"""
        chart = self.chart_type.currentText()
        try:
            import matplotlib
            matplotlib.use('Qt5Agg')
            import matplotlib.pyplot as plt
            self._plot_chart(plt, chart)
        except Exception as e:
            QMessageBox.warning(self, "图表错误", str(e))

    def _plot_chart(self, plt, chart_type):
        """E4: Generate matplotlib chart"""
        if not os.path.exists(self.csv_path):
            QMessageBox.information(self, "提示", "暂无事件数据")
            return
        rows = []
        with open(self.csv_path, 'r', encoding='utf-8') as f:
            reader = csv.reader(f)
            header = next(reader, None)
            rows = list(reader)
        if not rows:
            QMessageBox.information(self, "提示", "暂无事件数据")
            return

        plt.figure(figsize=(8, 5))
        if chart_type == "闹钟触发分布":
            alarms = [r for r in rows if r[0] == "ALARM" or r[0] == "ALARM_OFF"]
            if alarms:
                hours = []
                for a in alarms:
                    try:
                        h = int(a[1].split(' ')[1].split(':')[0]) if ' ' in a[1] else 0
                        hours.append(h)
                    except:
                        hours.append(0)
                plt.hist(hours, bins=24, range=(0, 24), color='orange', edgecolor='black')
                plt.xlabel('Hour')
                plt.ylabel('Count')
                plt.title('Alarm Distribution by Hour')
            else:
                plt.text(0.5, 0.5, 'No alarm data', ha='center')
        elif chart_type == "每日事件计数":
            dates = {}
            for r in rows:
                d = r[2][:10] if len(r) > 2 else 'unknown'
                dates[d] = dates.get(d, 0) + 1
            if dates:
                d_sorted = sorted(dates.items())[-14:]
                plt.bar([d[0][5:] for d in d_sorted], [d[1] for d in d_sorted])
                plt.xlabel('Date')
                plt.xticks(rotation=45)
                plt.title('Events per Day')
        else:  # NTP sync record
            ntps = [r for r in rows if r[0] == "NTP_SYNC"]
            if ntps:
                times = [n[1] for n in ntps[-20:]]
                plt.plot(range(len(times)), [1]*len(times), 'go-', markersize=8)
                plt.yticks([1], ['Sync'])
                plt.xlabel('Sync #')
                plt.title('NTP Sync Timeline')
            else:
                plt.text(0.5, 0.5, 'No NTP sync data', ha='center')
        plt.tight_layout()
        plt.show()

    def _log_csv(self, event_type, detail):
        """E4: Log event to events.csv"""
        try:
            exists = os.path.exists(self.csv_path)
            with open(self.csv_path, 'a', newline='', encoding='utf-8') as f:
                writer = csv.writer(f)
                if not exists:
                    writer.writerow(['Event', 'Detail', 'Timestamp'])
                writer.writerow([event_type, detail, datetime.now().isoformat()])
        except Exception:
            pass

    # ================================================================
    # 数据接收与协议解析（增强版）
    # ================================================================
    def handle_data(self, data):
        data_upper = data.upper()

        # Heartbeat events handled silently (flood the log otherwise)
        is_heartbeat = data_upper.startswith("*EVT:DISP") or data_upper.startswith("*EVT:LED")

        if self.show_recv.isChecked() and not is_heartbeat:
            self.log(f"{data}", "recv")

        # RX LED 闪烁
        self.leds2[0].set_state(True)
        QTimer.singleShot(100, lambda: self.leds2[0].set_state(False))

        # ── *EVT:DISP <8chars> <dpHex> (skip while PC shows weather) ──
        if data_upper.startswith("*EVT:DISP") and not self._show_weather_seg:
            parts = data.split()
            if len(parts) >= 3:
                disp = parts[1][:8].ljust(8, ' ')
                dp_hex = parts[2]
                try:
                    dp_val = int(dp_hex, 16)
                except ValueError:
                    dp_val = 0
                for i in range(8):
                    dp_on = bool(dp_val & (1 << (7 - i)))
                    # P4: night mode — only show positions 0-3 (HH.MM)
                    ch_raw = disp[i] if disp[i] != '_' else ' '
                    if self._current_mode == "NIGHT" and i >= 4:
                        ch_raw = ' '
                    if ch_raw == '.':
                        self.segments[i].set_digit('.', False)
                    else:
                        self.segments[i].set_digit(ch_raw, dp_on)
                # 更新 MCU 时间显示
                try:
                    h, m, s = int(disp[0:2]), int(disp[2:4]), int(disp[4:6])
                    self.lbl_mcu_time.setText(f"{h:02d}:{m:02d}:{s:02d}")
                except (ValueError, IndexError):
                    pass

        # ── *EVT:LED <hex2> ──
        elif data_upper.startswith("*EVT:LED"):
            parts = data.split()
            if len(parts) >= 2:
                try:
                    val = int(parts[1], 16)
                except ValueError:
                    val = 0
                for i in range(4):
                    self.leds1[i].set_state(bool(val & (1 << i)))
                for i in range(4):
                    self.leds2[i].set_state(bool(val & (1 << (i + 4))))

        # ── *PONG <uptime> ──
        elif data_upper.startswith("*PONG"):
            parts = data.split()
            if len(parts) >= 2:
                try:
                    self._set_uptime_display(int(parts[1]))
                except ValueError:
                    pass
            # 更新心跳时间戳, 恢复在线状态
            self._last_pong_time = time.time()
            if self._ping_offline:
                self._ping_offline = False
                self._set_connection_status(True)
                self.status_bar.showMessage("已恢复连接", 3000)
                self.log("心跳恢复: MCU 重新在线", "success")
            self.leds1[0].set_state(True)
            QTimer.singleShot(100, lambda: self.leds1[0].set_state(False))

        # ── *EVT:KEY <NAME> ──
        elif data_upper.startswith("*EVT:KEY"):
            parts = data.split()
            if len(parts) >= 2:
                key_name = parts[1].upper()

                # 贪吃蛇游戏模式: 方向键路由到游戏
                if self.game_widget._game_running:
                    self.game_widget.on_key(key_name)
                    if key_name == "USER1":
                        self.game_widget.pause_game()
                    return

                self.log(f"MCU按键: {key_name}", "event")
                # Track edit mode state on PC for edit highlight mirror
                if key_name == "FUNC":
                    self._edit_mode = (self._edit_mode + 1) % 4 if self._edit_mode < 3 else 0
                    self._edit_field = 0
                elif key_name == "SHIFT":
                    self._edit_field = (self._edit_field + 1) % 3
                elif key_name in ("SAVE",):
                    self._edit_mode = 0; self._edit_field = 0
                if key_name == "USER1":
                    QTimer.singleShot(100, self._on_ntp_sync)
                elif key_name == "USER2":
                    QTimer.singleShot(100, self._on_weather_fetch)

        # ── *EVT:ALARM (响铃) vs *EVT:ALARM_OFF ──
        elif data_upper.startswith("*EVT:ALARM_OFF"):
            self._alarm_ringing = False
            self._alarm_blink.stop()
            self.leds1[1].set_state(False)
            self.lbl_alarm_status.setText("闹钟已停止")
            self.lbl_alarm_status.setStyleSheet("color: #888; font-size: 14px;")
            self.status_bar.showMessage("闹钟已停止", 2000)

        elif data_upper.startswith("*EVT:ALARM"):
            self._alarm_ringing = True
            self._alarm_blink.start(250)  # blink LED1 at 4Hz
            self._log_csv("ALARM", "ON")
            self.lbl_alarm_status.setText("⏰ 闹钟响铃中!")
            self.lbl_alarm_status.setStyleSheet(
                "color: #FF6B6B; font-size: 14px; font-weight: bold;"
            )
            self.status_bar.showMessage("⏰ 闹钟！", 5000)
            self._flash_alarm(5)

        # ── *EVT:EDIT <TYPE> <VALUE> ──
        elif data_upper.startswith("*EVT:EDIT"):
            parts = data.split()
            if len(parts) >= 3:
                edit_type = parts[1].upper()
                edit_val = parts[2]
                self.log(f"MCU编辑保存: {edit_type} = {edit_val}", "event")
                if edit_type == "DATE":
                    try:
                        y = int(edit_val[:4])
                        mo = int(edit_val[5:7])
                        d = int(edit_val[8:10])
                        self.sp_year.setValue(y)
                        self.sp_month.setValue(mo)
                        self.sp_day.setValue(d)
                    except (ValueError, IndexError):
                        pass
                elif edit_type == "TIME":
                    try:
                        h = int(edit_val[:2])
                        mi = int(edit_val[3:5])
                        s = int(edit_val[6:8])
                        self.sp_hour.setValue(h)
                        self.sp_min.setValue(mi)
                        self.sp_sec.setValue(s)
                    except (ValueError, IndexError):
                        pass
                elif edit_type == "ALARM":
                    self.lbl_alarm_status.setText(f"闹钟已保存: {edit_val}")
                    self.lbl_alarm_status.setStyleSheet(
                        "color: #95E77E; font-size: 14px;"
                    )

        # ── *EVT:MODE <STATE> ──
        elif data_upper.startswith("*EVT:MODE"):
            parts = data.split()
            if len(parts) >= 2:
                self._set_mode_indicator(parts[1].upper())

        # ── *EVT:SPEED SLOW|FAST ──
        elif data_upper.startswith("*EVT:SPEED"):
            parts = data.split()
            if len(parts) >= 2:
                self._spd_updating = True
                if parts[1].upper() == "FAST":
                    self.radio_spd_fast.setChecked(True)
                else:
                    self.radio_spd_slow.setChecked(True)
                self._spd_updating = False

        # ── *EVT:ALARM:SET <ON|OFF> (EXT key toggle) ──
        elif data_upper.startswith("*EVT:ALARM:SET"):
            parts = data.split()
            if len(parts) >= 2:
                state = parts[1].upper()
                self._alarm_enabled = (state == "ON")
                self._set_alarm_indicator(self._alarm_enabled)
                self._update_alarm_ui()

        # ── OK 响应 ──
        elif data_upper.startswith("OK"):
            parts = data.split()
            if len(parts) >= 4:
                # Check for *GET:ALARM multi-slot response (old or compact format)
                if ((len(parts) >= 7 and parts[1].upper() in ("MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN")) or
                    (len(parts) >= 2 and ',' in parts[1] and parts[1].split(',')[0].upper() in ("MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN"))):
                    self._parse_alarm_response(parts, data_upper)
                # Check for single-slot: OK HH MM SS ON/OFF
                elif len(parts) >= 5 and parts[4].upper() in ("ON", "OFF"):
                    try:
                        hs = self.alarm_spins[0][0]
                        hs.setValue(int(parts[1]))
                        self.alarm_spins[0][1].setValue(int(parts[2]))
                        self.alarm_spins[0][2].setValue(int(parts[3]))
                        self.alarm_spins[0][3].setChecked(parts[4].upper() == "ON")
                    except (ValueError, IndexError):
                        pass
                # Check for *GET:MODE or *GET:DISP single-word response
                elif parts[1].upper() in ("DAY", "NIGHT"):
                    self._set_mode_indicator(parts[1].upper())
                elif parts[1].upper() in ("ON", "OFF"):
                    pass
                else:
                    try:
                        vals = [int(p) for p in parts[1:4]]
                        if vals[0] > 1000:
                            # OK YYYY MM DD (日期)
                            self.sp_year.setValue(vals[0])
                            self.sp_month.setValue(vals[1])
                            self.sp_day.setValue(vals[2])
                            self.lbl_mcu_date.setText(
                                f"{vals[0]}/{vals[1]:02d}/{vals[2]:02d}"
                            )
                            self.lbl_time_status.setText("日期已同步 ✅")
                        else:
                            # OK HH MM SS (时间)
                            self.sp_hour.setValue(vals[0])
                            self.sp_min.setValue(vals[1])
                            self.sp_sec.setValue(vals[2])
                            self.lbl_time_status.setText("时间已同步 ✅")
                        QTimer.singleShot(3000,
                            lambda: self.lbl_time_status.setText(""))
                    except (ValueError, IndexError):
                        pass
            self.log("命令成功", "success")

        # ── ERROR 响应 ──
        elif data_upper.startswith("ERROR"):
            self.log(f"错误: {data}", "error")

    def _parse_alarm_response(self, parts, data_upper):
        """Parse multi-slot alarm response.
        Old format: OK MON 06 00 00 ON TUE 06 00 00 ON ...
        New compact: OK MON,6,0,0,1 TUE,6,0,0,1 ..."""
        days_idx = {"MON": 0, "TUE": 1, "WED": 2, "THU": 3, "FRI": 4, "SAT": 5, "SUN": 6}
        # Try compact format first: MON,6,0,0,1
        if len(parts) >= 2 and ',' in parts[1]:
            for slot in parts[1:]:
                fields = slot.split(',')
                if len(fields) < 4: continue
                day = fields[0].upper()
                if day not in days_idx: continue
                idx = days_idx[day]
                try:
                    self.alarm_spins[idx][0].setValue(int(fields[1]))
                    self.alarm_spins[idx][1].setValue(int(fields[2]))
                    self.alarm_spins[idx][2].setValue(int(fields[3]))
                    self.alarm_spins[idx][3].setChecked(bool(int(fields[4])) if len(fields) > 4 else False)
                except (ValueError, IndexError):
                    pass
            self.lbl_alarm_status.setText("已从MCU读取")
            self.lbl_alarm_status.setStyleSheet("color: #95E77E; font-size: 13px;")
            return
        # Fallback: old format OK MON HH MM SS ON TUE HH MM SS ON ...
        i = 1
        while i + 4 < len(parts):
            day = parts[i].upper()
            if day not in days_idx: break
            idx = days_idx[day]
            try:
                self.alarm_spins[idx][0].setValue(int(parts[i + 1]))
                self.alarm_spins[idx][1].setValue(int(parts[i + 2]))
                self.alarm_spins[idx][2].setValue(int(parts[i + 3]))
                self.alarm_spins[idx][3].setChecked(parts[i + 4].upper() == "ON")
            except (ValueError, IndexError):
                pass
            i += 5
        self.lbl_alarm_status.setText("已从MCU读取")
        self.lbl_alarm_status.setStyleSheet("color: #95E77E; font-size: 13px;")

    def _flash_alarm(self, count):
        """闹钟标签红色闪烁"""
        if count <= 0:
            return
        if count % 2 == 0:
            self.lbl_alarm_status.setStyleSheet(
                "color: #FF6B6B; font-size: 14px; font-weight: bold;"
            )
        else:
            self.lbl_alarm_status.setStyleSheet("color: #888; font-size: 14px;")
        QTimer.singleShot(300, lambda: self._flash_alarm(count - 1))

    def _show_weather_on_seg(self):
        """Show weather on PC SEG for 3s, temporarily pause *EVT:DISP mirror"""
        if self._weather_temp == -99:
            return
        t_str = f"{self._weather_temp}C"
        cond_str = self._weather_cond[:3]
        full = (t_str + " " + cond_str).ljust(8, ' ')[:8]
        for i, ch in enumerate(full):
            self.segments[i].set_digit(ch, False)
        # 暂停 *EVT:DISP 更新 3s，避免覆盖天气显示
        self._show_weather_seg = True
        QTimer.singleShot(3000, self._end_weather_seg)

    def _end_weather_seg(self):
        """Restore *EVT:DISP mirroring"""
        self._show_weather_seg = False

    def _blink_alarm_led(self):
        """Blink alarm LED during ringing"""
        self._alarm_blink_on = not self._alarm_blink_on
        self.leds1[1].set_state(self._alarm_blink_on)

    def _update_alarm_ui(self):
        """Update alarm tab UI based on current alarm state"""
        if self._alarm_enabled:
            self.btn_alarm_toggle.setText("禁用闹钟 ✕")
            self.btn_alarm_toggle.setStyleSheet(
                "background-color: #8B0000; color: white; font-weight: bold;"
            )
            self.lbl_alarm_status.setText(
                f"闹钟已启用: {self._alarm_time}"
            )
            self.lbl_alarm_status.setStyleSheet(
                "color: #95E77E; font-size: 14px;"
            )
        else:
            self.btn_alarm_toggle.setText("启用闹钟 ✓")
            self.btn_alarm_toggle.setStyleSheet(
                "background-color: #006400; color: white; font-weight: bold;"
            )
            self.lbl_alarm_status.setText(
                f"闹钟已禁用"
            )
            self.lbl_alarm_status.setStyleSheet(
                "color: #888; font-size: 14px;"
            )

    # ================================================================
    # 日志工具
    # ================================================================
    def log(self, msg, typ="info"):
        timestamp = QDateTime.currentDateTime().toString("hh:mm:ss.zzz")
        colors = {
            "send": "#4ECDC4", "recv": "#95E77E", "error": "#FF6B6B",
            "success": "#95E77E", "info": "#888", "event": "#FFE66D"
        }
        icons = {
            "send": "📤", "recv": "📥", "error": "❌", "success": "✅",
            "info": "ℹ️", "event": "🎯"
        }
        color = colors.get(typ, "#888")
        icon = icons.get(typ, "")
        html = (
            f'<span style="color:#666;">[{timestamp}]</span> '
            f'<span style="color:{color};">{icon} {msg}</span><br>'
        )
        self.log_text.insertHtml(html)
        self.log_text.verticalScrollBar().setValue(
            self.log_text.verticalScrollBar().maximum()
        )

    def export_log(self):
        fn, _ = QFileDialog.getSaveFileName(
            self, "保存日志", "", "文本文件 (*.txt)"
        )
        if fn:
            with open(fn, 'w', encoding='utf-8') as f:
                f.write(self.log_text.toPlainText())
            QMessageBox.information(self, "成功", f"已保存到 {fn}")

    def showEvent(self, event):
        """窗口显示时自动刷新串口"""
        super().showEvent(event)
        if self.auto_refresh_check.isChecked():
            QTimer.singleShot(100, self.refresh_ports)


def main():
    app = QApplication(sys.argv)
    app.setStyle('Fusion')
    window = VirtualTwinPanel()
    window.show()
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
