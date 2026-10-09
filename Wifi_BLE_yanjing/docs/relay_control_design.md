# 仿生眼 · 继电器温控开关 详细设计文档

版本：v0.2（设计稿，待评审）
关联工程：`D:\Arduino\Wifi_BLE_yanjing`
关联文档：`docs/control_hub_design.md`、`docs/flow_system_design.md`
目标平台：ESP32（Arduino 框架，FreeRTOS 双核）

---

## 1. 需求与范围

本轮只做一件最小可用的事，不引入工厂控制中枢的全套规则引擎：

1. **接线**：接入一个**高电平（3V）触发**的继电器，控制脚接 ESP32 的 **D21 引脚（GPIO21）**。
2. **网页控制**：在现有 Web 上位机上可以手动**打开 / 关闭**继电器，并实时看到继电器状态。
3. **高温联动**：接已有 DHT20 温度数据：
   - 温度 **> 30 °C** → 继电器**启动（吸合 / 打开）**（例如驱动散热设备）；
   - 温度**降下来后** → 继电器**自动关闭（断开）**；
   - 也支持**人工在网页关闭**（人工优先，不与自动逻辑打架）。
4. **安全本地化**：温度联动必须由 ESP32 本机直接执行，不依赖 WiFi / MQTT / 网页；断网、上位机关闭时依然生效。

不在本轮范围：多路继电器、声光报警、RS485、云端历史曲线（可后续叠加，接口已预留）。

---

## 2. 关键假设与约定

| 项 | 约定 | 说明 |
|---|---|---|
| D21 引脚 | **D21 = GPIO21**（DOIT ESP32 DEVKIT 等常见开发板丝印） | 非 strapping，上电安全；见 §3.2 strapping 警告 |
| 触发电平 | **高电平触发**（`RELAY_ACTIVE_HIGH 1`） | IN=高 → 继电器吸合；IN=低 → 断开 |
| 默认状态 | **上电默认断开（OFF）** | 与 GPIO21 上电状态一致，建议加下拉，上电安全 |
| 温度语义 | **温度 > 阈值 → 启动；降温回落后 → 关闭** | 用于高温触发（如风扇/降温设备） |
| 阈值 | 默认 **30 °C**，回差默认 **0.5 °C**（29.5 °C 复归） | 见 §5.2 |
| 温度来源 | 现有 `sensor.cpp` 的 DHT20（`sensorGet()`） | 无需新增硬件 |
| Web 通道 | 复用现有 **Flask 上位机 + MQTT**（`BMP006` 指令 / `BMP004` 状态） | 不新起 ESP32 内置 WebServer，见 §7 |

> 若开发板丝印 D21 实际不是 GPIO21（少数板子不同），只需改 `config.h` 的 `RELAY_PIN`，逻辑不变。

---

## 3. 硬件设计

### 3.1 接线

```
ESP32                        继电器模块（光耦隔离型）
3V3  ──────────────── VCC/DC+（若模块为 5V 线圈，则用 5V，勿接 3V3）
GND  ──────────────── GND   （控制侧必须共地）
D21/GPIO21 ────────── IN    （高电平触发）
                          COM / NO ── 负载回路（按负载电压电流选型）
```

- 优先选**光耦隔离、带三极管驱动**的模块：GPIO 只驱动光耦/三极管，不直接带线圈。
- 若模块为 **5V 供电线圈**：VCC 接 5V，IN 仍可用 3.3V 逻辑（多数模块兼容），务必实测。
- 若是**裸继电器**（无模块）：必须加三极管/MOS 驱动 + 续流二极管（1N4148/1N4007），GPIO 不可直驱线圈。
- 负载为感性（电机、电磁阀）时，触点并联 RC 吸收或压敏电阻。

### 3.2 引脚选择与 strapping 说明

ESP32 上电/复位瞬间会采样几个 **strapping 引脚**（GPIO0、GPIO2、GPIO5、GPIO12、GPIO15）来决定启动模式，此时 `setup()` 尚未运行：

- 若外部电路在这些脚上给出错误电平，可能导致无法下载、无法启动、反复重启。
- 且在写入 `digitalWrite` 之前，引脚默认电平可能让继电器瞬间吸合。

**本方案选用 GPIO21（D21）**：它**不是 strapping 脚**，上电不参与启动模式判定，安全。

注意两点：

1. **GPIO21 是 ESP32 默认 I2C SDA**。本工程 `Wire.begin(26, 27)` 已改用自定义引脚，互不冲突；但需确认开发板 GPIO21 **没有板载 I2C 上拉电阻**（部分板会焊 10k 上拉，会导致上电默认高电平，高电平触发的继电器误吸合）。用万用表量一下最稳。
2. 建议在 GPIO21 与 GND 之间加 **10kΩ 下拉电阻**，保证上电/复位期间保持低电平 = 继电器断开。

> 不推荐 D2（GPIO2）和 D15（GPIO15）：二者都是 strapping 脚；且 D15 复位默认被内部上拉为高，高电平触发的继电器会在上电时直接吸合。
> 若 GPIO21 不可用，可换 GPIO4(D4)、GPIO25(D25)，仅改 `RELAY_PIN`。

### 3.3 供电与隔离

- 继电器模块与 ESP32 控制侧共地；负载侧按隔离要求单独走线。
- 6 个舵机堵转电流大，**舵机 5V 与 ESP32 3V3 分开供电**，单点共地，避免继电器动作引起 brownout。

---

## 4. 软件架构

### 4.1 模块与数据流

```
 DHT20 (I2C)                         网页(浏览器)
      │                                  │ HTTP/SSE
      ▼                                  ▼
 sensorTask ──temp/hum──┐          Flask app.py ── MQTT BMP006/BMP004 ──┐
                         │                                              │
                         ▼                                              ▼
                  ┌────────────────────────────────────────────────────────┐
                  │                     relay.cpp                          │
                  │  手动命令 / 自动温控开关（命令层写）                   │
                  │  relayUpdate(now, temp): 高温置 ON、降温置 OFF + 回差  │
                  │  输出 GPIO2（高=吸合 / 低=断开）                       │
                  └────────────────────────────────────────────────────────┘
                         │
                         ▼
                 telemetry/status 上报（BMP004）→ 网页 / 小智 MCP
```

### 4.2 新增文件

| 文件 | 职责 |
|---|---|
| `relay.h` / `relay.cpp` | 继电器初始化、输出、温度联动状态机、状态查询 |
| `docs/relay_control_design.md` | 本文档 |

改动文件：`config.h`（引脚/参数）、`app.h`（声明）、`Wifi_BLE_yanjing.ino`（`relayInit()`）、`sensor.cpp`（采集后调用 `relayUpdate()`）、`expression.cpp`（`dispatchCommand` 增补命令）、`mcp_tools.cpp`（MCP 工具）、`upper_computer/app.py` + `templates/index.html`（网页与 API）。

### 4.3 并发与存放位置

- 温度联动评估放在 **`sensorTask`（core1）**：该任务每 `RELAY_EVAL_MS`（默认 200ms）用最新缓存温度调用 `relayUpdate()`，DHT20 每 `DHT20_READ_MS`（默认 1s）采样一次。GPIO 写入极轻，不进入 `servoCoreTask`。
- 手动命令来自 MQTT 回调（core1）或 BLE 回调：只**设置目标状态**并立即调用 `relaySet()`，不阻塞。
- 共享状态用 `portMUX_TYPE relayMux` 保护，沿用现有 `exprMux` 风格；`bool` 单字读取本身原子，加锁主要为一致性。

---

## 5. 运行逻辑

### 5.1 数据结构

```cpp
// relay.h
enum RelaySource : uint8_t { RS_AUTO = 0, RS_MANUAL = 1 };

struct RelayRuntime {
  bool        on;             // 实际输出状态
  bool        autoControl;    // 是否启用自动温控（默认开）
  bool        forceOff;       // 人工关闭优先标记，见 §5.3
  bool        hotLatched;     // 当前是否处于"高温已启动"状态
  RelaySource source;         // 最近一次变化的来源
  float       threshold;      // 当前阈值 °C
  uint32_t    firstOverMs;    // 连续超过阈值起始时刻（0=未超）
  uint32_t    lastChangeMs;   // 最近一次输出变化
};
```

```cpp
// 接口
void relayInit();
void relaySet(bool on, const char* reason);   // 手动/命令
void relaySetAuto(bool on);                   // 自动温控总开关
void relaySetThreshold(float t);
void relayUpdate(unsigned long now);          // 采集任务周期调用
bool   relayGet();
String relayStatusJson();                     // 供上报/查询
```

### 5.2 温度联动状态机（核心）

```
                   自动温控开启（autoControl=true）
                              │
        temp > 30°C 且持续 ≥ 防抖  ─────────▶  ON（source=auto, reason=hot）
                              │
        temp ≤ 29.5°C（30 − 回差 0.5）──────▶  OFF（source=auto, reason=cool）
```

判定要点：

- **阈值**：`temp > RELAY_TEMP_THRESHOLD(30)` 启动；**回差**：`temp ≤ THR − HYST(0.5) = 29.5` 关闭，避免 30°C 附近反复通断。
- **防抖**：连续超阈 ≥ `RELAY_TEMP_DEBOUNCE_MS`（默认 0.5s）才确认启动，抑制噪声误触发（仅用于启动）。
- **迟滞带内（29.5~30）**：保持当前状态不变（不动作）。
- **评估节拍**：继电器每 `RELAY_EVAL_MS`（默认 200ms）用最新缓存温度评估一次；DHT20 每 `DHT20_READ_MS`（默认 1s）采样一次。
  - 启动延时 ≈ 采样周期(≤1s) + 防抖(0.5s) ≈ **0.5~1.5s**；
  - 关闭延时 ≈ 采样周期(≤1s) + 评估节拍(≤0.2s) ≈ **≤1.2s**。
  - 想要更快可调小 `DHT20_READ_MS`（不建议 < 500ms，避免 DHT20 自发热）。
- **传感器失效/陈旧**：保持当前输出并上报 `fault`（本场景启动含义是"降温"，失效时盲目断开/吸合都可能不安全，故不自动动作）。

### 5.3 人工优先（网页手动关闭）

为避免"人工刚关掉、自动立刻又打开"的打架，引入 **forceOff**：

| 操作 | 结果 |
|---|---|
| 网页点 **关闭** | `forceOff=true`，输出 **OFF**；高温期间保持关闭（人工优先） |
| 网页点 **开启** | `forceOff=false`，输出 **ON** |
| `forceOff=true` 且温度回落到 **≤ 29.5 °C** | 清除 `forceOff`，恢复自动跟随 |
| 关闭自动温控 `autoControl=false` | 只受手动控制，温度不再影响输出（网页需二次确认） |
| 自动温控开启 + 未人工干预 | 完全按 §5.2 跟随温度 |

> 即：**人工关闭会一直保持，直到温度回到正常（≤29.5°C）后自动解除，之后重新按温度联动。**
> 关于"人工开启"：直接置 ON 并清除 forceOff，温度正常时保持开启直到下一次降温复归事件或人工关闭。

### 5.4 输出映射

```cpp
// 高电平触发
int level = (RELAY_ACTIVE_HIGH ? (on ? HIGH : LOW) : (on ? LOW : HIGH));
digitalWrite(RELAY_PIN, level);
```

---

## 6. 通信协议

### 6.1 MQTT / BLE 命令（`dispatchCommand` 增补）

| 命令 | 别名 / 中文 | 回执 |
|---|---|---|
| `relay on` | 打开继电器 | `relay:on:manual` |
| `relay off` | 关闭继电器 | `relay:off:manual` |
| `relay toggle` | — | `relay:on` / `relay:off` |
| `relay status` | 继电器状态 | `relay:<on\|off>:<auto\|manual>:<autoControl>` |
| `relay auto on` | 开启自动温控 | `relay:auto:on` |
| `relay auto off` | 关闭自动温控 | `relay:auto:off` |
| `relay threshold 30` | 设置温度阈值 | `relay:threshold:30.0` |

### 6.2 状态 / 遥测（`BMP004`）

现有 `sensor.cpp` 上报 `{"temp":..,"hum":..}`。扩展为：

```json
{"temp":31.2,"hum":55.0,"relay":"on","auto":true,"reason":"hot","thr":30.0}
```

- 需把 `pendingTelemetry` 缓冲从 **48 → 128 字节**（`app.h` 与 `app.cpp` 同步）。
- 为避免刷屏，仅当**状态跳变**或温度数值变化时上报（沿用现有温度上报节奏）。
- 网页侧 `_handle_status` 解析 JSON 后分发 `type:"sensor"` 与新增 `type:"relay"` 事件。

### 6.3 MCP 工具（`mcp_tools.cpp`，可选）

| 工具 | 参数 | 用途 |
|---|---|---|
| `relay_control` | `state`: `on`/`off`/`status` | 语音控制继电器 |
| `relay_auto` | `enabled`: bool | 开关自动温控（谨慎） |
| `set_temp_threshold` | `value`: number | 设置阈值 |

> MCP 非实时、可能断连，**绝不作为安全回路**；温控逻辑始终由本机执行。

---

## 7. 网页上位机（`upper_computer/`）

> 采用现有 Flask + MQTT 通道（已确认）：无需在 ESP32 上再开 HTTP 服务，复用现成登录/日志/SSE 框架。

### 7.1 后端 `app.py`

- `Bridge` 增加 `self.relay = {"on": False, "auto": True, "reason": "", "thr": 30.0}`。
- `_handle_status` 解析遥测 JSON 中的 `relay/auto/reason/thr`，广播 `{"type":"relay", ...}`。
- 新增发送方法 `relay_cmd(payload)`（复用 `send_raw`）与 API：

| 方法 | 路径 | 作用 |
|---|---|---|
| `POST` | `/api/relay` | `{state:"on"\|"off"\|"toggle"}` → `relay <state>` |
| `POST` | `/api/relay/auto` | `{enabled:bool}` → `relay auto on/off` |
| `POST` | `/api/relay/threshold` | `{value:number}` → `relay threshold <v>` |
| `GET` | `/api/relay` | 返回本地缓存状态 |

- `/api/state` 与 `/api/stream` 初始帧加入 `relay` 字段。

### 7.2 前端 `index.html`

新增"继电器温控"面板：

- 大开关按钮：`🔌 继电器：开 / 关`（与微动按钮同风格），带状态色与"原因"文字（自动高温 / 手动）。
- 自动温控开关 `🌡 自动温控：开 / 关`（关闭需 `confirm()` 二次确认）。
- 阈值输入框 + 保存按钮（默认 30 °C）。
- 实时温度显示（复用 `sensorTempText`，面板内再显一份）。
- 高温已启动时面板高亮，人工关闭时提示"已人工关闭，温度回落后恢复自动"。
- 新增 `handleEvent` 分支 `case 'relay': setRelayUI(evt)`。

---

## 8. 配置项（`config.h` 新增）

```cpp
// ==================== 继电器（D21 / GPIO21，高电平 3V 触发） ====================
#define RELAY_ENABLED            1
#define RELAY_PIN                21       // D21 = GPIO21（非 strapping，上电安全）
#define RELAY_ACTIVE_HIGH        1        // 1=高电平吸合，0=低电平吸合
#define RELAY_BOOT_SAFE_MS       500      // 上电保持断开的时间
#define RELAY_EVAL_MS            200      // 继电器评估节拍 ms

// ==================== 自动温控 ====================
#define RELAY_TEMP_AUTO_ENABLED  1        // 默认启用自动温控
#define RELAY_TEMP_THRESHOLD     30.0f    // 高于该温度启动继电器 °C
#define RELAY_TEMP_HYSTERESIS    0.5f     // 回差，降到(阈值-0.5)就关闭
#define RELAY_TEMP_DEBOUNCE_MS   500      // 连续超阈确认时间 ms（仅启动）
#define RELAY_SENSOR_STALE_MS    10000    // 缓存温度超过该时长视为失效
```

---

## 9. 分阶段实施与验收

### P1 · 硬件与手动开关
- `relay.h/.cpp`、`relayInit/relaySet/relayGet`，接入 `.ino` 与 `config.h`。
- 命令 `relay on/off/status`。
- 验收：MQTT 发 `relay on` 继电器吸合，`relay off` 断开；`relay status` 回显正确。

### P2 · 温度联动
- `relayUpdate()` 接入 `sensorTask`，实现阈值 + 回差 + 防抖 + 人工优先。
- 遥测 JSON 扩展与 `pendingTelemetry` 扩容；支持 `relay auto/threshold` 命令。
- 验收：加热超过 30 °C 继电器自动启动；降到 29.5 °C 以下自动关闭；高温时网页手动关闭能保持，温度正常后恢复自动。

### P3 · 网页可视化
- `app.py` API/SSE + `index.html` 面板。
- 验收：网页可开/关、看到实时温度与触发原因、可改阈值；关闭自动温控需二次确认。

### P4 ·（可选）MCP 语音
- `relay_control` / `relay_auto` / `set_temp_threshold` 工具；小智可查询与手动控制。

---

## 10. 风险与安全清单

| 风险 | 对策 |
|---|---|
| 上电瞬间继电器抖动/误吸合 | `RELAY_BOOT_SAFE_MS` 静默 + 尽早写 OFF + GPIO21 加 10kΩ 下拉 |
| GPIO21 板载 I2C 上拉导致上电默认高电平 | 实测确认无板载上拉；高电平触发下需下拉保持断开 |
| 模块 5V 线圈 vs 3.3V 逻辑不匹配 | 光耦/三极管模块；实测 IN 触发电压 |
| 阈值附近反复通断（chatter） | 回差 + 防抖 + 迟滞带内保持 |
| 人工关闭与自动打架 | `forceOff` 人工优先，降温复归后自动解除 |
| 传感器失效导致误判 | 保持当前输出并上报 fault，不盲目动作 |
| DHT20 1s 采集延迟 | 启动延时可调小 `DHT20_READ_MS` 或 `RELAY_TEMP_DEBOUNCE_MS`；不建议 < 500ms |
| 网页/MQTT 不能作安全回路 | 温控 100% 本机执行，断网仍生效 |
| 继电器动作引起电源跌落 | 独立供电、光耦隔离、大电容 |
| `pendingTelemetry[48]` 溢出 | 扩容到 128，并 `snprintf` 截断保护 |

---

## 11. 待确认问题

1. **D21 丝印**：确认开发板 D21 确为 GPIO21，且无板载 I2C 上拉；若不可用，可否改用 GPIO4/25？
2. **继电器模块**：是光耦模块还是裸继电器？线圈供电 3.3V 还是 5V？负载类型/电压/电流？
3. **默认阈值**：30 °C + 回差 0.5 °C（29.5 °C 复归）是否合适？是否需要掉电保存（NVS）？
4. **人工优先范围**：人工关闭保持到降温（≤29.5 °C）后自动恢复，是否符合预期？是否还需要"人工关闭后一直保持，直到人工再开"的强保持模式？
5. **人工开启**：温度正常时网页手动开启后，是否需要"下次降温自动关闭"？
6. **失效策略**：传感器失效时保持当前输出（本设计）是否可接受？
