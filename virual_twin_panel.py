#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
智能联网时钟系统 - 数字孪生面板（简化可靠版）
"""

import sys
import serial
import serial.tools.list_ports
from PyQt5.QtWidgets import *
from PyQt5.QtCore import *
from PyQt5.QtGui import *

class SimpleSevenSegment(QWidget):
    """简单可靠的7段数码管"""
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setFixedSize(80, 120)
        self.digit = '8'
        self.dp = False
        
    def set_digit(self, digit, dp=False):
        self.digit = str(digit)[0] if digit != '_' else ' '
        self.dp = dp
        self.update()
    
    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing)
        
        w, h = self.width(), self.height()
        
        # 绘制外框
        painter.setPen(QPen(QColor(80, 80, 80), 2))
        painter.setBrush(QColor(20, 20, 20))
        painter.drawRoundedRect(5, 5, w-10, h-10, 8, 8)
        
        # 绘制数字
        painter.setPen(Qt.NoPen)
        
        # 定义7段的位置（简化版）
        thick = 8
        gap = 5
        
        # 每个段的坐标 [x1, y1, x2, y2, 水平?]
        seg_coords = {
            'a': (gap, gap, w-gap, gap+thick, True),      # 上横
            'b': (w-gap-thick, gap, w-gap, h//2-thick//2, False),  # 右上竖
            'c': (w-gap-thick, h//2+thick//2, w-gap, h-gap, False), # 右下竖
            'd': (gap, h-gap-thick, w-gap, h-gap, True),  # 下横
            'e': (gap, h//2+thick//2, gap+thick, h-gap, False),     # 左下竖
            'f': (gap, gap, gap+thick, h//2-thick//2, False),        # 左上竖
            'g': (gap+thick//2, h//2-thick//2, w-gap-thick//2, h//2+thick//2, True)  # 中横
        }
        
        # 数字对应的段（哪些段亮）
        seg_pattern = {
            '0': ['a','b','c','d','e','f'],
            '1': ['b','c'],
            '2': ['a','b','d','e','g'],
            '3': ['a','b','c','d','g'],
            '4': ['b','c','f','g'],
            '5': ['a','c','d','f','g'],
            '6': ['a','c','d','e','f','g'],
            '7': ['a','b','c'],
            '8': ['a','b','c','d','e','f','g'],
            '9': ['a','b','c','d','f','g'],
            'A': ['a','b','c','e','f','g'],
            'b': ['c','d','e','f','g'],
            'C': ['a','d','e','f'],
            'd': ['b','c','d','e','g'],
            'E': ['a','d','e','f','g'],
            'F': ['a','e','f','g'],
            ' ': []
        }
        
        pattern = seg_pattern.get(self.digit.upper(), seg_pattern['8'])
        
        # 绘制每个段
        for seg_name in ['a','b','c','d','e','f','g']:
            if seg_name in pattern:
                painter.setBrush(QColor(255, 69, 0))  # 亮起的段
            else:
                painter.setBrush(QColor(40, 40, 40))  # 熄灭的段
            
            x1, y1, x2, y2, is_horiz = seg_coords[seg_name]
            if is_horiz:
                painter.drawRect(x1, y1, x2-x1, thick)
            else:
                painter.drawRect(x1, y1, thick, y2-y1)
        
        # 绘制小数点
        if self.dp:
            painter.setBrush(QColor(255, 69, 0))
            painter.drawEllipse(w-15, h-15, 8, 8)

class SimpleLED(QWidget):
    """简单可靠的LED指示灯"""
    def __init__(self, label="LED", parent=None):
        super().__init__(parent)
        self.setFixedSize(50, 60)
        self.label = label
        self.on = False
        
    def set_state(self, on):
        self.on = on
        self.update()
    
    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing)
        
        # 绘制外框
        painter.setPen(QPen(QColor(100, 100, 100), 2))
        painter.setBrush(QColor(30, 30, 30))
        painter.drawRoundedRect(5, 5, 40, 40, 5, 5)
        
        # 绘制LED
        if self.on:
            painter.setBrush(QColor(0, 255, 0))
            # 发光效果
            painter.setPen(QPen(QColor(0, 255, 0, 100), 2))
        else:
            painter.setBrush(QColor(20, 20, 20))
            painter.setPen(Qt.NoPen)
        
        painter.drawEllipse(8, 8, 34, 34)
        
        # 绘制标签
        painter.setPen(QPen(QColor(200, 200, 200), 1))
        painter.setFont(QFont("Arial", 9))
        painter.drawText(self.rect(), Qt.AlignBottom | Qt.AlignHCenter, self.label)

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
                    line = self.serial.readline().decode().strip()
                    if line:
                        self.data_received.emit(line)
            except:
                pass
            self.msleep(10)

class VirtualTwinPanel(QMainWindow):
    def __init__(self):
        super().__init__()
        self.serial_thread = SerialThread()
        self.init_ui()
        
    def init_ui(self):
        self.setWindowTitle("智能联网时钟 - 数字孪生面板")
        self.setMinimumSize(900, 750)
        
        # 设置全局样式
        self.setStyleSheet("""
            QMainWindow { background-color: #2b2b2b; }
            QGroupBox { 
                color: white; 
                font-weight: bold; 
                border: 2px solid #555; 
                border-radius: 8px; 
                margin-top: 10px;
                padding-top: 15px;
                background-color: #3c3c3c;
            }
            QGroupBox::title { 
                subcontrol-origin: margin; 
                left: 10px; 
                padding: 0 10px; 
            }
            QPushButton { 
                background-color: #4a4a4a; 
                border: 1px solid #666; 
                border-radius: 5px; 
                padding: 8px; 
                color: white;
            }
            QPushButton:hover { background-color: #5a5a5a; border-color: #0078d7; }
            QLabel { color: white; }
            QCheckBox { color: white; }
        """)
        
        central = QWidget()
        self.setCentralWidget(central)
        layout = QVBoxLayout(central)
        layout.setSpacing(15)
        
        # 1. 串口控制
        serial_box = self.create_serial_box()
        layout.addWidget(serial_box)
        
        # 状态栏
        self.status_bar = QStatusBar()
        self.setStatusBar(self.status_bar)
        self.status_bar.showMessage("未连接")
        
        # 2. 数码管区域
        seg_box = self.create_segment_box()
        layout.addWidget(seg_box)
        
        # 3. LED区域
        led_box = self.create_led_box()
        layout.addWidget(led_box)
        
        # 4. 按键区域
        key_box = self.create_key_box()
        layout.addWidget(key_box)
        
        # 5. 命令区域
        cmd_box = self.create_command_box()
        layout.addWidget(cmd_box)
        
        # 6. 日志区域
        log_box = self.create_log_box()
        layout.addWidget(log_box)
        
        # 初始化显示
        self.init_display()
        
        # 信号连接
        self.serial_thread.data_received.connect(self.handle_data)
    
    def create_serial_box(self):
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
        
        layout.addStretch()
        
        test_btn = QPushButton("测试 *PING")
        test_btn.clicked.connect(lambda: self.send_cmd("*PING"))
        layout.addWidget(test_btn)
        
        mock_btn = QPushButton("模拟数据")
        mock_btn.clicked.connect(self.send_mock)
        layout.addWidget(mock_btn)
        
        group.setLayout(layout)
        return group
    
    def create_segment_box(self):
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
    
    def create_led_box(self):
        group = QGroupBox("LED状态指示灯")
        layout = QVBoxLayout()
        
        # 第一排
        row1 = QHBoxLayout()
        row1.setAlignment(Qt.AlignCenter)
        led1_labels = ['❤️ 心跳', '⏰ 闹钟', '✏️ 编辑', '📤 TX']
        self.leds1 = []
        for label in led1_labels:
            led = SimpleLED(label)
            row1.addWidget(led)
            self.leds1.append(led)
        row1.addStretch()
        
        # 第二排
        row2 = QHBoxLayout()
        row2.setAlignment(Qt.AlignCenter)
        led2_labels = ['📥 RX', '🔴 状态1', '🟡 状态2', '🔵 状态3']
        self.leds2 = []
        for label in led2_labels:
            led = SimpleLED(label)
            row2.addWidget(led)
            self.leds2.append(led)
        row2.addStretch()
        
        # 控制按钮
        ctrl = QHBoxLayout()
        ctrl.setAlignment(Qt.AlignCenter)
        
        btn1 = QPushButton("切换状态1")
        btn1.clicked.connect(lambda: self.toggle_led(1))  # 状态1
        ctrl.addWidget(btn1)
        
        btn2 = QPushButton("切换状态2")
        btn2.clicked.connect(lambda: self.toggle_led(2))  # 状态2
        ctrl.addWidget(btn2)
        
        btn3 = QPushButton("LED全亮")
        btn3.clicked.connect(self.all_leds_on)
        ctrl.addWidget(btn3)
        
        btn4 = QPushButton("LED全灭")
        btn4.clicked.connect(self.all_leds_off)
        ctrl.addWidget(btn4)
        
        layout.addLayout(row1)
        layout.addLayout(row2)
        layout.addLayout(ctrl)
        
        group.setLayout(layout)
        return group
    
    def create_key_box(self):
        group = QGroupBox("虚拟按键（点击发送串口命令）")
        layout = QGridLayout()
        layout.setSpacing(10)
        
        keys = [
            ('FUNC', 0, 0), ('SHIFT', 0, 1), ('ADD', 0, 2), ('SAVE', 0, 3),
            ('DISP', 1, 0), ('SPEED', 1, 1), ('FORMAT', 1, 2), ('EXT', 1, 3),
            ('USER1', 2, 0), ('USER2', 2, 1)
        ]
        
        self.key_btns = {}
        for name, row, col in keys:
            btn = QPushButton(name)
            btn.setMinimumWidth(100)
            btn.setMinimumHeight(40)
            btn.clicked.connect(lambda checked, n=name: self.key_click(n))
            layout.addWidget(btn, row, col)
            self.key_btns[name] = btn
        
        group.setLayout(layout)
        return group
    
    def create_command_box(self):
        group = QGroupBox("自定义命令")
        layout = QHBoxLayout()
        
        layout.addWidget(QLabel("命令:"))
        self.cmd_input = QLineEdit()
        self.cmd_input.setPlaceholderText("例如: *SET:LED FF")
        self.cmd_input.returnPressed.connect(self.send_custom)
        layout.addWidget(self.cmd_input)
        
        send_btn = QPushButton("发送")
        send_btn.clicked.connect(self.send_custom)
        layout.addWidget(send_btn)
        
        quick1 = QPushButton("*GET:TIME")
        quick1.clicked.connect(lambda: self.send_cmd("*GET:TIME"))
        layout.addWidget(quick1)
        
        quick2 = QPushButton("*SET:DISP OFF")
        quick2.clicked.connect(lambda: self.send_cmd("*SET:DISPLAY OFF"))
        layout.addWidget(quick2)
        
        quick3 = QPushButton("*SET:DISP ON")
        quick3.clicked.connect(lambda: self.send_cmd("*SET:DISPLAY ON"))
        layout.addWidget(quick3)
        
        group.setLayout(layout)
        return group
    
    def create_log_box(self):
        group = QGroupBox("通信日志")
        layout = QVBoxLayout()
        
        self.log_text = QTextEdit()
        self.log_text.setMaximumHeight(150)
        self.log_text.setReadOnly(True)
        self.log_text.setFont(QFont("Monaco", 10))
        layout.addWidget(self.log_text)
        
        btn_layout = QHBoxLayout()
        clear_btn = QPushButton("清空")
        clear_btn.clicked.connect(lambda: self.log_text.clear())
        btn_layout.addWidget(clear_btn)
        
        export_btn = QPushButton("导出")
        export_btn.clicked.connect(self.export_log)
        btn_layout.addWidget(export_btn)
        
        btn_layout.addStretch()
        
        self.show_send = QCheckBox("显示发送")
        self.show_send.setChecked(True)
        self.show_recv = QCheckBox("显示接收")
        self.show_recv.setChecked(True)
        btn_layout.addWidget(self.show_send)
        btn_layout.addWidget(self.show_recv)
        
        layout.addLayout(btn_layout)
        
        group.setLayout(layout)
        return group
    
    def init_display(self):
        """初始化显示1-8"""
        for i in range(8):
            self.segments[i].set_digit(str(i + 1), False)
        self.log("初始化: 显示 1-8", "info")
    
    def toggle_led(self, num):
        """独立控制LED（不经过串口）"""
        # 状态1 = leds2[1], 状态2 = leds2[2]
        idx = 1 if num == 1 else 2
        led = self.leds2[idx]
        new_state = not led.on
        led.set_state(new_state)
        self.log(f"本地: {led.label} {'亮' if new_state else '灭'}", "event")
    
    def all_leds_on(self):
        for led in self.leds1 + self.leds2:
            led.set_state(True)
        self.log("本地: 所有LED全亮", "event")
    
    def all_leds_off(self):
        for led in self.leds1 + self.leds2:
            led.set_state(False)
        self.log("本地: 所有LED全灭", "event")
    
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
            self.status_bar.showMessage("未连接")
            self.log("串口已断开", "info")
        else:
            text = self.com_combo.currentText()
            if text and "无可用" not in text:
                port = text.split(' - ')[0]
                ok, msg = self.serial_thread.connect(port)
                if ok:
                    self.connect_btn.setText("断开")
                    self.status_bar.showMessage(f"已连接 {port}")
                    self.log(f"连接成功: {port}", "success")
                else:
                    self.log(f"连接失败: {msg}", "error")
                    QMessageBox.critical(self, "错误", msg)
            else:
                QMessageBox.warning(self, "警告", "请选择串口")
    
    def send_cmd(self, cmd):
        if self.serial_thread.send_command(cmd):
            if self.show_send.isChecked():
                self.log(f"发送: {cmd}", "send")
            # TX LED闪烁
            self.leds1[3].set_state(True)
            QTimer.singleShot(100, lambda: self.leds1[3].set_state(False))
            return True
        else:
            self.log(f"发送失败: {cmd}", "error")
            return False
    
    def send_custom(self):
        cmd = self.cmd_input.text().strip()
        if cmd:
            self.send_cmd(cmd)
            self.cmd_input.clear()
    
    def key_click(self, name):
        cmd = f"*SET:KEY {name}"
        self.send_cmd(cmd)
        self.log(f"按键: {name}", "event")
        # 按钮反馈
        btn = self.key_btns[name]
        btn.setStyleSheet("background-color: #0078d7;")
        QTimer.singleShot(100, lambda: btn.setStyleSheet(""))
    
    def send_mock(self):
        """模拟数据"""
        from datetime import datetime
        now = datetime.now()
        # 显示当前时间 HHMMSS
        time_str = now.strftime("%H%M%S")
        for i, ch in enumerate(time_str):
            if i < 8:
                self.segments[i].set_digit(ch, False)
        self.log(f"模拟: 显示当前时间 {time_str}", "info")
    
    def handle_data(self, data):
        if self.show_recv.isChecked():
            self.log(f"接收: {data}", "recv")
        
        # RX LED闪烁
        self.leds2[0].set_state(True)
        QTimer.singleShot(100, lambda: self.leds2[0].set_state(False))
        
        # 解析协议
        if data.startswith("*EVT:DISP"):
            parts = data.split()
            if len(parts) >= 2:
                disp = parts[1][:8].ljust(8, ' ')
                dp = parts[2] if len(parts) >= 3 else "00"
                try:
                    dp_val = int(dp, 16)
                except:
                    dp_val = 0
                for i in range(8):
                    dp_on = bool(dp_val & (1 << (7 - i)))
                    self.segments[i].set_digit(disp[i] if disp[i] != '_' else ' ', dp_on)
        
        elif data.startswith("*EVT:LED"):
            parts = data.split()
            if len(parts) >= 2:
                try:
                    val = int(parts[1], 16)
                except:
                    val = 0
                for i in range(4):
                    self.leds1[i].set_state(bool(val & (1 << i)))
                for i in range(4):
                    self.leds2[i].set_state(bool(val & (1 << (i + 4))))
        
        elif data.startswith("*PONG"):
            self.log("心跳响应", "info")
            self.status_bar.showMessage("心跳", 1000)
            self.leds1[0].set_state(True)
            QTimer.singleShot(100, lambda: self.leds1[0].set_state(False))
        
        elif data.startswith("OK"):
            self.log("命令成功", "success")
        
        elif data.startswith("ERROR"):
            self.log(f"错误: {data}", "error")
    
    def log(self, msg, typ="info"):
        timestamp = QDateTime.currentDateTime().toString("hh:mm:ss")
        colors = {"send": "#4ECDC4", "recv": "#95E77E", "error": "#FF6B6B", 
                  "success": "#95E77E", "info": "#888", "event": "#FFE66D"}
        icons = {"send": "📤", "recv": "📥", "error": "❌", "success": "✅", 
                 "info": "ℹ️", "event": "🎯"}
        html = f'<span style="color:#888;">[{timestamp}]</span> <span style="color:{colors.get(typ,"#888")};">{icons.get(typ,"")} {msg}</span><br>'
        self.log_text.insertHtml(html)
        self.log_text.verticalScrollBar().setValue(self.log_text.verticalScrollBar().maximum())
    
    def export_log(self):
        fn, _ = QFileDialog.getSaveFileName(self, "保存日志", "", "文本文件 (*.txt)")
        if fn:
            with open(fn, 'w', encoding='utf-8') as f:
                f.write(self.log_text.toPlainText())
            QMessageBox.information(self, "成功", f"已保存")

def main():
    app = QApplication(sys.argv)
    app.setStyle('Fusion')
    window = VirtualTwinPanel()
    window.show()
    sys.exit(app.exec_())

if __name__ == "__main__":
    main()
