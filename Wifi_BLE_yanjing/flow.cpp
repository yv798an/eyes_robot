#include "app.h"
#include "flows.h"

// ==================== 动作流引擎 ====================
// 单线程执行：flowStart/flowStop 只投递请求，实际状态推进全部在 servoCoreTask
// 内通过 flowUpdate() 完成，避免跨任务竞争。

#if FLOW_ENABLED

enum FlowCmd : uint8_t { FCMD_NONE = 0, FCMD_START, FCMD_STOP };
enum : uint8_t { FST_IDLE = 0, FST_PRE_INIT, FST_RUNNING };

static volatile FlowCmd s_cmd = FCMD_NONE;
static char            s_cmdName[16] = {0};
static volatile bool   s_cmdGotoInit = true;
static portMUX_TYPE    s_cmdMux = portMUX_INITIALIZER_UNLOCKED;

static const FlowDef*  s_def = nullptr;
static char            s_name[16] = {0};
static uint8_t         s_state = FST_IDLE;
static uint8_t         s_step = 0;
static uint8_t         s_loop = 0;
static unsigned long   s_stepStart = 0;
static unsigned long   s_flowStart = 0;
static bool            s_stepIssued = false;
static volatile bool   s_running = false;
static bool            s_prevBehavior = true;

// ==================== 工具 ====================
static const FlowDef* findFlow(const char* name) {
  for (uint8_t i = 0; i < FLOW_TABLE_COUNT; i++) {
    if (strcmp(FLOW_TABLE[i].name, name) == 0) return &FLOW_TABLE[i];
  }
  return nullptr;
}

static uint8_t effectiveRepeat(const FlowDef* d) {
  return d->repeat == 0 ? 1 : d->repeat;
}

void flowInit() {
  s_cmd = FCMD_NONE;
  s_def = nullptr;
  s_name[0] = '\0';
  s_state = FST_IDLE;
  s_step = s_loop = 0;
  s_stepIssued = false;
  s_running = false;
}

int flowCount() { return FLOW_TABLE_COUNT; }

const char* flowNameAt(int i) {
  if (i < 0 || i >= (int)FLOW_TABLE_COUNT) return "";
  return FLOW_TABLE[i].name;
}

bool flowIsRunning() { return s_running; }

const char* flowCurrentName() { return s_name; }

int flowProgress() {
  if (!s_running || s_def == nullptr) return 0;
  int total = s_def->count * effectiveRepeat(s_def);
  if (total <= 0) return 0;
  int done = s_loop * s_def->count + s_step;
  if (done > total) done = total;
  return (done * 100) / total;
}

// ==================== 命令层请求 ====================
bool flowStart(const char* name) {
  if (name == nullptr || name[0] == '\0') return false;
  if (s_running) return false;
  if (actionGetOwner() > AP_FLOW) return false;
  if (findFlow(name) == nullptr) return false;

  portENTER_CRITICAL(&s_cmdMux);
  strncpy(s_cmdName, name, sizeof(s_cmdName) - 1);
  s_cmdName[sizeof(s_cmdName) - 1] = '\0';
  s_cmd = FCMD_START;
  portEXIT_CRITICAL(&s_cmdMux);
  return true;
}

void flowStop(bool gotoInit) {
  portENTER_CRITICAL(&s_cmdMux);
  s_cmd = FCMD_STOP;
  s_cmdGotoInit = gotoInit;
  portEXIT_CRITICAL(&s_cmdMux);
}

// ==================== 引擎内部 ====================
static void releaseFlow(bool gotoInit) {
  if (!s_running) return;

  if (s_def != nullptr && s_def->blockIdle) {
    setBehaviorEnabled(s_prevBehavior);
  }

  actionSetOwner(AP_IDLE);

  if (gotoInit) {
    planTo(INIT, PLAN_MIN_DURATION, PLAN_MAX_DURATION);
  }

  s_running = false;
  s_def = nullptr;
  s_state = FST_IDLE;
  s_step = s_loop = 0;
  s_stepIssued = false;
  s_name[0] = '\0';
}

static void issueStep() {
  const FlowStep& st = s_def->steps[s_step];

  switch (st.type) {
    case FS_INIT:
      requestExpr("init", AP_FLOW, AS_FLOW);
      break;
    case FS_EXPR:
      requestExpr(st.expr, AP_FLOW, AS_FLOW);
      break;
    case FS_POSE: {
      Frame t = planCurrent;
      t.x = st.x;
      t.y = st.y;
      planTo(t, PLAN_MIN_DURATION, PLAN_MAX_DURATION);
      break;
    }
    case FS_WAIT:
    default:
      break;
  }

  s_stepIssued = true;
}

static void enterStep(unsigned long now) {
  s_stepStart = now;
  s_stepIssued = false;
}

static void finishFlow() {
  char nm[16];
  strncpy(nm, s_name, sizeof(nm) - 1);
  nm[sizeof(nm) - 1] = '\0';

  bool ri = (s_def != nullptr) ? s_def->returnInit : true;
  releaseFlow(ri);

  Serial.println("✅ 动作流完成：" + String(nm));
  markFlowEvent(String("flow|") + nm + "|done");
}

static void advanceStep(unsigned long now) {
  s_step++;

  if (s_step >= s_def->count) {
    s_loop++;
    if (s_loop < effectiveRepeat(s_def)) {
      s_step = 0;
    } else {
      finishFlow();
      return;
    }
  }

  enterStep(now);
}

static void startNow(const char* name, unsigned long now) {
  if (s_running) return;
  if (actionGetOwner() > AP_FLOW) return;

  const FlowDef* d = findFlow(name);
  if (d == nullptr) return;

  // 抢占前先清掉旧的临时序列，保证唯一执行者
  if (actionQueue != NULL) xQueueReset(actionQueue);
  sequenceRunning = false;
  nextActionTime = 0;

  s_def = d;
  strncpy(s_name, d->name, sizeof(s_name) - 1);
  s_name[sizeof(s_name) - 1] = '\0';
  s_step = 0;
  s_loop = 0;
  s_stepIssued = false;
  s_flowStart = now;          // 必须用循环已取的 now，避免与 millis() 产生无符号下溢

  s_prevBehavior = behaviorEnabled;
  if (d->blockIdle) setBehaviorEnabled(false);

  actionSetOwner(AP_FLOW);
  s_running = true;

  if (d->autoInit) {
    s_state = FST_PRE_INIT;
    planTo(INIT, PLAN_MIN_DURATION, PLAN_MAX_DURATION);
  } else {
    s_state = FST_RUNNING;
    enterStep(now);
  }

  Serial.println("▶ 动作流开始：" + String(s_name) + (d->autoInit ? "（先初始化）" : ""));
  markFlowEvent(String("flow|") + s_name + "|run");
}

// ==================== 主更新（舵机任务内） ====================
void flowUpdate(unsigned long now) {
  // ---- 消费命令 ----
  FlowCmd cmd;
  char nm[16];
  bool gi;

  portENTER_CRITICAL(&s_cmdMux);
  cmd = s_cmd;
  s_cmd = FCMD_NONE;
  strncpy(nm, s_cmdName, sizeof(nm) - 1);
  nm[sizeof(nm) - 1] = '\0';
  gi = s_cmdGotoInit;
  portEXIT_CRITICAL(&s_cmdMux);

  if (cmd == FCMD_START) {
    startNow(nm, now);
  } else if (cmd == FCMD_STOP) {
    if (s_running) {
      char cur[16];
      strncpy(cur, s_name, sizeof(cur) - 1);
      cur[sizeof(cur) - 1] = '\0';
      releaseFlow(gi);
      Serial.println("⏹ 动作流停止：" + String(cur));
      markFlowEvent(String("flow|") + cur + "|stop");
    }
  }

  if (!s_running) return;

  // ---- 整流程超时保护（带符号差值，防止回绕误判） ----
  if ((long)(now - s_flowStart) > FLOW_MAX_DURATION_MS) {
    Serial.println("⚠️ 动作流超时，强制结束");
    finishFlow();
    return;
  }

  // ---- 状态推进 ----
  if (s_state == FST_PRE_INIT) {
    if (!planActive) {
      s_state = FST_RUNNING;
      enterStep(now);
    }
    return;
  }

  if (s_state != FST_RUNNING) return;

  if (!s_stepIssued) {
    issueStep();
  }

  const FlowStep& st = s_def->steps[s_step];
  unsigned long elapsed = now - s_stepStart;

  // 单步超时保护
  if (elapsed >= FLOW_STEP_TIMEOUT_MS) {
    advanceStep(now);
    return;
  }

  // 等待保持时间 + 姿态到位
  if (elapsed >= st.holdMs) {
    if (st.type == FS_WAIT || !planActive) {
      advanceStep(now);
    }
  }
}

#else  // FLOW_ENABLED == 0

void flowInit() {}
bool flowStart(const char*) { return false; }
void flowStop(bool) {}
void flowUpdate(unsigned long) {}
bool flowIsRunning() { return false; }
const char* flowCurrentName() { return ""; }
int flowProgress() { return 0; }
int flowCount() { return 0; }
const char* flowNameAt(int) { return ""; }

#endif // FLOW_ENABLED
