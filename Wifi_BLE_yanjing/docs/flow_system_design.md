# 仿生眼 · 动作流（Flow）系统 详细设计文档

版本：v0.1（设计稿，待评审）
关联工程：`D:\Arduino\Wifi_BLE_yanjing`
关联文档：`docs/control_hub_design.md`
目标平台：ESP32（Arduino 框架，FreeRTOS 双核）

---

## 1. 背景与目标

现有工程里被称作"动作流"的功能，实际是**待机微动**（眨眼 / 微扫视 / 漂移，见 `behavior.cpp`，开关由 `behavior on/off` 控制）。本设计要新增的是另一类东西：**有步骤、可循环、开始前先初始化、运行中独占控制权的"动作流 / 程序（Flow）"**。

目标：

1. **流程开始先初始化**：先回到 `init` 再执行步骤，保证确定的起始姿态。
2. **运行中独占**：流程运行期间，外部（MQTT / BLE / MCP / 单表情按钮）不能再触发其他动作，被拒绝并回 `busy`。
3. **预置多条流程**：表驱动、静态数组，新增流程 = 加一行。
4. **安全例外**：告警可抢占流程；摇杆 / 长按键可人工中断流程，避免"锁死"。

设计原则：不破坏现有表情、微动、手动模式、WiFi/BLE/MQTT/MCP 功能；沿用"命令层只发请求、舵机任务内统一执行"的现有模式（`expression.cpp:70` / `control.cpp:190`）。

---

## 2. 命名调整（消除歧义）

| 概念 | 原名称 | 新名称 | 命令 |
|---|---|---|---|
| 待机微动（眨眼/微扫视/漂移） | 动作流 | **微动 / 待机微动** | `behavior on/off`，新增 `micro on/off` 别名；中文"微动开/关" |
| 有步骤的独占程序 | — | **动作流 / 程序（Flow）** | `flow start <name>` / `flow stop` / `flow list` / `flow status` |

改动点：

- `config.h`：`BEHAVIOR_ENABLED` 注释改为"待机微动"。
- `expression.cpp:303-321` 的 `dispatchCommand`：`behavior on/off` 保留；移除其中把 `flow on/off` 当作微动的别名（`flow` 让给新系统）；中文"动作流开/关"改为"微动开/关"。
- `app.py`：现有 `BEHAVIOR_CMD_ON/OFF` 不变（`behavior on/off`），仅 UI 文案改"微动"。
- `index.html`：`behaviorBtn` 文案"🌀 动作流：开"→"🌀 微动：开"。

> 兼容：为不破坏老上位机，`behavior on/off`、`behavior_on/off` 继续有效。

---

## 3. 优先级与抢占模型

引入统一优先级，避免流程与告警/手动互相打架：

| 级别 | 名称 | 来源 | 说明 |
|---|---|---|---|
| P4 | EMERGENCY | 急停、L3 紧急、安全回路 | 最高，可抢占一切；锁存直到 ack |
| P3 | ALARM | L1/L2 告警表情 | 可抢占 P2 流程 |
| P2 | FLOW | 动作流 | 独占；屏蔽 P1 外部单动作 |
| P1 | EXPR | 单表情 / 临时序列 | 默认级 |
| P0 | IDLE | 待机微动 | 始终与其他层叠加，不占所有权 |

规则：

- 同一时刻只有一个 **action owner**（P1~P4 中的一个）。
- 请求优先级 ≥ 当前 owner 才可抢占；否则：
  - 外部请求（`AS_USER`）→ **拒绝**，回 `busy`。
  - 流程内部步骤（`AS_FLOW`）→ 正常执行。
- **摇杆移动 / 按键**（`AS_MANUAL`）：可中断 P1/P2（停流程、清所有权）；**不中断 P3/P4**（告警优先）。
- 抢占后是否恢复原流程：默认**不恢复**，回 `init`（可配）。

---

## 4. 数据结构（静态表，零动态内存）

```cpp
// flow.h
enum FlowStepType : uint8_t {
  FS_INIT = 0,   // 回到初始帧
  FS_EXPR,       // 播放表情并保持 holdMs
  FS_WAIT,       // 纯等待 holdMs
  FS_POSE,       // 眼球直接定位 x,y + 保持
};

struct FlowStep {
  uint8_t  type;
  char     expr[16];   // FS_EXPR 使用
  float    x, y;       // FS_POSE 使用
  uint16_t holdMs;     // 保持时间
};

struct FlowDef {
  const char*     name;      // "greet"
  const FlowStep* steps;
  uint8_t         count;     // 步骤数 ≤ 16
  uint8_t         repeat;    // 循环次数，0/1 = 一次
  bool            autoInit;  // 开始前先 init（需求 1）
  bool            blockIdle; // 运行期间压制待机微动
  bool            returnInit;// 结束后回 init
};

enum FlowState : uint8_t { FLOW_IDLE, FLOW_PRE_INIT, FLOW_RUNNING, FLOW_DONE };

struct FlowRuntime {
  FlowState       state;
  const FlowDef*  def;
  uint8_t         stepIndex;
  uint8_t         loopCount;
  unsigned long   stepStart;
  unsigned long   flowStart;
};
```

配置示例（`flows.h`，静态数组）：

```cpp
static const FlowStep GREET_STEPS[] = {
  {FS_EXPR, "smile",     0, 0, 1500},
  {FS_EXPR, "wink",      0, 0, 1200},
  {FS_EXPR, "left_open", 0, 0, 1000},
  {FS_EXPR, "right_open",0, 0, 1000},
  {FS_EXPR, "init",      0, 0, 800},
};

static const FlowDef FLOW_TABLE[] = {
  {"greet",   GREET_STEPS, 5, 1, true, true, true},
  // ... 其他流程
};
static const uint8_t FLOW_COUNT = sizeof(FLOW_TABLE) / sizeof(FLOW_TABLE[0]);
```

内存估算：单条流程 ≤ 16 步 × ~28B ≈ 448B，全部预置流程（约 7 条）< 3KB，静态存储。

---

## 5. 运行时状态机

```
            flowStart(name)
                 │
                 ▼
        ┌────────────────┐  autoInit=true   ┌──────────────┐
        │   FLOW_IDLE    │─────────────────▶│ FLOW_PRE_INIT │
        └────────────────┘                  └──────┬───────┘
                 ▲                                 │ init 规划完成
                 │                                 ▼
                 │                          ┌──────────────┐
                 │        flowStop / 抢占   │ FLOW_RUNNING │
                 ├──────────────────────────└──────┬───────┘
                 │                                 │ 步骤耗尽
                 │                                 ▼
                 │                          ┌──────────────┐
                 └──────────────────────────│  FLOW_DONE   │
                    returnInit → init       └──────────────┘
```

超时保护：

- 单步最长 `FLOW_STEP_TIMEOUT_MS`（默认 8000），超时强制下一步，防止卡死。
- 整流程最长 `FLOW_MAX_DURATION_MS`（默认 60000），到达即结束。

---

## 6. 执行引擎 `flow.cpp`

**在 `servoCoreTask` 内驱动**（与现有 `processActionQueue` 同一上下文，单线程，无竞争）。每 tick 调用顺序（`control.cpp:221-225`）：

```cpp
processActionQueue();
handlePendingExpr();
flowUpdate(now);          // 新增
updatePlanner(now);
behaviorUpdate(now);
servoWriteIfChanged(composeOutputFrame());
```

核心接口：

```cpp
void flowInit();
bool flowStart(const char* name);      // 校验名字、抢占 P2
void flowStop(bool gotoInit);          // 停止并释放所有权
void flowUpdate(unsigned long now);
bool flowIsRunning();
const char* flowCurrentName();
int  flowProgress();                   // 0..100，供 UI
```

`flowUpdate` 逻辑：

```cpp
switch (rt.state) {
  case FLOW_PRE_INIT:
     planTo(INIT, PLAN_MIN_DURATION, PLAN_MAX_DURATION);
     if (!planActive) { rt.state = FLOW_RUNNING; rt.stepIndex = 0; rt.stepStart = now; }
     break;
  case FLOW_RUNNING:
     if (now - rt.stepStart >= stepHold(cur)) { advanceStep(now); }
     break;
  case FLOW_DONE:
     if (def->returnInit) planTo(INIT, ...);
     releaseOwner();
     rt.state = FLOW_IDLE;
     break;
}
```

`advanceStep` 检测 `planActive`，保证姿态到位再进入下一步（不硬切）。

---

## 7. 与现有模块的集成

### 7.1 统一动作入口

把现在分散的 `applyExpr`（`expression.cpp:70`）收敛为带来源/优先级的请求：

```cpp
enum ActionPriority : uint8_t { AP_IDLE=0, AP_EXPR=1, AP_FLOW=2, AP_ALARM=3, AP_EMERGENCY=4 };
enum ActionSource   : uint8_t { AS_USER, AS_FLOW, AS_ALARM, AS_SYSTEM, AS_MANUAL };

bool requestExpr(const char* expr, ActionPriority prio, ActionSource src);
```

- `applyExpr(e)` → `requestExpr(e, AP_EXPR, AS_USER)`（兼容旧调用）。
- 流程内部 → `requestExpr(e, AP_FLOW, AS_FLOW)`，绕过独占检查。
- 告警（来自 `control_hub_design.md` 的规则引擎）→ `requestExpr(expr, AP_ALARM, AS_ALARM)`，可抢占流程并回 `busy` 给流程。

### 7.2 现有序列 (`startSequence` / `actionQueue`)

建议将 `actionQueue` 的执行逻辑并入 flow 引擎，`eye_expression_sequence` 作为**临时非独占流程**（`exclusive=false`）复用同一套步骤执行；这样只有"唯一执行者"，不会两套队列互相踩。过渡期也可先并存，用 `owner` 互斥。

### 7.3 待机微动

- 流程 `blockIdle=true` 时，在 `flowUpdate` 前后设置抑制标志；`composeOutputFrame()`（`motion.cpp:89`）中当抑制生效则不叠加 `behaviorBlink`/漂移，避免眨眼盖住刻意的眼皮动作。
- `blockIdle=false`（如 `sleep`）仍保留微动，显得"活着"。

### 7.4 手动模式

- `control.cpp:213-219` 的离线手动分支：检测到摇杆移动 / 按键变化 / 电位器变化时，调用 `flowStop(false)` 并释放所有权，实现"摇杆可中断"。
- 在线时使用摇杆？当前逻辑是"断连才手动"。保持现状；若未来在线也响应摇杆，则同样触发 `flowStop`。

### 7.5 断连处理

- 运行中 WiFi/BLE 掉线：按 `config.h` 的 `FLOW_ABORT_ON_DISCONNECT`（默认 1）中断流程并回 `init`；置 0 则继续本地跑完。

---

## 8. 命令与协议

### 8.1 MQTT / BLE（`dispatchCommand` 增补）

| 命令 | 说明 | 回执 |
|---|---|---|
| `flow start <name>` | 启动流程 | `flow:<name>:running` / `flow:busy` / `flow:unknown` |
| `flow stop` | 停止流程 | `flow:stopped` |
| `flow list` | 列出流程名 | 逗号分隔 |
| `flow status` | 查询状态 | `flow:<name>:<state>:<progress>` |
| `micro on/off` | 微动开关（新名） | `micro_on/off` |

中文可用："运行动作流 打招呼" / "停止动作流"。

### 8.2 MCP 工具（`mcp_tools.cpp` 新增）

| 工具 | 参数 | 返回 |
|---|---|---|
| `flow_run` | `name`（enum 取全部流程名） | `{status, name}` |
| `flow_stop` | 无 | `{status}` |
| `flow_list` | 无 | `{flows:[...]}` |
| `flow_status` | 无 | `{running, name, progress}` |

小智："做个打招呼的动作" → `flow_run{name:"greet"}`。流程运行中调用 `eye_expression` 应返回 `{"status":"busy"}`。

### 8.3 上位机（`upper_computer/`）

- `app.py`：`KNOWN_FLOWS` 列表；`POST /api/flow/start`、`POST /api/flow/stop`、`GET /api/flow/list`；SSE 新增 `{"type":"flow", name, running, progress}`。
- `index.html`：新增"动作流"面板（下拉 + 启动/停止 + 进度）；把原"动作流：开/关"按钮改文案为"微动：开/关"；流程运行时禁用单表情按钮。

---

## 9. 预置流程

| 名称 | 步骤 | 循环 | autoInit | blockIdle | returnInit |
|---|---|---|---|---|---|
| `selftest` 自检 | init → close → init → surprise → smile → init | 1 | true | true | true |
| `greet` 打招呼 | init → smile → wink → left_open → right_open → init | 1 | true | true | true |
| `alert` 告警 | init → surprise（快速）×3 | 1 | true | true | false |
| `sleep` 休眠 | tired → close | 1 | true | false | false |
| `wake` 唤醒 | close → tired → init → smile | 1 | true | true | true |
| `scan` 巡视 | init → pose(左) → pose(右) → pose(中) → init | 1 | true | true | true |
| `demo` 演示 | init → close → dis → surprise → tired → smile → wink → left_open → right_open → blink → init | 1 | true | true | true |

`pose` 的 X/Y 取值参考 `config.h` 的 `XARM_LEFT/RIGHT`、`YARM_TOP/BOTTOM`。

---

## 10. 与工厂控制中枢的衔接

- 规则引擎（`control_hub_design.md` §4.5）触发 L1/L2 时，用 `AP_ALARM` 抢占当前流程，播放告警表情；L3 用 `AP_EMERGENCY`。
- 告警结束（人工 ack）后可选：恢复被抢占前的流程（`FLOW_RESUME_AFTER_ALARM`，默认 0 = 回 init）。
- 可新增"巡检流程"由定时/传感触发，与动作流共用引擎。

---

## 11. 新增配置项（`config.h`）

```cpp
#define FLOW_ENABLED                1
#define FLOW_MAX_STEPS              16
#define FLOW_STEP_TIMEOUT_MS        8000
#define FLOW_MAX_DURATION_MS        60000
#define FLOW_ABORT_ON_DISCONNECT    1
#define FLOW_RESUME_AFTER_ALARM     0
#define FLOW_PRE_INIT_DELAY_MS      300   // autoInit 后额外稳定时间
```

---

## 12. 分阶段实施与验收

### P1 · 引擎与基础
- `flow.h/.cpp/.flows.h`、静态流程表、`flowUpdate` 接入 `servoCoreTask`。
- 命令 `flow start/stop/list/status`。
- MCP `flow_run/flow_stop/flow_list/flow_status`。
- 验收：MQTT 发 `flow start greet` 能完整走完并回执；运行中发 `smile` 返回 `busy`。

### P2 · 集成与抢占
- 统一 `requestExpr` 优先级；微动抑制 `blockIdle`；摇杆中断；断连处理。
- 验收：`greet` 运行中摇杆一动立即停止并回手动；微动被正确压制。

### P3 · UI 与告警联动
- `upper_computer` 面板、进度、按钮禁用；命名从"动作流"改"微动"。
- 告警抢占流程。
- 验收：网页可选流程、看进度；告警能打断流程。

---

## 13. 风险与注意事项

| 风险 | 对策 |
|---|---|
| 流程把眼睛锁死 | 优先级抢占 + 摇杆中断 + 单步/整流程超时 |
| 命名迁移影响老上位机 | 保留 `behavior on/off` 兼容别名 |
| 抢占后姿态突兀 | 抢占也走 `planTo` 平滑过渡，不硬切 |
| 双执行者（队列 vs 流程）互相踩 | 统一为唯一 owner；序列并入 flow 引擎 |
| 内存 | 静态表，步骤上限 16，流程上限约 8 |
| 断连时长期占屏 | `FLOW_ABORT_ON_DISCONNECT` 默认中断回 init |
| `pose` 越界损坏机械 | 复用 `config.h` 的眼球行程常量并做椭圆限幅 |

---

## 14. 待确认

1. 预置流程清单是否照上表？是否要增减（如"巡检""欢迎"等）。
2. `repeat` 循环是否需要（如 `demo` 循环播放）？还是全部只跑一次。
3. 抢占后是否需要"恢复原流程"（`FLOW_RESUME_AFTER_ALARM`）。
4. 在线时摇杆是否也要能中断流程（当前只有离线手动生效）。
5. `eye_expression_sequence` 是否本轮就并入 flow 引擎，还是先并存。
