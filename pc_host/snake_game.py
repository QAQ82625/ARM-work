#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""贪吃蛇小游戏 — MCU 物理按键操控, PC 渲染, MCU 7-SEG 同步分数"""

import random
from PyQt5.QtWidgets import QWidget, QVBoxLayout, QHBoxLayout, QLabel, QPushButton
from PyQt5.QtCore import Qt, QTimer, QRect
from PyQt5.QtGui import QPainter, QColor, QBrush, QPen, QFont

GRID_W = 20   # 棋盘宽度 (格子数)
GRID_H = 15   # 棋盘高度 (格子数)
CELL_SIZE = 28
MARGIN = 4
TICK_MS = 150  # 游戏循环间隔

# 按键盘映射
KEY_DIR = {
    "ADD": 3,     # 左
    "SHIFT": 1,   # 右
    "FUNC": 2,    # 下
    "DISP": 0,    # 上
}


class SnakeGameWidget(QWidget):
    """贪吃蛇游戏控件 — 嵌入 QTabWidget"""

    def __init__(self, send_cmd, parent=None):
        super().__init__(parent)
        self.send_cmd = send_cmd
        self.snake = [(10, 7), (9, 7), (8, 7)]  # 初始蛇 (头在右)
        self.direction = 1      # 0=上 1=右 2=下 3=左
        self.next_dir = 1
        self.food = (15, 7)
        self.score = 0
        self._game_running = False
        self._game_paused = False

        self.timer = QTimer(self)
        self.timer.timeout.connect(self._tick)

        self._init_ui()
        self.setMinimumSize(GRID_W * CELL_SIZE + 200, GRID_H * CELL_SIZE + 80)

    def _init_ui(self):
        layout = QVBoxLayout(self)

        # 分数 + 状态
        top = QHBoxLayout()
        self.lbl_score = QLabel("SCORE: 0")
        self.lbl_score.setStyleSheet("color: #FFF; font-size: 22px; font-weight: bold;")
        top.addWidget(self.lbl_score)
        top.addStretch()
        self.lbl_status = QLabel("")
        self.lbl_status.setStyleSheet("color: #FFE66D; font-size: 18px;")
        top.addWidget(self.lbl_status)
        layout.addLayout(top)

        # 画布
        self.canvas = _GameCanvas(self)
        layout.addWidget(self.canvas, 1)

        # 按钮行
        btn_row = QHBoxLayout()
        self.btn_start = QPushButton("▶ 开始游戏")
        self.btn_start.clicked.connect(self.start_game)
        btn_row.addWidget(self.btn_start)

        self.btn_pause = QPushButton("⏸ 暂停")
        self.btn_pause.clicked.connect(self.pause_game)
        self.btn_pause.setEnabled(False)
        btn_row.addWidget(self.btn_pause)

        self.btn_quit = QPushButton("✕ 退出")
        self.btn_quit.clicked.connect(self.quit_game)
        self.btn_quit.setEnabled(False)
        btn_row.addWidget(self.btn_quit)

        self.btn_start.setStyleSheet(
            "QPushButton{background:#4CAF50;color:#fff;font-weight:bold;"
            "padding:6px 18px;border-radius:4px;}"
            "QPushButton:hover{background:#66BB6A;}")
        self.btn_pause.setStyleSheet(
            "QPushButton{background:#FFA726;color:#fff;font-weight:bold;"
            "padding:6px 18px;border-radius:4px;}")
        self.btn_quit.setStyleSheet(
            "QPushButton{background:#EF5350;color:#fff;font-weight:bold;"
            "padding:6px 18px;border-radius:4px;}")

        layout.addLayout(btn_row)

    def start_game(self):
        self.snake = [(10, 7), (9, 7), (8, 7)]
        self.direction = 1
        self.next_dir = 1
        self.score = 0
        self._game_running = True
        self._game_paused = False
        self._spawn_food()
        self.lbl_score.setText("SCORE: 0")
        self.lbl_status.setText("")
        self.btn_start.setEnabled(False)
        self.btn_pause.setEnabled(True)
        self.btn_pause.setText("⏸ 暂停")
        self.btn_quit.setEnabled(True)
        self.send_cmd("*SET:GAME START")
        self.timer.start(TICK_MS)
        self.canvas.update()

    def pause_game(self):
        if self._game_paused:
            self._game_paused = False
            self.btn_pause.setText("⏸ 暂停")
            self.timer.start(TICK_MS)
            self.lbl_status.setText("")
        else:
            self._game_paused = True
            self.btn_pause.setText("▶ 继续")
            self.timer.stop()
            self.lbl_status.setText("已暂停")

    def quit_game(self):
        self.timer.stop()
        self._game_running = False
        self._game_paused = False
        self.btn_start.setEnabled(True)
        self.btn_pause.setEnabled(False)
        self.btn_quit.setEnabled(False)
        self.lbl_status.setText("已退出")
        self.send_cmd("*SET:GAME QUIT")
        self.canvas.update()

    def on_key(self, key_name):
        """MCU 按键事件路由到方向控制"""
        if not self._game_running or self._game_paused:
            return
        if key_name in KEY_DIR:
            new_dir = KEY_DIR[key_name]
            # 不允许反向 (不能 180° 掉头)
            if (new_dir + 2) % 4 != self.direction:
                self.next_dir = new_dir

    def _tick(self):
        """游戏主循环 — 每 150ms 执行一次"""
        if not self._game_running or self._game_paused:
            return

        self.direction = self.next_dir

        # 计算新头部
        hx, hy = self.snake[0]
        if self.direction == 0:    hy -= 1  # 上
        elif self.direction == 1:  hx += 1  # 右
        elif self.direction == 2:  hy += 1  # 下
        elif self.direction == 3:  hx -= 1  # 左

        # 撞墙检测
        if hx < 0 or hx >= GRID_W or hy < 0 or hy >= GRID_H:
            self._game_over()
            return

        # 撞自己检测
        if (hx, hy) in self.snake:
            self._game_over()
            return

        # 前进
        self.snake.insert(0, (hx, hy))

        # 吃食物
        if (hx, hy) == self.food:
            self.score += 1
            self.lbl_score.setText(f"SCORE: {self.score}")
            self.send_cmd(f"*SET:GAME SCORE {self.score}")
            self._spawn_food()
        else:
            self.snake.pop()

        self.canvas.update()

    def _game_over(self):
        self.timer.stop()
        self._game_running = False
        score = self.score
        self.lbl_status.setText(f"GAME OVER! 得分: {score}")
        self.lbl_status.setStyleSheet(
            "color: #FF5252; font-size: 20px; font-weight: bold;")
        self.send_cmd(f"*SET:GAME OVER {score}")
        self.btn_start.setEnabled(True)
        self.btn_pause.setEnabled(False)
        self.btn_quit.setEnabled(False)
        self.canvas.update()

    def _spawn_food(self):
        while True:
            fx = random.randint(0, GRID_W - 1)
            fy = random.randint(0, GRID_H - 1)
            if (fx, fy) not in self.snake:
                self.food = (fx, fy)
                break


class _GameCanvas(QWidget):
    """贪吃蛇画布 — paintEvent 渲染"""

    def __init__(self, game_widget):
        super().__init__(game_widget)
        self.game = game_widget

    def paintEvent(self, event):
        w = GRID_W * CELL_SIZE + MARGIN * 2
        h = GRID_H * CELL_SIZE + MARGIN * 2
        self.setFixedSize(w, h)

        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)

        # 背景
        p.fillRect(0, 0, w, h, QColor("#1a2a1a"))

        # 网格线
        pen = QPen(QColor("#2a3a2a"), 1)
        p.setPen(pen)
        for x in range(GRID_W + 1):
            px = MARGIN + x * CELL_SIZE
            p.drawLine(px, MARGIN, px, MARGIN + GRID_H * CELL_SIZE)
        for y in range(GRID_H + 1):
            py = MARGIN + y * CELL_SIZE
            p.drawLine(MARGIN, py, MARGIN + GRID_W * CELL_SIZE, py)

        # 食物 — 红色圆点
        fx, fy = self.game.food
        p.setBrush(QColor("#FF5252"))
        p.setPen(Qt.NoPen)
        cx = MARGIN + fx * CELL_SIZE + CELL_SIZE // 2
        cy = MARGIN + fy * CELL_SIZE + CELL_SIZE // 2
        p.drawEllipse(cx - 8, cy - 8, 16, 16)

        # 蛇身
        for idx, (sx, sy) in enumerate(self.game.snake):
            rx = MARGIN + sx * CELL_SIZE + 2
            ry = MARGIN + sy * CELL_SIZE + 2
            rw = CELL_SIZE - 4
            rh = CELL_SIZE - 4
            if idx == 0:
                # 蛇头 — 亮绿色
                p.setBrush(QColor("#8BC34A"))
            else:
                # 蛇身 — 标准绿
                p.setBrush(QColor("#4CAF50"))
            p.drawRoundedRect(rx, ry, rw, rh, 4, 4)

        # GAME OVER 叠加
        if not self.game._game_running and self.game.score > 0:
            p.fillRect(0, h // 2 - 30, w, 60, QColor(0, 0, 0, 180))
            p.setPen(QColor("#FF5252"))
            font = QFont("Arial", 28, QFont.Bold)
            p.setFont(font)
            p.drawText(QRect(0, 0, w, h), Qt.AlignCenter, "GAME OVER")

        p.end()
