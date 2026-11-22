"""
STM32 Ethernet Monitor  --  Nucleo-F767ZI + lwIP

Monitors ICMP ping in both directions:

  * Board -> PC : the firmware pings the PC and reports over UART, one line
                  per event:   "PING seq=12 rtt=1 ms"   /   "PING seq=12 timeout"
  * PC -> Board : this app runs the system `ping` against the board's IP.

The board's ping is started/stopped with the UART commands '1' / '0'
(the firmware calls ping_start() / ping_stop() and switches the LED).

Requires:  pip install PySide6 pyserial
"""

import platform
import re
import sys
import time
from collections import deque
from datetime import datetime

import serial
import serial.tools.list_ports

from PySide6.QtCore import QPointF, QProcess, Qt, QTimer
from PySide6.QtGui import QColor, QFont, QPainter, QPen, QPixmap
from PySide6.QtWidgets import (
    QApplication, QComboBox, QGridLayout, QGroupBox, QHBoxLayout, QLabel,
    QLineEdit, QMainWindow, QPushButton, QTabWidget, QTextEdit,
    QVBoxLayout, QWidget,
)

# --------------------------------------------------------------------------
# Settings
# --------------------------------------------------------------------------
DEFAULT_BOARD_IP = "192.168.1.90"   # IP of the Nucleo (the PC is .100)
BAUDRATE = 115200
HISTORY = 120            # samples shown in each plot
PING_PERIOD_MS = 1000    # PC -> board ping period
RX_FLUSH_S = 0.15        # a UART line without "\n" is flushed after this time

COLOR_BOARD = "#34D399"
COLOR_PC = "#60A5FA"
COLOR_BAD = "#F87171"
COLOR_WARN = "#FBBF24"
COLOR_MUTED = "#929BAA"


# --------------------------------------------------------------------------
# Parsing helpers (no Qt, easy to test)
# --------------------------------------------------------------------------
_BOARD_TIMEOUT_RE = re.compile(r"PING\s+seq\s*=\s*(\d+)\s+timeout", re.I)
# "PING seq=12 rtt=1 ms"  and also the older "rtt = 1" (no seq)
_BOARD_RTT_RE = re.compile(r"(?:PING\s+seq\s*=\s*(\d+)\s+)?rtt\s*=\s*(\d+)", re.I)
_LED_RE = re.compile(r"LED\w*\s+(?:is\s+)?(ON|OFF)\b", re.I)
# "time=1ms", "time<1ms", "durata=1ms" (Italian Windows), "time=0.345 ms" (Linux)
_PC_RTT_RE = re.compile(r"([=<])\s*(\d+(?:[.,]\d+)?)\s*ms", re.I)


def parse_board_line(line):
    """Classify one UART line from the board.

    Returns ("timeout", seq) | ("reply", seq_or_None, rtt_ms) | ("led", "ON"/"OFF")
    or None when the line is not interesting.
    """
    m = _BOARD_TIMEOUT_RE.search(line)
    if m:
        return ("timeout", int(m.group(1)))
    m = _BOARD_RTT_RE.search(line)
    if m:
        seq = int(m.group(1)) if m.group(1) else None
        return ("reply", seq, float(m.group(2)))
    m = _LED_RE.search(line)
    if m:
        return ("led", m.group(1).upper())
    return None


def parse_ping_output(text):
    """RTT in ms from the output of the system `ping`, or None if no reply.

    Only lines that contain a TTL are replies: "Destination host unreachable"
    has none, and the statistics lines ("Minimum = 1ms") are skipped for the
    same reason. "<1ms" is counted as 0.5 ms.
    """
    for line in text.splitlines():
        if "ttl" not in line.lower():
            continue
        m = _PC_RTT_RE.search(line)
        if m:
            if m.group(1) == "<":
                return 0.5
            return float(m.group(2).replace(",", "."))
    return None


class PingStats:
    """Counters and history for one direction. RTT in ms, None = timeout."""

    def __init__(self, history=HISTORY):
        self.history = deque(maxlen=history)
        self.reset()

    def reset(self):
        self.received = 0
        self.lost = 0
        self.total_rtt = 0.0
        self.last = None
        self.min_rtt = None
        self.max_rtt = None
        self.history.clear()

    def add_reply(self, rtt):
        self.received += 1
        self.total_rtt += rtt
        self.last = rtt
        self.min_rtt = rtt if self.min_rtt is None else min(self.min_rtt, rtt)
        self.max_rtt = rtt if self.max_rtt is None else max(self.max_rtt, rtt)
        self.history.append(rtt)

    def add_timeout(self):
        self.lost += 1
        self.last = None
        self.history.append(None)

    @property
    def sent(self):
        return self.received + self.lost

    @property
    def loss_percent(self):
        return 100.0 * self.lost / self.sent if self.sent else 0.0

    @property
    def avg_rtt(self):
        return self.total_rtt / self.received if self.received else None


# --------------------------------------------------------------------------
# Widgets
# --------------------------------------------------------------------------
class RttPlot(QWidget):
    """Small live plot of the last HISTORY RTT samples (red bars = timeouts)."""

    def __init__(self, stats, color):
        super().__init__()
        self.stats = stats
        self.color = QColor(color)
        self.setMinimumHeight(150)

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        w, h = self.width(), self.height()
        left, right, top, bottom = 46, 12, 12, 24
        pw, ph = w - left - right, h - top - bottom
        painter.fillRect(0, 0, w, h, QColor("#10141C"))
        if pw <= 0 or ph <= 0:
            painter.end()
            return

        data = list(self.stats.history)
        values = [v for v in data if v is not None]
        y_max = max(2.0, (max(values) if values else 0.0) * 1.25)

        painter.setFont(QFont("Segoe UI", 8))
        fm = painter.fontMetrics()

        # grid + y labels
        for i in range(5):
            y = top + ph * i / 4
            painter.setPen(QPen(QColor("#252D3A"), 1))
            painter.drawLine(QPointF(left, y), QPointF(w - right, y))
            value = y_max * (1 - i / 4)
            label = f"{value:.1f}" if y_max < 10 else f"{value:.0f}"
            painter.setPen(QColor(COLOR_MUTED))
            painter.drawText(
                QPointF(left - 6 - fm.horizontalAdvance(label), y + fm.ascent() / 2 - 1),
                label,
            )
        painter.setPen(QColor(COLOR_MUTED))
        painter.drawText(QPointF(left, h - 6), f"RTT in ms  -  last {HISTORY} samples")

        n = len(data)
        if n == 0:
            painter.end()
            return

        def x_of(i):
            # newest sample sits at the right edge
            return left + pw * (HISTORY - n + i) / max(HISTORY - 1, 1)

        def y_of(v):
            return top + ph * (1 - v / y_max)

        # timeouts
        bad = QColor(COLOR_BAD)
        bad.setAlpha(150)
        painter.setPen(QPen(bad, 2))
        for i, v in enumerate(data):
            if v is None:
                x = x_of(i)
                painter.drawLine(QPointF(x, top), QPointF(x, top + ph))

        # RTT line (broken at timeouts)
        painter.setPen(QPen(self.color, 2))
        painter.setBrush(self.color)
        prev = None
        for i, v in enumerate(data):
            if v is None:
                prev = None
                continue
            point = QPointF(x_of(i), y_of(v))
            if prev is not None:
                painter.drawLine(prev, point)
            painter.drawEllipse(point, 2.5, 2.5)
            prev = point
        painter.end()


class PingPanel(QGroupBox):
    """Big RTT readout + statistics + plot for one direction."""

    FIELDS = [
        ("sent", "SENT"), ("received", "RECEIVED"), ("loss", "LOSS"),
        ("min", "MIN"), ("avg", "AVG"), ("max", "MAX"),
    ]

    def __init__(self, title, color):
        super().__init__(title)
        self.color = color
        self.stats = PingStats()

        layout = QVBoxLayout(self)
        layout.setSpacing(10)

        self.big = QLabel()
        self.big.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.state = QLabel()
        self.state.setAlignment(Qt.AlignmentFlag.AlignCenter)

        grid = QGridLayout()
        self.values = {}
        for i, (key, caption) in enumerate(self.FIELDS):
            cap = QLabel(caption)
            cap.setStyleSheet(f"color: {COLOR_MUTED}; font-size: 8pt;")
            val = QLabel("-")
            val.setStyleSheet("font-size: 13pt; font-weight: bold;")
            grid.addWidget(cap, (i // 3) * 2, i % 3)
            grid.addWidget(val, (i // 3) * 2 + 1, i % 3)
            self.values[key] = val

        self.plot = RttPlot(self.stats, color)

        layout.addWidget(self.big)
        layout.addWidget(self.state)
        layout.addLayout(grid)
        layout.addWidget(self.plot, 1)

        self.reset()

    def set_big(self, text, color):
        self.big.setText(text)
        self.big.setStyleSheet(f"font-size: 30pt; font-weight: bold; color: {color};")

    def add_reply(self, rtt, seq=None):
        self.stats.add_reply(rtt)
        self.set_big(f"{rtt:.1f} ms", self.color)
        tag = f"seq {seq}" if seq is not None else "reply"
        self.state.setText(f"{tag}  -  {datetime.now():%H:%M:%S}")
        self.state.setStyleSheet(f"color: {COLOR_MUTED};")
        self.refresh()

    def add_timeout(self, seq=None):
        self.stats.add_timeout()
        self.set_big("TIMEOUT", COLOR_BAD)
        tag = f"seq {seq}" if seq is not None else "no reply"
        self.state.setText(f"{tag}  -  {datetime.now():%H:%M:%S}")
        self.state.setStyleSheet(f"color: {COLOR_BAD};")
        self.refresh()

    def reset(self):
        self.stats.reset()
        self.set_big("- ms", "#737B89")
        self.state.setText("waiting for data")
        self.state.setStyleSheet(f"color: {COLOR_MUTED};")
        self.refresh()

    def refresh(self):
        s = self.stats

        def ms(v):
            return "-" if v is None else f"{v:.1f} ms"

        self.values["sent"].setText(str(s.sent))
        self.values["received"].setText(str(s.received))
        self.values["loss"].setText(f"{s.loss_percent:.0f} %")
        self.values["loss"].setStyleSheet(
            "font-size: 13pt; font-weight: bold;"
            + (f" color: {COLOR_BAD};" if s.lost else "")
        )
        self.values["min"].setText(ms(s.min_rtt))
        self.values["avg"].setText(ms(s.avg_rtt))
        self.values["max"].setText(ms(s.max_rtt))
        self.plot.update()


# --------------------------------------------------------------------------
# Main window
# --------------------------------------------------------------------------
class EthernetMonitorApp(QMainWindow):

    def __init__(self):
        super().__init__()

        self.setWindowTitle("STM32 Ethernet Monitor")
        self.resize(1100, 900)

        self.serial_port = None
        self.rx_buffer = ""
        self.last_rx_time = 0.0

        self.pc_process = None
        self.pc_running = False

        self.setup_ui()
        self.refresh_ports()
        self.set_connected(False)

        # Poll incoming UART data
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.read_serial)
        self.timer.start(30)

        # PC -> board ping, once per second
        self.pc_timer = QTimer(self)
        self.pc_timer.timeout.connect(self.run_pc_ping)

    # ---------------------------------------------------------------- UI
    def setup_ui(self):

        self.setStyleSheet("""
            QMainWindow { background-color: #151922; }
            QWidget {
                color: #E8ECF2;
                font-family: Segoe UI;
                font-size: 10pt;
            }
            QGroupBox {
                border: 1px solid #343B49;
                border-radius: 10px;
                margin-top: 12px;
                padding: 15px;
                font-weight: bold;
            }
            QGroupBox::title {
                subcontrol-origin: margin;
                left: 12px;
                padding: 0 5px;
            }
            QPushButton {
                background-color: #30394A;
                border: none;
                border-radius: 7px;
                padding: 9px 14px;
                font-weight: bold;
            }
            QPushButton:hover { background-color: #43516A; }
            QPushButton:disabled {
                background-color: #252B36;
                color: #737B89;
            }
            QComboBox, QLineEdit {
                background-color: #252D3A;
                padding: 8px;
                border-radius: 6px;
            }
            QTextEdit {
                background-color: #10141C;
                border: 1px solid #343B49;
                border-radius: 7px;
                font-family: Consolas;
            }
            QTabWidget::pane {
                border: 1px solid #343B49;
                border-radius: 7px;
            }
            QTabBar::tab {
                background-color: #252B36;
                padding: 8px 18px;
                margin-right: 2px;
                border-top-left-radius: 6px;
                border-top-right-radius: 6px;
            }
            QTabBar::tab:selected { background-color: #30394A; }
        """)

        central = QWidget()
        self.setCentralWidget(central)

        main_layout = QVBoxLayout(central)
        main_layout.setSpacing(14)
        main_layout.setContentsMargins(25, 20, 25, 20)

        # Header
        title = QLabel("STM32 Ethernet Monitor")
        title.setStyleSheet("font-size: 23pt; font-weight: bold;")
        subtitle = QLabel("Nucleo-F767ZI  |  lwIP  |  ICMP ping in both directions")
        subtitle.setStyleSheet(f"color: {COLOR_MUTED};")
        main_layout.addWidget(title)
        main_layout.addWidget(subtitle)

        # Connection
        connection_box = QGroupBox("DEVICE CONNECTION (UART)")
        connection_layout = QHBoxLayout(connection_box)

        self.port_combo = QComboBox()
        self.refresh_button = QPushButton("Refresh")
        self.refresh_button.clicked.connect(self.refresh_ports)
        self.connect_button = QPushButton("Connect")
        self.connect_button.clicked.connect(self.connect_serial)
        self.disconnect_button = QPushButton("Disconnect")
        self.disconnect_button.clicked.connect(self.disconnect_serial)
        self.connection_status = QLabel("● Disconnected")
        self.connection_status.setStyleSheet(f"color: {COLOR_BAD};")

        connection_layout.addWidget(QLabel("Serial Port:"))
        connection_layout.addWidget(self.port_combo, 1)
        connection_layout.addWidget(self.refresh_button)
        connection_layout.addWidget(self.connect_button)
        connection_layout.addWidget(self.disconnect_button)
        connection_layout.addWidget(self.connection_status)
        main_layout.addWidget(connection_box)

        # Ping control
        control_box = QGroupBox("PING CONTROL")
        control = QGridLayout(control_box)

        self.start_board_button = QPushButton("▶  Start board ping")
        self.start_board_button.setToolTip("Sends '1': LED on, firmware starts pinging the PC")
        self.start_board_button.clicked.connect(lambda: self.send_command("1"))
        self.stop_board_button = QPushButton("■  Stop board ping")
        self.stop_board_button.setToolTip("Sends '0': LED off, firmware stops pinging")
        self.stop_board_button.clicked.connect(lambda: self.send_command("0"))
        self.led_indicator = QLabel("● LED: UNKNOWN")
        self.led_indicator.setStyleSheet(f"color: {COLOR_WARN}; font-weight: bold;")

        board_label = QLabel("Board → PC")
        board_label.setStyleSheet(f"color: {COLOR_BOARD}; font-weight: bold;")
        control.addWidget(board_label, 0, 0)
        control.addWidget(self.start_board_button, 0, 1)
        control.addWidget(self.stop_board_button, 0, 2)
        control.addWidget(self.led_indicator, 0, 3)

        self.ip_edit = QLineEdit(DEFAULT_BOARD_IP)
        self.ip_edit.setPlaceholderText("board IP")
        self.start_pc_button = QPushButton("▶  Start PC ping")
        self.start_pc_button.clicked.connect(self.start_pc_ping)
        self.stop_pc_button = QPushButton("■  Stop PC ping")
        self.stop_pc_button.clicked.connect(self.stop_pc_ping)
        self.stop_pc_button.setEnabled(False)
        self.reset_button = QPushButton("Reset statistics")
        self.reset_button.clicked.connect(self.reset_stats)

        pc_label = QLabel("PC → Board")
        pc_label.setStyleSheet(f"color: {COLOR_PC}; font-weight: bold;")
        control.addWidget(pc_label, 1, 0)
        control.addWidget(self.ip_edit, 1, 1)
        control.addWidget(self.start_pc_button, 1, 2)
        control.addWidget(self.stop_pc_button, 1, 3)
        control.addWidget(self.reset_button, 1, 4)
        control.setColumnStretch(4, 1)
        main_layout.addWidget(control_box)

        # The two panels
        panels = QHBoxLayout()
        self.board_panel = PingPanel("BOARD → PC   (measured by the firmware)", COLOR_BOARD)
        self.pc_panel = PingPanel("PC → BOARD   (measured by the PC)", COLOR_PC)
        panels.addWidget(self.board_panel)
        panels.addWidget(self.pc_panel)
        main_layout.addLayout(panels, 3)

        # Tabs: UART log + (future) image viewer
        self.tabs = QTabWidget()

        log_tab = QWidget()
        log_layout = QVBoxLayout(log_tab)
        self.log = QTextEdit()
        self.log.setReadOnly(True)
        self.log.document().setMaximumBlockCount(2000)
        clear_button = QPushButton("Clear Log")
        clear_button.clicked.connect(self.log.clear)
        log_layout.addWidget(self.log)
        log_layout.addWidget(clear_button)
        self.tabs.addTab(log_tab, "UART Log")

        self.image_label = QLabel("Image / telemetry viewer\n\nNothing received yet.")
        self.image_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.image_label.setStyleSheet(f"color: {COLOR_MUTED};")
        self.tabs.addTab(self.image_label, "Image Viewer")

        main_layout.addWidget(self.tabs, 2)

    def set_connected(self, connected):
        self.connect_button.setEnabled(not connected)
        self.disconnect_button.setEnabled(connected)
        self.refresh_button.setEnabled(not connected)
        self.port_combo.setEnabled(not connected)
        self.start_board_button.setEnabled(connected)
        self.stop_board_button.setEnabled(connected)

    # ------------------------------------------------------------ logging
    def log_line(self, text):
        self.log.append(f"{datetime.now():%H:%M:%S}  {text}")

    # ------------------------------------------------------------- serial
    def refresh_ports(self):
        self.port_combo.clear()
        for port in serial.tools.list_ports.comports():
            self.port_combo.addItem(f"{port.device} - {port.description}", port.device)

    def connect_serial(self):
        port = self.port_combo.currentData()
        if not port:
            self.log_line("No serial port selected.")
            return

        try:
            self.serial_port = serial.Serial(
                port=port, baudrate=BAUDRATE, timeout=0, write_timeout=0
            )
        except serial.SerialException as error:
            self.log_line(f"Connection error: {error}")
            return

        self.rx_buffer = ""
        self.connection_status.setText(f"● Connected to {port}")
        self.connection_status.setStyleSheet("color: #34D399;")
        self.set_connected(True)
        self.log_line(f"Connected to {port}")

    def disconnect_serial(self):
        was_open = bool(self.serial_port and self.serial_port.is_open)
        if was_open:
            self.serial_port.close()
        self.serial_port = None
        self.rx_buffer = ""

        self.connection_status.setText("● Disconnected")
        self.connection_status.setStyleSheet(f"color: {COLOR_BAD};")
        self.set_connected(False)
        if was_open:
            self.log_line("Disconnected.")

    def send_command(self, command):
        if not (self.serial_port and self.serial_port.is_open):
            return
        try:
            self.serial_port.write(command.encode("ascii"))
            self.log_line(f"TX  →  {command}")
        except serial.SerialException as error:
            self.log_line(f"TX error: {error}")

    def read_serial(self):
        if not (self.serial_port and self.serial_port.is_open):
            return

        try:
            data = self.serial_port.read(self.serial_port.in_waiting)
        except serial.SerialException as error:
            self.log_line(f"RX error: {error}")
            return

        now = time.monotonic()

        if data:
            self.rx_buffer += data.decode("ascii", errors="replace")
            self.last_rx_time = now
            # complete lines
            while "\n" in self.rx_buffer:
                line, self.rx_buffer = self.rx_buffer.split("\n", 1)
                line = line.strip()
                if line:
                    self.handle_line(line)
        elif self.rx_buffer and now - self.last_rx_time > RX_FLUSH_S:
            # firmware message without a trailing newline: treat as a line
            line = self.rx_buffer.strip()
            self.rx_buffer = ""
            if line:
                self.handle_line(line)

    def handle_line(self, line):
        self.log_line(f"RX  ←  {line}")

        event = parse_board_line(line)
        if event is None:
            return

        if event[0] == "reply":
            _, seq, rtt = event
            self.board_panel.add_reply(rtt, seq)
        elif event[0] == "timeout":
            self.board_panel.add_timeout(event[1])
        elif event[0] == "led":
            self.update_led(event[1])

    def update_led(self, state):
        if state == "ON":
            self.led_indicator.setText("● LED: ON")
            self.led_indicator.setStyleSheet("color: #34D399; font-weight: bold;")
        else:
            self.led_indicator.setText("● LED: OFF")
            self.led_indicator.setStyleSheet(f"color: {COLOR_MUTED}; font-weight: bold;")

    # ----------------------------------------------------- PC -> board ping
    def start_pc_ping(self):
        if not self.ip_edit.text().strip():
            self.log_line("Enter the board IP first.")
            return
        self.pc_running = True
        self.ip_edit.setEnabled(False)
        self.start_pc_button.setEnabled(False)
        self.stop_pc_button.setEnabled(True)
        self.log_line(f"PC ping started → {self.ip_edit.text().strip()}")
        self.run_pc_ping()
        self.pc_timer.start(PING_PERIOD_MS)

    def stop_pc_ping(self):
        was_running = self.pc_running
        self.pc_running = False
        self.pc_timer.stop()
        process, self.pc_process = self.pc_process, None
        if process is not None and process.state() != QProcess.ProcessState.NotRunning:
            process.kill()
        self.ip_edit.setEnabled(True)
        self.start_pc_button.setEnabled(True)
        self.stop_pc_button.setEnabled(False)
        if was_running:
            self.log_line("PC ping stopped.")

    def run_pc_ping(self):
        # never overlap two pings (a timeout takes ~1 s)
        if self.pc_process is not None:
            return

        ip = self.ip_edit.text().strip()
        if platform.system() == "Windows":
            args = ["-n", "1", "-w", "1000", ip]
        else:
            args = ["-c", "1", "-W", "1", ip]

        process = QProcess(self)
        self.pc_process = process
        process.finished.connect(lambda *_, p=process: self.on_pc_ping_finished(p))
        process.errorOccurred.connect(lambda *_, p=process: self.on_pc_ping_error(p))
        process.start("ping", args)

    def on_pc_ping_finished(self, process):
        output = bytes(process.readAllStandardOutput().data()).decode(
            "utf-8", errors="replace"
        )
        process.deleteLater()
        if process is not self.pc_process:
            return                      # stopped meanwhile: ignore
        self.pc_process = None

        rtt = parse_ping_output(output)
        if rtt is None:
            self.pc_panel.add_timeout()
        else:
            self.pc_panel.add_reply(rtt)

    def on_pc_ping_error(self, process):
        if process is not self.pc_process:
            return
        if process.error() == QProcess.ProcessError.FailedToStart:
            self.log_line("Could not start the system 'ping' command.")
            self.stop_pc_ping()

    # ---------------------------------------------------------------- misc
    def reset_stats(self):
        self.board_panel.reset()
        self.pc_panel.reset()

    def show_image(self, jpeg_bytes):
        """Hook for later: show a received JPEG in the Image Viewer tab."""
        pixmap = QPixmap()
        if pixmap.loadFromData(jpeg_bytes):
            self.image_label.setPixmap(
                pixmap.scaled(
                    self.image_label.size(),
                    Qt.AspectRatioMode.KeepAspectRatio,
                    Qt.TransformationMode.SmoothTransformation,
                )
            )
            self.tabs.setCurrentWidget(self.image_label)

    def closeEvent(self, event):
        self.stop_pc_ping()
        self.disconnect_serial()
        event.accept()


if __name__ == "__main__":

    app = QApplication(sys.argv)

    window = EthernetMonitorApp()
    window.show()

    sys.exit(app.exec())
