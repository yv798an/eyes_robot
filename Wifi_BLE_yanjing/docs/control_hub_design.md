# 仿生眼 · 机器人工厂控制中枢 详细设计文档

版本：v0.1（设计稿，待评审）
关联工程：`D:\Arduino\Wifi_BLE_yanjing`
目标平台：ESP32（Arduino 框架，FreeRTOS 双核）

---

## 1. 目标与范围

在**不破坏现有仿生眼全部功能**（6 舵机表情、动作序列、眨眼/微扫视/漂移行为层、摇杆/电位器手动模式、WiFi+MQTT、BLE NUS 备用链路、小智 MCP）的前提下，扩展为一个"机器人工厂控制中枢"：

- **感知**：接入数个环境类传感器（温湿度、温度、模拟量/开关量）。
- **对话**：通过小智 MCP 让小智能查询实时数据、历史、状态、配置阈值。
- **决策**：本地规则引擎，对传感器数据做分级判断。
- **动作**：异常时本地执行继电器停机、声光报警，并通过 MQTT 上报，表情作为直观反馈。
- **可视**：上位机 `app.py` + `index.html` 增加传感仪表盘、告警面板、事件历史。

设计原则：

1. **安全本地化**：停机/切断等安全动作必须由本机逻辑直接完成，不依赖 WiFi、MQTT、MCP 或云端。
2. **表驱动**：新增传感器 = 在配置表加一行，不改核心逻辑；新增规则同理。
3. **非阻塞**：所有任务不得长时间 `delay`；沿用现有固定节拍风格。
4. **可裁剪**：任何传感器/规则可通过宏关闭，保证旧功能可单独编译运行。
5. **不破坏现有协议**：`BMP004`（状态/表情 ACK）语义保持不变，新增独立主题。

---

## 2. 现状回顾与扩展点

| 文件 | 现有职责 | 本次改动 |
|---|---|---|
| `Wifi_BLE_yanjing.ino` | 启动 3 个 FreeRTOS 任务 | 新增 `sensorTask` |
| `config.h` | 引脚/参数/开关 | 新增传感器表、规则表、继电器/MQTT 主题宏 |
| `app.h` / `app.cpp` | 类型、全局状态、表情帧 | 新增传感器/Alarm 全局状态与共享快照 |
| `motion.cpp` | 规划、舵机输出、输出合成 | 略改：合成时叠加告警表情覆盖 |
| `behavior.cpp` | 眨眼/微扫视/漂移 | 略改：告警时抑制（可选） |
| `expression.cpp` | 表情、解析、队列、分发 | 新增告警触发表情入口（复用 `startSingleExpr`） |
| `control.cpp` | 手动/任务/WiFi | 新增 `sensorTask`、`sendTelemetry()` 调用点 |
| `links.cpp` | BLE + MQTT | 新增 MQTT 上报发布函数 |
| `mcp_tools.cpp` | 2 个表情工具 | 新增传感器/告警相关工具 |
| `app.py` / `index.html` | MQTT 上位机 | 新增 telemetry/alarm 订阅、API、UI |

> 结论：现有分层（命令层只发请求、舵机任务内规划）**保持不变**，新功能以"新增模块 + 挂队列"方式接入。

---

## 3. 硬件设计

### 3.1 引脚占用与规划

现有占用（不可动）：

| 功能 | 引脚 |
|---|---|
| 舵机 TL/BL/TR/BR/X/Y | 16, 17, 22, 23, 18, 19 |
| 摇杆 X/Y | 33, 32 |
| 摇杆按键 SW | 13 |
| 电位器 R | 34（ADC1，输入专用） |

本次新增（**注意：WiFi 运行时 ADC2 不可用，模拟量只能用 ADC1**）：

| 功能 | 建议引脚 | 说明 |
|---|---|---|
| I2C SDA | GPIO27 | DHT20 数据线（自定义，避开默认 22 的 TR 舵机） |
| I2C SCL | GPIO26 | DHT20 时钟线 |
| DS18B20 单总线 | GPIO4 | 需 4.7kΩ 上拉到 3V3 |
| 继电器 1（安全停机） | GPIO5 | 光耦模块，低电平触发 |
| 继电器 2（声光/预留） | GPIO25 | 光耦模块 |
| 蜂鸣器 | GPIO26 | 有源蜂鸣器或经三极管 |
| 备用数字/模拟输入 | GPIO35 / 36 / 39 | ADC1，输入专用，无内部上拉 |
| （预留）RS485 Modbus | GPIO14(RX) / 15(TX) | 后续扩展，注意 strapping |

**禁止/谨慎使用**：GPIO0/2/12/15 是 strapping 脚（上电电平影响启动模式），新外设尽量避开；GPIO1/3 是串口下载/日志。

### 3.2 传感器选型（本期）

| 设备 | 接口 | 地址/引脚 | 产出通道 | 采样周期 |
|---|---|---|---|---|
| SHT30 / SHT31 | I2C | 0x44 | 温度、湿度 | 1s |
| BME280（可选替代） | I2C | 0x76/0x77 | 温度、湿度、气压 | 1s |
| DS18B20 ×N | OneWire | GPIO4 | 温度 ×N | 2s（转换需 750ms） |
| 模拟量（烟感/光照等） | ADC1 | 35/36/39 | 原始电压→物理量 | 200ms |
| 开关量（门磁/液位） | GPIO | 35/36/39 数字模式 | 0/1 | 100ms |

### 3.3 执行器（继电器安全逻辑）

- 采用**光耦隔离继电器模块**，控制侧 3V3，负载侧独立供电。
- **失效安全（fail-safe）**：定义"继电器吸合 = 设备允许运行"。因此：
  - MCU 掉电/复位 → 继电器释放 → 被测设备停止（安全）。
  - L2/L3 告警 → 主动释放继电器 → 停机。
- 触点容量按实际负载选择，感性负载并联 RC 吸收/续流二极管。
- 共地：控制侧与 ESP32 共地；负载侧按隔离要求处理。

### 3.4 供电

- 6 舵机堵转电流大，务必**舵机独立 5V/大电流供电**，与 ESP32 3V3 分开走线，单点共地。
- 传感器走 3V3；继电器模块用 5V（若为 5V 模块，注意电平匹配，优先选 3V3 兼容或光耦）。
- 保留现有 brownout 诊断（`Wifi_BLE_yanjing.ino:30`）。

---

## 4. 软件架构

### 4.1 模块总览

```
                     ┌──────────────────────────────────────────┐
   传感器硬件 ──I2C──▶│  sensorTask (core1)                       │
             ──1-Wire─▶│   ├─ 驱动层 driver 读原始值               │
             ──ADC───▶│   ├─ 滤波(EMA) + 陈旧检测                 │
                     │   ├─ 写入 g_sensors 共享快照(带锁)         │
                     │   └─ rulesEvaluate() 规则引擎             │
                     └───────────────┬──────────────────────────┘
                                     │ 触发
                     ┌───────────────▼──────────────────────────┐
                     │ 告警分发                                  │
                     │  ├─ actuator: 继电器/蜂鸣(本地, 立即)      │
                     │  ├─ alertQueue ─▶ servoCoreTask: 表情覆盖  │
                     │  ├─ eventLog: 环形缓冲记录                 │
                     │  ├─ netTask: MQTT 上报(telemetry/alarm)    │
                     │  └─ MCP: 主动通知/应答                     │
                     └──────────────────────────────────────────┘
```

### 4.2 任务与并发

| 任务 | 核心 | 优先级 | 周期 | 说明 |
|---|---|---|---|---|
| `servoCoreTask` | 0 | 3 | 10ms | 现有，实时性最高；消费告警表情 |
| `networkAuxTask` | 1 | 1 | — | 现有，WiFi+MQTT；增加周期上报 |
| `mcpTask` | 1 | 1 | 5ms | 现有 |
| `sensorTask`（新） | 1 | 2 | 100ms | 传感器采集 + 规则评估 |

- 传感器放 core 1，避免与舵机实时任务抢占。
- I2C/OneWire 读操作可能阻塞数 ms，放在独立任务中，绝不进入 `servoCoreTask`。
- 共享数据（传感器快照、告警状态）用 `portMUX_TYPE` 保护，沿用现有 `exprMux` 风格。
- 告警到表情用 FreeRTOS 队列传递（或 `volatile` 请求标志 + 现有 `applyExpr` 请求机制）。

### 4.3 数据结构（建议）

```cpp
// 传感器种类（驱动选择）
enum SensorKind : uint8_t {
  SK_SHT3X   = 0,   // I2C 温湿度
  SK_BME280  = 1,   // I2C 温湿压
  SK_DS18B20 = 2,   // OneWire 温度
  SK_ANALOG  = 3,   // ADC1 模拟量
  SK_DIGITAL = 4,   // 开关量
  SK_MODBUS  = 5,   // 预留
};

// 物理设备
struct SensorDevice {
  const char* id;        // "env1"
  const char* name;      // "车间环境"
  SensorKind  kind;
  uint8_t     addr;      // I2C 地址 或 GPIO
  uint16_t    periodMs;  // 采样周期
  uint32_t    lastRead;
  bool        ok;        // 最近一次读取是否成功
  float       raw[4];    // 解出的物理量（温度/湿度/气压…）
  uint8_t     rawCount;
};

// 测量通道（告警以通道为单位）
struct SensorDef {
  const char* id;        // "temp1"
  const char* name;      // "车间温度"
  const char* unit;      // "°C"
  uint8_t     devIndex;  // 指向 g_devices
  uint8_t     channel;   // 该设备的第几个量
  float       warnLo, warnHi;
  float       critLo, critHi;
  uint16_t    debounceMs; // 连续越限多久才确认
  uint16_t    cooldownMs; // 同级重复告警抑制
  uint8_t     level;      // 越限定义级别 1=警告 2=严重（可被 crit 提升）
};

// 实时快照（双缓冲/加锁）
struct SensorValue {
  float    value;
  float    ema;       // 滤波值
  uint32_t ts;
  uint8_t  state;     // 0 正常 1 警告 2 严重 3 陈旧
  bool     valid;
};

// 告警等级
enum AlertLevel : uint8_t { AL_NONE=0, AL_WARN=1, AL_CRIT=2, AL_EMERGENCY=3 };

// 规则运行态
struct RuleState {
  bool     active;
  uint32_t since;      // 越限起始
  uint32_t lastFired;  // 冷却用
  uint8_t  level;
};
```

> 内存：通道数建议 ≤ 16，设备数 ≤ 8，规则表静态分配，避免动态内存。

### 4.4 驱动层接口

```cpp
// sensor.cpp
bool sensorInit();                       // 初始化 I2C、探针、继电器
void sensorTask(void*);                  // 采集主循环
bool sensorReadDevice(SensorDevice& d);  // 按 kind 分发到具体驱动

// 具体驱动（静态函数）
static bool readSHT3X(SensorDevice&);    // CRC 校验
static bool readBME280(SensorDevice&);
static bool readDS18B20(SensorDevice&);  // 需非阻塞状态机或 750ms 等待（放本任务可接受）
static bool readAnalog(SensorDevice&);
static bool readDigital(SensorDevice&);
```

- I2C 读失败不阻塞：记录 `ok=false`，由 **陈旧检测** 升级为 `state=3`（故障告警）。
- EMA 滤波系数可配（默认 α≈0.3），DS18B20 可跳过滤波。
- 读取节流：每个设备按 `periodMs` 到期才读，`sensorTask` 100ms 轮询调度。

### 4.5 规则引擎（rules.cpp）

伪代码：

```cpp
void rulesEvaluate(uint32_t now) {
  for (每个通道 c) {
    SensorValue& v = g_values[c];
    // 1. 陈旧/无效 → 视为严重故障
    if (!v.valid || now - v.ts > STALE_MS) { fire(c, AL_CRIT, "stale"); continue; }

    // 2. 判定越限级别（带回差，避免边界抖动）
    uint8_t lvl = levelFor(c, v.ema);   // 含 hysteresis

    // 3. 防抖：连续越限 debounceMs 才确认
    RuleState& r = g_ruleState[c];
    if (lvl != r.level) { r.since = now; r.level = lvl; }
    if (lvl > AL_NONE && now - r.since < c.debounceMs) continue;

    // 4. 冷却
    if (lvl > AL_NONE && now - r.lastFired < c.cooldownMs) continue;

    // 5. 状态变化才触发
    if (lvl > AL_NONE && !r.active) { fire(c, lvl, "over"); r.active = true; r.lastFired = now; }
    else if (lvl == AL_NONE && r.active) { clear(c); r.active = false; }
  }
}
```

`fire()` 统一分发：

```cpp
void fire(int ch, AlertLevel lvl, const char* reason);
```

- 更新全局最高告警等级 `g_maxLevel`。
- 记录事件日志。
- 触发执行器（见 4.7）。
- 触发表情（队列投递给舵机任务）。
- 标记载荷供网络任务上报。

### 4.6 告警状态机

```
        ┌──────────┐  越限>防抖   ┌──────────┐  越限达crit   ┌──────────┐
        │  NORMAL  │─────────────▶│   WARN   │──────────────▶│   CRIT   │
        └────┬─────┘              └────┬─────┘               └────┬─────┘
             │ 恢复                     │ 恢复                     │ 恢复(需人工/自动)
             ▼                          ▼                          ▼
          NORMAL                     NORMAL                    NORMAL
   传感器失效/急停 ─────────────────────────────────────▶ EMERGENCY（锁存，需 ack）
```

- **WARN/CRIT** 可自动恢复（值回到正常且经过回差）。
- **EMERGENCY** 锁存：需人工 `ack_alarm` 或本地复位，防止"报警一闪而过"被忽略。
- 全局 `g_maxLevel = max(所有活动告警)`。

### 4.7 与现有表情/行为层的集成

- 复用 `startSingleExpr()`（`expression.cpp:108`）触发表情，**不改舵机任务结构**。
- 建议等级→表情映射（可在 `config.h` 配置）：
  - L1 警告 → `tired`（困倦提示）
  - L2 严重 → `surprise`（惊讶）
  - L3 紧急 → `dis`（严厉）+ 可选持续闭眼表示停机
- 在 `composeOutputFrame()`（`motion.cpp:89`）中：当 `g_maxLevel >= AL_CRIT` 时，可屏蔽 `behaviorBlink`/漂移，避免松眼睑盖住告警表情；语音对话进行时不干扰。
- 告警解除后自动恢复 `init`。

### 4.8 继电器/执行器（actuator.cpp）

```cpp
void actuatorInit();
void relaySet(int ch, bool on);      // 本地立即生效
void buzzerPulse(int times);         // 非阻塞节拍
void emergencyStop(bool cut);        // 释放/吸合继电器
```

- 所有执行器操作在 `sensorTask`/规则上下文中直接完成（本地、不排队到网络任务）。
- 蜂鸣器用节拍状态机，不用 `delay`。

### 4.9 事件日志（eventlog.cpp）

- 固定 32 条环形缓冲：`{ts, level, sensorId, value, reason, action}`。
- 供 MCP `factory_status` / 上位机历史查询。
- 掉电不保存（如需可写 NVS，注意擦写寿命）。

---

## 5. 通信协议

### 5.1 MCP 工具（新增，注册于 `registerTools()`）

保持"工具数量少、描述精确"的原则，避免小智误调用。

| 工具名 | 参数 | 返回 | 用途 |
|---|---|---|---|
| `sensor_read` | `ids?: string[]` | `{ts, sensors:[{id,name,value,unit,state}]}` | 查询实时数据 |
| `sensor_history` | `id, window_ms?, limit?` | `{id,min,max,avg,last}` | 趋势/最值 |
| `factory_status` | 无 | `{uptime,wifi,mqtt,alarm_level,active_alarms[],relays}` | 总体状态 |
| `set_threshold` | `id, warn_lo?,warn_hi?,crit_lo?,crit_hi?` | 回显 | 在线改阈值 |
| `ack_alarm` | `id?` | `{ok}` | 人工确认/解锁存 |
| `set_rule_enabled` | `id, enabled` | `{ok}` | 启停规则 |
| `relay_control` | `channel, state, reason` | `{ok}` | 手动控制（需 reason，慎用） |

示例返回（`sensor_read`）：

```json
{"ts":123456,"sensors":[
  {"id":"temp1","name":"车间温度","value":31.2,"unit":"°C","state":0},
  {"id":"hum1","name":"车间湿度","value":45.0,"unit":"%RH","state":1}
]}
```

**主动上报**：库提供 public 的 `sendMessage()`（`xiaozhi-mcp/src/WebSocketMCP.h:210`）。可在告警时主动发 JSON-RPC 通知，让小智播报。**待验证**：xiaozhi.me 服务端是否会把设备主动消息转成语音；若不支持，则告警语音改由 MQTT 触发外部服务。MCP **绝不作为安全回路**。

### 5.2 MQTT 主题与载荷

现有 `BMP006`（指令）、`BMP004`（状态）语义不变。新增：

| 宏 | 建议值 | 方向 | 载荷 |
|---|---|---|---|
| `MQTT_TOPIC_TELEMETRY` | `BMP003` | 上行 | 周期快照 JSON |
| `MQTT_TOPIC_ALARM` | `BMP005` | 上行 | 告警事件 JSON |

Telemetry（周期/变化上报）：

```json
{"type":"telemetry","t":123456,"s":{"temp1":31.2,"hum1":45.0,"vib":0.12},"alarm":1}
```

Alarm（状态跳变时上报）：

```json
{"type":"alarm","t":123456,"id":"temp1","level":2,"value":58.3,"unit":"°C","reason":"over","action":"relay_off"}
```

- 巴法云同一 Client ID 主题数有限，选择空闲编号；若受限，可复用 `BMP004` 但需保证 app.py 能区分 JSON 与纯文本。
- 上报周期建议 5~10s；告警即时上报。

### 5.3 上位机（app.py / index.html）

- 订阅新增两个主题；`_handle_status` 中先判断 payload 是否为 JSON telemetry/alarm，再回退现有纯文本逻辑（保持兼容）。
- 新增 SSE 事件：`{"type":"sensors",...}`、`{"type":"alarm",...}`。
- 新增 API：
  - `GET /api/sensors` 实时快照
  - `GET /api/sensors/history?id=&window=`
  - `POST /api/sensors/threshold`
  - `GET /api/alarms`、`POST /api/alarms/ack`
- UI：仪表盘卡片（温度/湿度/状态色）、告警横幅、事件时间线、阈值表单、可选曲线（前端 ECharts 或轻量 canvas）。

---

## 6. 配置表（`config.h` 新增）

```cpp
// ==================== 控制中枢开关 ====================
#define CONTROL_HUB_ENABLED 1
#define STALE_MS            5000     // 超过该时间无有效读数 → 陈旧告警
#define TELEMETRY_PERIOD_MS 5000
#define EMA_ALPHA           0.3f

// 引脚
#define I2C_SDA_PIN   27
#define I2C_SCL_PIN   26
#define ONEWIRE_PIN   4
#define RELAY1_PIN    5
#define RELAY2_PIN    25
#define BUZZER_PIN    26

// MQTT 新主题
#define MQTT_TOPIC_TELEMETRY "BMP003"
#define MQTT_TOPIC_ALARM     "BMP005"

// 告警→表情映射
#define EXPR_WARN   "tired"
#define EXPR_CRIT   "surprise"
#define EXPR_EMERG  "dis"

// 传感器/规则表在 sensor_rules.h 中以数组形式定义，示例见下
```

`sensor_rules.h`（表驱动示例）：

```cpp
static const SensorDevice SENSOR_DEVICES[] = {
  {"env1",  "车间环境",  SK_SHT3X,   0x44, 1000, 0, false, {0}, 0},
  {"temp2", "电机温度",  SK_DS18B20, 4,    2000, 0, false, {0}, 0},
};

static const SensorDef SENSOR_CHANNELS[] = {
  {"temp1","车间温度","°C",  0,0, 10,35, 5,45, 3000,10000,1},
  {"hum1", "车间湿度","%RH", 0,1, 20,80, 10,90, 3000,10000,1},
  {"temp2","电机温度","°C",  1,0, 20,60, 10,75, 1000,10000,2},
};
```

---

## 7. 分阶段实施与验收

### P1 · 感知与对话（基础）
- 新增 `sensor.h/.cpp`、`sensor_rules.h`，接入 SHT30 + DS18B20。
- `sensorTask` 采集 + EMA + 陈旧检测，共享快照加锁。
- MCP 工具 `sensor_read` / `factory_status`。
- 验收：小智问"现在车间温度多少"能返回实时值；拔掉传感器能返回故障状态。

### P2 · 规则与本地应对
- `rules.cpp` + `actuator.cpp`：阈值、防抖、回差、冷却、分级。
- 继电器停机、蜂鸣、告警表情。
- 事件日志。
- MCP `set_threshold` / `ack_alarm`。
- 验收：加热/加湿触发 L1/L2；L2 自动释放继电器停机，蜂鸣+表情；恢复后自动回 `init`。

### P3 · 上云与可视化
- MQTT telemetry/alarm 上报；`app.py` 订阅与 API；UI 仪表盘与告警面板。
- 验收：网页实时显示数值与告警；历史可查；阈值可在线改。

### P4 · 工业扩展（可选）
- Modbus RTU（RS485）驱动、更多通道、NVS 持久化阈值、MCP 主动语音播报验证。

---

## 8. 风险与安全清单

| 风险 | 对策 |
|---|---|
| WiFi 运行时 ADC2 不可用 | 模拟量一律用 ADC1（35/36/39）或 ADS1115 |
| 2.4G 共存（WiFi/BLE）干扰 I2C 时序 | I2C 降速到 100kHz，短线+上拉 |
| 舵机/继电器电源跌落 brownout | 独立供电、光耦隔离、大电容、保留复位诊断 |
| MCP 非实时、可能断连 | 安全动作 100% 本地；MCP 仅查询/语音 |
| I2C 读阻塞影响实时任务 | 采集放独立 `sensorTask`，绝不进 `servoCoreTask` |
| 阈值抖动刷屏/继电器频繁动作 | 防抖 + 回差 + 冷却 + EMERGENCY 锁存 |
| strapping 脚误用导致无法启动 | 避开 0/2/12/15 |
| **`config.h:7` MCP token 明文入库** | 移到未提交的 `secrets.h` 或 NVS，避免泄露 |
| ESP32 内存有限 | 静态表、限制通道数、history 采样精简 |

---

## 9. 待确认问题

1. 传感器确切型号与数量（SHT30 还是 BME280？DS18B20 几路？是否要气压）？
2. 继电器负载类型与容量（是否感性、电压电流）？停机语义是"切断主电"还是"发停止信号"？
3. MQTT 上报是否可新增主题（巴法云主题额度）？还是必须复用 `BMP004`？
4. 是否需要 MCP 主动语音播报告警？需先验证 xiaozhi 服务端行为。
5. 阈值是否需要掉电保存（NVS）？
