# -*- coding: utf-8 -*-
"""
智能仿生眼控制上位机（Flask 版）
架构：浏览器(HTTP + SSE) <-> Flask <-> MQTT over TCP <-> broker <-> ESP32
依赖：pip install flask paho-mqtt
运行：python app.py  然后浏览器打开 http://127.0.0.1:5000
"""
import json
import queue
import threading
import time

from flask import Flask, Response, jsonify, render_template, request
import paho.mqtt.client as mqtt

# ==================== 默认配置（与 ESP32 固件一致） ====================
DEFAULT_CONFIG = {
    "host": "119.91.109.180",
    "port": 9501,
    "client_id": "0cf0a6fc7b174a1cb6c716a17b671472",
    "cmd_topic": "BMP006",
    "status_topic": "BMP004",
}

ACK_TIMEOUT_MS = 5000
LATE_ACK_GRACE_MS = 15000

KNOWN_EXPRESSIONS = ["init", "close", "dis", "surprise", "tired", "smile", "wink",
                     "left_open", "right_open", "blink", "anger", "shy"]

# 固件预置动作流（与 flows.h 保持一致）
KNOWN_FLOWS = ["selftest", "greet", "alert", "sleep", "wake", "scan", "demo"]
FLOW_LABELS = {
    "selftest": "🩺 自检",
    "greet": "👋 打招呼",
    "alert": "🚨 告警",
    "sleep": "😴 休眠",
    "wake": "⏰ 唤醒",
    "scan": "👁 巡视",
    "demo": "🎬 演示",
}

# 动作流（行为层）开关：设备回复这两个状态之一
CONTROL_STATES = ["behavior_on", "behavior_off"]
BEHAVIOR_CMD_ON = "behavior on"
BEHAVIOR_CMD_OFF = "behavior off"

app = Flask(__name__)


def normalize(v):
    return str(v if v is not None else "").strip().lower()


def sse(evt):
    return "data: {}\n\n".format(json.dumps(evt, ensure_ascii=False))


# ==================== MQTT 桥 ====================
class Bridge:
    def __init__(self):
        self.lock = threading.RLock()
        self.client = None
        self.config = dict(DEFAULT_CONFIG)
        self.connected = False
        self.current_state = ""
        self.pending = None      # {"cmd", "deadline", "event", "result"}
        self.recent = []         # [(cmd, time)]
        self.subs = []           # [queue.Queue]
        self.seq_running = False
        self.seq_stop = threading.Event()
        self.seq_thread = None
        self.behavior_enabled = None
        self.flow = {"running": False, "name": "", "event": ""}
        self.sensor = {"temp": None, "hum": None, "ts": None}
        self.relay = {"on": False, "auto": True, "reason": "", "thr": 30.0}

        self._watcher_thread = threading.Thread(target=self._watcher, daemon=True)
        self._watcher_thread.start()

    # ---------- 事件广播（推给所有 SSE 订阅者） ----------
    def emit(self, evt):
        for q in list(self.subs):
            try:
                q.put_nowait(dict(evt))
            except queue.Full:
                pass

    # ---------- 待确认命令超时监视 ----------
    def _watcher(self):
        while True:
            time.sleep(0.05)
            with self.lock:
                p = self.pending
                if p and time.time() > p["deadline"]:
                    self.pending = None
                    self._append_recent(p["cmd"])
                    p["result"] = {"ok": False, "reason": "timeout"}
                    p["event"].set()
                    self.emit({"type": "log", "level": "error",
                               "message": "⏱ {} ms 内未收到设备对指令 {} 的同步回复，暂记为未确认；若稍后收到回传，将按延迟确认处理。".format(ACK_TIMEOUT_MS, p["cmd"])})
                    self.emit({"type": "pending", "cmd": None})

    def _append_recent(self, cmd):
        now = time.time()
        self.recent = [(c, t) for c, t in self.recent if now - t <= LATE_ACK_GRACE_MS / 1000.0]
        self.recent.append((cmd, now))

    def _consume_recent(self, cmd):
        now = time.time()
        found = False
        kept = []
        for c, t in self.recent:
            if now - t <= LATE_ACK_GRACE_MS / 1000.0:
                if c == cmd and not found:
                    found = True
                else:
                    kept.append((c, t))
        self.recent = kept
        return found

    # ---------- 连接管理 ----------
    def connect(self, config):
        with self.lock:
            self.disconnect()
            self.config = config
            c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1,
                            client_id=config["client_id"], protocol=mqtt.MQTTv311)
            c.username_pw_set(config["username"], config["password"])
            c.on_connect = self._on_connect
            c.on_disconnect = self._on_disconnect
            c.on_message = self._on_message
            c.reconnect_delay_set(1, 30)
            c.loop_start()
            self.client = c
            self.connected = False
            self.current_state = ""
            try:
                c.connect_async(config["host"], config["port"], keepalive=30)
            except Exception as e:
                self.emit({"type": "log", "level": "error", "message": "连接发起失败：" + str(e)})
        self.emit({"type": "conn", "status": "connecting"})
        self.emit({"type": "log", "level": "info",
                   "message": "正在连接 MQTT {}:{} ...".format(config["host"], config["port"])})

    def disconnect(self):
        with self.lock:
            if self.client is not None:
                try:
                    self.client.loop_stop()
                    self.client.disconnect()
                except Exception:
                    pass
            self.client = None
            self.connected = False
            if self.pending:
                p = self.pending
                self.pending = None
                p["result"] = {"ok": False, "reason": "disconnected"}
                p["event"].set()
        self.emit({"type": "conn", "status": "disconnected"})

    def _on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            self.connected = True
            self.emit({"type": "conn", "status": "connected"})
            self.emit({"type": "log", "level": "info",
                       "message": "MQTT 已连接：{}:{}".format(self.config["host"], self.config["port"])})
            try:
                client.subscribe(self.config["status_topic"], qos=0)
                self.emit({"type": "log", "level": "info",
                           "message": "已订阅状态主题：" + self.config["status_topic"]})
            except Exception as e:
                self.emit({"type": "log", "level": "error", "message": "订阅失败：" + str(e)})
        else:
            self.connected = False
            self.emit({"type": "conn", "status": "disconnected"})
            hints = {
                1: "协议版本错误（服务器可能只支持 MQTT 3.1）",
                2: "Client ID 被服务器拒绝",
                3: "服务器不可用（broker 服务未启动）",
                4: "用户名或密码错误",
                5: "认证失败：Client ID / 用户名 / 密码（私钥）不正确",
            }
            hint = hints.get(rc, "")
            msg = "MQTT 连接被拒绝，返回码 rc={}。{}".format(rc, hint)
            if rc == 5:
                msg += " 请到巴法云控制台核对私钥，并确认 Client ID 与私钥一致。"
            self.emit({"type": "log", "level": "error", "message": msg})

    def _on_disconnect(self, client, userdata, rc):
        self.connected = False
        self.emit({"type": "conn", "status": "disconnected"})
        self.emit({"type": "log", "level": "error", "message": "MQTT 连接已断开，paho 将自动重连。"})
        with self.lock:
            if self.pending:
                p = self.pending
                self.pending = None
                p["result"] = {"ok": False, "reason": "disconnected"}
                p["event"].set()
        self.emit({"type": "pending", "cmd": None})

    def _on_message(self, client, userdata, msg):
        text = ""
        try:
            text = msg.payload.decode("utf-8", errors="replace")
        except Exception:
            pass
        topic = msg.topic or ""
        if topic != self.config["status_topic"]:
            self.emit({"type": "log", "level": "info",
                       "message": "收到非状态主题消息：" + topic + "，已忽略。"})
            return
        self._handle_status(text, "设备回复")

    # ---------- 状态同步 / ACK ----------
    def _handle_status(self, text, source="设备回复"):
        raw = str(text or "").strip()
        t = normalize(text)

        # 动作流事件：flow|<name>|<run|done|stop|busy|unknown>
        if t.startswith("flow|"):
            parts = raw.split("|")
            name = parts[1] if len(parts) > 1 else ""
            ev = parts[2] if len(parts) > 2 else ""
            running = (ev == "run")
            self.flow = {"running": running, "name": name if running else "", "event": ev}
            self.emit({"type": "flow", "running": running, "name": name, "event": ev})
            label = FLOW_LABELS.get(name, name)
            if ev == "run":
                msg = "▶ 动作流开始：{}（{}）".format(label, name)
            elif ev == "done":
                msg = "✅ 动作流完成：{}".format(label)
            elif ev == "stop":
                msg = "⏹ 动作流已停止：{}".format(label)
            elif ev == "busy":
                msg = "⛔ 动作流被拒绝：当前已有动作流在运行"
            elif ev == "unknown":
                msg = "⚠️ 未知动作流名称"
            else:
                msg = "动作流事件：{}".format(ev)
            self.emit({"type": "log", "level": "received", "message": "📥 " + msg})
            return

        # 环境遥测（JSON）：{"temp":25.3,"hum":60.1}
        if raw.startswith("{"):
            try:
                obj = json.loads(raw)
            except Exception:
                obj = None
            if isinstance(obj, dict) and ("temp" in obj or "hum" in obj):
                self.sensor = {"temp": obj.get("temp"), "hum": obj.get("hum"), "ts": time.time()}
                self.emit({"type": "sensor", "temp": obj.get("temp"), "hum": obj.get("hum")})
                if "relay" in obj:
                    self.relay = {
                        "on": obj.get("relay") == "on",
                        "auto": bool(obj.get("auto", True)),
                        "reason": obj.get("reason", ""),
                        "thr": obj.get("thr"),
                    }
                    self.emit({"type": "relay", **self.relay})
                return

        # 微动开关状态：独立事件，不参与表情 ACK 流程
        if t in CONTROL_STATES:
            enabled = (t == "behavior_on")
            self.behavior_enabled = enabled
            self.emit({"type": "behavior", "enabled": enabled})
            self.emit({"type": "log", "level": "received",
                       "message": "📥 微动状态更新：{}".format("已开启" if enabled else "已关闭")})
            return

        if not t:
            self.emit({"type": "log", "level": "info", "message": "收到空状态消息，已忽略。"})
            return
        if t not in KNOWN_EXPRESSIONS:
            self.emit({"type": "log", "level": "info", "message": "收到未知状态消息：" + t + "，已忽略。"})
            return

        with self.lock:
            p = self.pending
            if p:
                if t == p["cmd"]:
                    self.pending = None
                    p["result"] = {"ok": True, "reason": "confirmed"}
                    p["event"].set()
                    self.current_state = t
                    self.emit({"type": "log", "level": "received",
                               "message": "📥 收到设备同步信息：{}，命令已确认。来源：{}".format(t, source)})
                    self.emit({"type": "state", "expr": t})
                    self.emit({"type": "pending", "cmd": None})
                    return
                if self._consume_recent(t):
                    self.current_state = t
                    self.emit({"type": "log", "level": "info",
                               "message": "📥 收到此前超时指令的延迟回传：{}；当前仍等待 {}，不覆盖待确认流程。".format(t, p["cmd"])})
                    self.emit({"type": "state", "expr": t})
                    return
                self.emit({"type": "log", "level": "info",
                           "message": "📥 收到设备同步信息：{}，但当前等待的是 {}；暂不更新界面。".format(t, p["cmd"])})
                return

            late = self._consume_recent(t)
            self.current_state = t
            if late:
                self.emit({"type": "log", "level": "info",
                           "message": "📥 收到此前超时指令的延迟回传：{}，已按设备实际状态同步。".format(t)})
            else:
                self.emit({"type": "log", "level": "received",
                           "message": "📥 收到设备同步信息：{}，已同步当前状态。来源：{}".format(t, source)})
            self.emit({"type": "state", "expr": t})

    # ---------- 发送指令 ----------
    def send_command(self, cmd, wait=False):
        c = normalize(cmd)
        if c not in KNOWN_EXPRESSIONS:
            return {"ok": False, "reason": "unknown_expression"}

        with self.lock:
            if not self.connected or self.client is None:
                self.emit({"type": "log", "level": "error",
                           "message": "未连接 MQTT 服务器，无法发送指令：" + c})
                return {"ok": False, "reason": "not_connected"}

            if c in ("anger", "shy"):
                self.emit({"type": "log", "level": "info",
                           "message": "提示：当前 ESP32 固件暂未实现 " + c + " 表情，设备可能会忽略该指令。"})

            try:
                self.client.publish(self.config["cmd_topic"], c, qos=0)
            except Exception as e:
                self.emit({"type": "log", "level": "error", "message": "发送异常：" + str(e)})
                return {"ok": False, "reason": "publish_error"}

            if self.pending:
                old = self.pending
                self.pending = None
                old["result"] = {"ok": False, "reason": "overridden"}
                old["event"].set()
                self.emit({"type": "log", "level": "info",
                           "message": "上一个待确认命令 {} 被新的命令 {} 覆盖。".format(old["cmd"], c)})

            pend = {"cmd": c,
                    "deadline": time.time() + ACK_TIMEOUT_MS / 1000.0,
                    "event": threading.Event(),
                    "result": None}
            self.pending = pend

            self.emit({"type": "log", "level": "sent",
                       "message": "📤 已发送指令：{}，等待设备在 {} ms 内回传。".format(c, ACK_TIMEOUT_MS)})
            self.emit({"type": "pending", "cmd": c})

        if not wait:
            return {"ok": True, "reason": "sent"}

        pend["event"].wait(ACK_TIMEOUT_MS / 1000.0 + 1.0)
        return pend["result"] or {"ok": False, "reason": "timeout"}

    # ---------- 微动开关 ----------
    def set_behavior(self, enabled):
        cmd = BEHAVIOR_CMD_ON if enabled else BEHAVIOR_CMD_OFF
        with self.lock:
            if not self.connected or self.client is None:
                self.emit({"type": "log", "level": "error",
                           "message": "未连接 MQTT 服务器，无法切换微动。"})
                return {"ok": False, "reason": "not_connected"}
            try:
                self.client.publish(self.config["cmd_topic"], cmd, qos=0)
            except Exception as e:
                self.emit({"type": "log", "level": "error", "message": "发送异常：" + str(e)})
                return {"ok": False, "reason": "publish_error"}
        self.emit({"type": "log", "level": "sent",
                   "message": "📤 已发送微动指令：{}".format(cmd)})
        return {"ok": True}

    # ---------- 动作流（程序） ----------
    def send_raw(self, payload):
        with self.lock:
            if not self.connected or self.client is None:
                self.emit({"type": "log", "level": "error",
                           "message": "未连接 MQTT 服务器，无法发送：" + payload})
                return {"ok": False, "reason": "not_connected"}
            try:
                self.client.publish(self.config["cmd_topic"], payload, qos=0)
            except Exception as e:
                self.emit({"type": "log", "level": "error", "message": "发送异常：" + str(e)})
                return {"ok": False, "reason": "publish_error"}
        return {"ok": True}

    def flow_start(self, name):
        n = normalize(name)
        if n not in KNOWN_FLOWS:
            return {"ok": False, "reason": "unknown_flow"}
        res = self.send_raw("flow start " + n)
        if res.get("ok"):
            self.emit({"type": "log", "level": "sent",
                       "message": "📤 已请求运行动作流：{}".format(FLOW_LABELS.get(n, n))})
        return res

    def flow_stop(self):
        res = self.send_raw("flow stop")
        if res.get("ok"):
            self.emit({"type": "log", "level": "sent", "message": "📤 已请求停止动作流"})
        return res

    # ---------- 继电器 / 温控 ----------
    def relay_cmd(self, state):
        s = normalize(state)
        if s not in ("on", "off", "toggle"):
            return {"ok": False, "reason": "unknown_state"}
        res = self.send_raw("relay " + s)
        if res.get("ok"):
            label = {"on": "打开", "off": "关闭", "toggle": "切换"}.get(s, s)
            self.emit({"type": "log", "level": "sent", "message": "📤 已发送继电器指令：{}".format(label)})
        return res

    def relay_set_auto(self, enabled):
        cmd = "relay auto " + ("on" if enabled else "off")
        res = self.send_raw(cmd)
        if res.get("ok"):
            self.emit({"type": "log", "level": "sent",
                       "message": "📤 自动温控：{}".format("开启" if enabled else "关闭")})
        return res

    def relay_set_threshold(self, value):
        try:
            v = float(value)
        except (TypeError, ValueError):
            return {"ok": False, "reason": "invalid_value"}
        cmd = "relay threshold {}".format(v)
        res = self.send_raw(cmd)
        if res.get("ok"):
            self.emit({"type": "log", "level": "sent",
                       "message": "📤 温度阈值已发送：{} °C".format(v)})
        return res

    # ---------- 模拟设备回复 ----------
    def simulate_reply(self):
        with self.lock:
            if self.pending:
                s = self.pending["cmd"]
            else:
                s = self.current_state or "init"
        self.emit({"type": "log", "level": "info", "message": "🧪 本地模拟设备回复：" + s})
        threading.Timer(0.15, self._handle_status, args=(s, "本地模拟")).start()

    # ---------- 多表情序列 ----------
    def start_sequence(self, items, interval_ms, repeat, continue_on_timeout, return_init):
        items = [normalize(x) for x in items if normalize(x) in KNOWN_EXPRESSIONS]
        with self.lock:
            if self.seq_running:
                return {"ok": False, "reason": "already_running"}
            if not items:
                return {"ok": False, "reason": "empty_sequence"}
            if not self.connected:
                return {"ok": False, "reason": "not_connected"}
            self.seq_running = True
            self.seq_stop.clear()

        self.seq_thread = threading.Thread(
            target=self._seq_run,
            args=(items, interval_ms, repeat, continue_on_timeout, return_init),
            daemon=True)
        self.seq_thread.start()
        return {"ok": True}

    def _seq_run(self, items, interval, repeat, cont, ret_init):
        self.emit({"type": "log", "level": "info",
                   "message": "▶ 开始多动作循环：序列={}，循环次数={}，间隔={} ms。".format(" → ".join(items), repeat, interval)})
        stopped = False
        try:
            for rnd in range(1, repeat + 1):
                if self.seq_stop.is_set():
                    stopped = True
                    break
                for cmd in items:
                    if self.seq_stop.is_set():
                        stopped = True
                        break
                    self.emit({"type": "seq", "running": True,
                               "text": "第 {}/{} 轮，正在执行：{}".format(rnd, repeat, cmd)})
                    res = self.send_command(cmd, wait=True)
                    if res.get("ok"):
                        self.emit({"type": "log", "level": "received", "message": "✅ 指令 {} 已确认。".format(cmd)})
                    else:
                        self.emit({"type": "log", "level": "error",
                                   "message": "多动作中的指令 {} 未确认（{}）。".format(cmd, res.get("reason", "unknown"))})
                        if not cont:
                            self.emit({"type": "log", "level": "error",
                                       "message": "已设置为超时不继续，多动作循环终止。"})
                            stopped = True
                            break
                    self.seq_stop.wait(interval / 1000.0)
        finally:
            if not stopped and ret_init:
                self.emit({"type": "log", "level": "info",
                           "message": "↩ 循环结束，自动发送初始化指令 init，使仿生眼回正。"})
                self.send_command("init", wait=True)
            with self.lock:
                self.seq_running = False
            self.emit({"type": "seq", "running": False,
                       "text": "已停止" if stopped else "执行完成"})
            self.emit({"type": "log", "level": "received" if stopped else "info",
                       "message": "⏹ 多动作循环已停止。" if stopped else "✅ 多动作循环执行完成。"})

    def stop_sequence(self):
        self.seq_stop.set()
        self.emit({"type": "log", "level": "info", "message": "⏹ 已请求停止多动作循环。当前已发送的单个指令仍会等待设备回传。"})


bridge = Bridge()


# ==================== 页面 ====================
@app.route("/")
def index():
    return render_template("index.html")


# ==================== API ====================
@app.get("/api/config")
def api_get_config():
    return jsonify({"ok": True, "config": bridge.config})


@app.post("/api/config")
def api_set_config():
    data = request.get_json(silent=True) or {}
    host = str(data.get("host", "")).strip()
    port = data.get("port")
    cid = str(data.get("client_id", "")).strip()

    if not host:
        return jsonify({"ok": False, "reason": "Host 不能为空"})
    try:
        port = int(port)
        if not 1 <= port <= 65535:
            raise ValueError
    except (TypeError, ValueError):
        return jsonify({"ok": False, "reason": "Port 必须是 1 到 65535 之间的数字"})
    if not cid:
        return jsonify({"ok": False, "reason": "Client ID 不能为空"})

    config = {
        "host": host,
        "port": port,
        "client_id": cid,
        "username": cid,
        "password": cid,
        "cmd_topic": str(data.get("cmd_topic") or DEFAULT_CONFIG["cmd_topic"]).strip(),
        "status_topic": str(data.get("status_topic") or DEFAULT_CONFIG["status_topic"]).strip(),
    }
    bridge.connect(config)
    return jsonify({"ok": True})


@app.post("/api/disconnect")
def api_disconnect():
    bridge.disconnect()
    return jsonify({"ok": True})


@app.post("/api/behavior")
def api_behavior():
    data = request.get_json(silent=True) or {}
    enabled = bool(data.get("enabled", True))
    return jsonify(bridge.set_behavior(enabled))


@app.get("/api/flow/list")
def api_flow_list():
    return jsonify({
        "ok": True,
        "flows": [{"name": n, "label": FLOW_LABELS.get(n, n)} for n in KNOWN_FLOWS],
        "running": bridge.flow,
    })


@app.post("/api/flow/start")
def api_flow_start():
    data = request.get_json(silent=True) or {}
    return jsonify(bridge.flow_start(data.get("name", "")))


@app.post("/api/flow/stop")
def api_flow_stop():
    return jsonify(bridge.flow_stop())


@app.get("/api/relay")
def api_relay_get():
    return jsonify({"ok": True, "relay": bridge.relay})


@app.post("/api/relay")
def api_relay_cmd():
    data = request.get_json(silent=True) or {}
    return jsonify(bridge.relay_cmd(data.get("state", "")))


@app.post("/api/relay/auto")
def api_relay_auto():
    data = request.get_json(silent=True) or {}
    return jsonify(bridge.relay_set_auto(bool(data.get("enabled", True))))


@app.post("/api/relay/threshold")
def api_relay_threshold():
    data = request.get_json(silent=True) or {}
    return jsonify(bridge.relay_set_threshold(data.get("value")))


@app.get("/api/sensor")
def api_sensor():
    return jsonify({"ok": True, "sensor": bridge.sensor})


@app.get("/api/state")
def api_state():
    return jsonify({
        "ok": True,
        "connected": bridge.connected,
        "state": bridge.current_state,
        "seq_running": bridge.seq_running,
        "pending": bridge.pending["cmd"] if bridge.pending else None,
        "behavior": bridge.behavior_enabled,
        "flow": bridge.flow,
        "sensor": bridge.sensor,
        "relay": bridge.relay,
    })


@app.post("/api/command")
def api_command():
    data = request.get_json(silent=True) or {}
    res = bridge.send_command(data.get("cmd", ""))
    return jsonify(res)


@app.post("/api/sequence/start")
def api_seq_start():
    data = request.get_json(silent=True) or {}
    actions = data.get("actions") or []
    interval_ms = int(data.get("interval_ms") or 3000)
    repeat = int(data.get("repeat") or 3)
    continue_on_timeout = bool(data.get("continue_on_timeout", True))
    return_init = bool(data.get("return_to_init", True))
    interval_ms = max(300, min(10000, interval_ms))
    repeat = max(1, min(99, repeat))
    res = bridge.start_sequence(actions, interval_ms, repeat, continue_on_timeout, return_init)
    return jsonify(res)


@app.post("/api/sequence/stop")
def api_seq_stop():
    bridge.stop_sequence()
    return jsonify({"ok": True})


@app.route("/api/simulate", methods=["GET", "POST"])
def api_simulate():
    bridge.simulate_reply()
    return jsonify({"ok": True})


@app.get("/api/stream")
def api_stream():
    q = queue.Queue(maxsize=200)
    bridge.subs.append(q)

    def gen():
        try:
            yield sse({"type": "conn", "status": "connected" if bridge.connected else "disconnected"})
            if bridge.current_state:
                yield sse({"type": "state", "expr": bridge.current_state})
            if bridge.behavior_enabled is not None:
                yield sse({"type": "behavior", "enabled": bridge.behavior_enabled})
            yield sse({"type": "flow", "running": bridge.flow.get("running", False),
                       "name": bridge.flow.get("name", ""), "event": bridge.flow.get("event", "")})
            if bridge.sensor.get("temp") is not None or bridge.sensor.get("hum") is not None:
                yield sse({"type": "sensor", "temp": bridge.sensor.get("temp"),
                           "hum": bridge.sensor.get("hum")})
            yield sse({"type": "relay", **bridge.relay})
            while True:
                try:
                    evt = q.get(timeout=15)
                    yield sse(evt)
                except queue.Empty:
                    yield ": ping\n\n"
        finally:
            try:
                bridge.subs.remove(q)
            except ValueError:
                pass

    return Response(gen(), mimetype="text/event-stream",
                    headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})


if __name__ == "__main__":
    print("=" * 56)
    print("  智能仿生眼控制上位机（Flask 版）")
    print("  浏览器打开: http://127.0.0.1:5000")
    print("  局域网访问: http://<本机IP>:5000")
    print("=" * 56)
    app.run(host="0.0.0.0", port=5000, threaded=True, debug=False)
