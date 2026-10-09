#include "app.h"

// ==================== 状态读写 ====================
void setCurrentExpr(const char* e) {
  portENTER_CRITICAL(&exprMux);
  strncpy(currentExpr, e, sizeof(currentExpr) - 1);
  currentExpr[sizeof(currentExpr) - 1] = '\0';
  portEXIT_CRITICAL(&exprMux);
}

void getCurrentExpr(char* out, size_t outSize) {
  portENTER_CRITICAL(&exprMux);
  strncpy(out, currentExpr, outSize - 1);
  out[outSize - 1] = '\0';
  portEXIT_CRITICAL(&exprMux);
}

void markStatusPending(const char* e) {
  portENTER_CRITICAL(&statusMux);
  strncpy(pendingStatus, e, sizeof(pendingStatus) - 1);
  pendingStatus[sizeof(pendingStatus) - 1] = '\0';
  hasPendingStatus = true;
  portEXIT_CRITICAL(&statusMux);
}

void markFlowEvent(const String& s) {
  portENTER_CRITICAL(&flowEventMux);
  strncpy(pendingFlowEvent, s.c_str(), sizeof(pendingFlowEvent) - 1);
  pendingFlowEvent[sizeof(pendingFlowEvent) - 1] = '\0';
  hasFlowEvent = true;
  portEXIT_CRITICAL(&flowEventMux);
}

void markTelemetry(const String& s) {
  portENTER_CRITICAL(&telemetryMux);
  strncpy(pendingTelemetry, s.c_str(), sizeof(pendingTelemetry) - 1);
  pendingTelemetry[sizeof(pendingTelemetry) - 1] = '\0';
  hasTelemetry = true;
  portEXIT_CRITICAL(&telemetryMux);
}

void publishPendingStatus() {
  if (!mqttClient.connected()) return;

  char localStatus[16];
  char localFlow[32];
  char localTelemetry[128];
  bool needPublish = false;
  bool needFlow = false;
  bool needTelemetry = false;

  portENTER_CRITICAL(&statusMux);
  if (hasPendingStatus) {
    strncpy(localStatus, pendingStatus, sizeof(localStatus) - 1);
    localStatus[sizeof(localStatus) - 1] = '\0';
    hasPendingStatus = false;
    needPublish = true;
  }
  portEXIT_CRITICAL(&statusMux);

  portENTER_CRITICAL(&flowEventMux);
  if (hasFlowEvent) {
    strncpy(localFlow, pendingFlowEvent, sizeof(localFlow) - 1);
    localFlow[sizeof(localFlow) - 1] = '\0';
    hasFlowEvent = false;
    needFlow = true;
  }
  portEXIT_CRITICAL(&flowEventMux);

  portENTER_CRITICAL(&telemetryMux);
  if (hasTelemetry) {
    strncpy(localTelemetry, pendingTelemetry, sizeof(localTelemetry) - 1);
    localTelemetry[sizeof(localTelemetry) - 1] = '\0';
    hasTelemetry = false;
    needTelemetry = true;
  }
  portEXIT_CRITICAL(&telemetryMux);

  if (needPublish) {
    mqttClient.publish(MQTT_TOPIC_STATUS, localStatus);
  }
  if (needFlow) {
    mqttClient.publish(MQTT_TOPIC_STATUS, localFlow);
  }
  if (needTelemetry) {
    mqttClient.publish(MQTT_TOPIC_STATUS, localTelemetry);
  }
}

bool isValidExpr(const char* e) {
  return strcmp(e, "init") == 0 || strcmp(e, "close") == 0 ||
         strcmp(e, "dis") == 0  || strcmp(e, "surprise") == 0 ||
         strcmp(e, "tired") == 0|| strcmp(e, "smile") == 0 ||
         strcmp(e, "wink") == 0 || strcmp(e, "left_open") == 0 ||
         strcmp(e, "right_open") == 0 || strcmp(e, "blink") == 0;
}

// ==================== 表情控制 ====================
Frame getFrame() {
  char expr[16];
  getCurrentExpr(expr, sizeof(expr));

  if (strcmp(expr, "init") == 0) return INIT;
  if (strcmp(expr, "close") == 0) return CLOSE;
  if (strcmp(expr, "dis") == 0) return DIS;
  if (strcmp(expr, "surprise") == 0) return useB ? SUR_B : SUR_A;
  if (strcmp(expr, "tired") == 0) return useB ? TIRE_B : TIRE_A;
  if (strcmp(expr, "smile") == 0) return useB ? SMIL_B : SMIL_A;
  if (strcmp(expr, "wink") == 0) return useB ? WINK_B : WINK_A;
  if (strcmp(expr, "left_open") == 0) return LEFT_OPEN;
  if (strcmp(expr, "right_open") == 0) return RIGHT_OPEN;
  if (strcmp(expr, "blink") == 0) return INIT;   // 眨眼由行为层播放，基帧保持睁眼

  return INIT;
}

// 命令层只发请求，实际规划在舵机任务内执行，避免跨任务竞争
void applyExpr(const char* e) {
  if (!isValidExpr(e)) {
    Serial.println("⚠️ 未知表情，忽略：" + String(e));
    return;
  }

  setCurrentExpr(e);

  portENTER_CRITICAL(&reqMux);
  strncpy(pendingExpr, e, sizeof(pendingExpr) - 1);
  pendingExpr[sizeof(pendingExpr) - 1] = '\0';
  newExprPending = true;
  portEXIT_CRITICAL(&reqMux);

  Serial.println("✅ 执行：" + String(e));
  markStatusPending(e);
  bleNotify(String(e) + "\n");
}

// ==================== 动作所有权 / 优先级门控 ====================
static volatile uint8_t g_actionOwner = AP_IDLE;
static portMUX_TYPE ownerMux = portMUX_INITIALIZER_UNLOCKED;

void actionSetOwner(uint8_t prio) {
  portENTER_CRITICAL(&ownerMux);
  g_actionOwner = prio;
  portEXIT_CRITICAL(&ownerMux);
}

uint8_t actionGetOwner() {
  uint8_t v;
  portENTER_CRITICAL(&ownerMux);
  v = g_actionOwner;
  portEXIT_CRITICAL(&ownerMux);
  return v;
}

// 带优先级的动作请求：低优先级的外部请求会被高优先级所有者挡住
bool requestExpr(const char* e, ActionPriority prio, ActionSource src) {
  if (!isValidExpr(e)) return false;

  uint8_t cur = actionGetOwner();
  if (prio < cur && src == AS_USER) {
    Serial.println("⛔ 忙，" + String(e) + " 被忽略（当前所有者优先级 " + String(cur) + "）");
    return false;
  }

  if (prio >= cur) actionSetOwner(prio);
  applyExpr(e);
  return true;
}

void handlePendingExpr() {
  char e[16];
  bool has = false;

  portENTER_CRITICAL(&reqMux);
  if (newExprPending) {
    strncpy(e, pendingExpr, sizeof(e) - 1);
    e[sizeof(e) - 1] = '\0';
    newExprPending = false;
    has = true;
  }
  portEXIT_CRITICAL(&reqMux);

  if (!has) return;

  useB = !useB;

  if (strcmp(e, "blink") == 0) {
    // 眨眼不是静态帧，交给行为层播放一次快速闭-开
    triggerBlink();
    return;
  }

  planTo(getFrame(), PLAN_MIN_DURATION, PLAN_MAX_DURATION);
}

bool startSingleExpr(const char* e) {
  // 动作流运行中，外部单表情被拒绝（独占）
  if (flowIsRunning() || actionGetOwner() > AP_EXPR) return false;

  if (actionQueue != NULL) {
    xQueueReset(actionQueue);
  }

  sequenceRunning = false;
  nextActionTime = 0;

  return requestExpr(e, AP_EXPR, AS_USER);
}

bool startSequence(ActionItem* items, int count) {
  if (actionQueue == NULL || count <= 0) return false;

  // 动作流运行中，外部序列被拒绝（独占）
  if (flowIsRunning() || actionGetOwner() > AP_EXPR) return false;

  xQueueReset(actionQueue);

  int realCount = 0;

  for (int i = 0; i < count; i++) {
    if (!isValidExpr(items[i].expr)) continue;

    xQueueSend(actionQueue, &items[i], 0);
    realCount++;
  }

  if (realCount <= 0) return false;

  actionSetOwner(AP_EXPR);

  // 先入队，后置标志，避免舵机任务抢先读到空队列
  nextActionTime = 0;
  sequenceRunning = true;

  Serial.println("✅ 动作序列已加入队列，共 " + String(realCount) + " 个动作");
  return true;
}

void processActionQueue() {
  if (!sequenceRunning || actionQueue == NULL) return;

  unsigned long now = millis();

  if (nextActionTime != 0 && now < nextActionTime) {
    return;
  }

  ActionItem item;

  if (xQueueReceive(actionQueue, &item, 0) == pdTRUE) {
    applyExpr(item.expr);
    nextActionTime = millis() + item.holdMs;
  } else {
    sequenceRunning = false;
    nextActionTime = 0;
    Serial.println("✅ 动作序列执行完成");
  }
}

// ==================== 表情识别与映射 ====================
String toLowerCopy(String s) {
  s.toLowerCase();
  return s;
}

const char* canonicalizeExpr(String raw) {
  raw.trim();
  String s = toLowerCopy(raw);

  // 单眼动作 / 眨眼 必须优先于通用“睁眼”，否则“左睁眼”会被 init 抢走
  if (s == "left_open" || s == "left open" || s.indexOf("左睁眼") >= 0 || s.indexOf("睁左眼") >= 0 || s.indexOf("左眼睁开") >= 0) return "left_open";
  if (s == "right_open" || s == "right open" || s.indexOf("右睁眼") >= 0 || s.indexOf("睁右眼") >= 0 || s.indexOf("右眼睁开") >= 0) return "right_open";
  if (s == "blink" || s.indexOf("眨眼") >= 0 || s.indexOf("眨一下") >= 0) return "blink";
  if (s == "wink" || s.indexOf("媚眼") >= 0 || s.indexOf("抛媚眼") >= 0) return "wink";

  if (s == "init" || s.indexOf("睁眼") >= 0 || s.indexOf("正常") >= 0 || s.indexOf("初始") >= 0 || s.indexOf("复位") >= 0 || s.indexOf("重置") >= 0 || s.indexOf("恢复") >= 0) return "init";
  if (s == "close" || s.indexOf("闭眼") >= 0 || s.indexOf("睡觉") >= 0 || s.indexOf("闭上") >= 0) return "close";
  if (s == "dis" || s.indexOf("鄙视") >= 0 || s.indexOf("不屑") >= 0 || s.indexOf("嫌弃") >= 0 || s.indexOf("白眼") >= 0) return "dis";
  if (s == "surprise" || s.indexOf("惊讶") >= 0 || s.indexOf("吃惊") >= 0 || s.indexOf("震惊") >= 0) return "surprise";
  if (s == "tired" || s.indexOf("困") >= 0 || s.indexOf("累") >= 0 || s.indexOf("想睡") >= 0 || s.indexOf("疲倦") >= 0 || s.indexOf("疲惫") >= 0) return "tired";
  if (s == "smile" || s.indexOf("笑") >= 0 || s.indexOf("开心") >= 0 || s.indexOf("高兴") >= 0 || s.indexOf("微笑") >= 0) return "smile";

  return "";
}

static bool alreadyHasNear(ParsedAction* arr, int count, int pos, const char* expr) {
  for (int i = 0; i < count; i++) {
    if (arr[i].pos == pos && strcmp(arr[i].expr, expr) == 0) return true;
  }
  return false;
}

static void addTermMatches(String s, const char* term, const char* expr, ParsedAction* out, int& count, int maxCount) {
  int start = 0;
  while (count < maxCount) {
    int p = s.indexOf(term, start);
    if (p < 0) break;

    if (!alreadyHasNear(out, count, p, expr)) {
      out[count].pos = p;
      out[count].expr = expr;
      count++;
    }
    start = p + strlen(term);
  }
}

static void sortActionsByPosition(ParsedAction* arr, int count) {
  for (int i = 0; i < count - 1; i++) {
    for (int j = i + 1; j < count; j++) {
      if (arr[j].pos < arr[i].pos) {
        ParsedAction tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
      }
    }
  }
}

int parseActionsFromText(String raw, ActionItem* out, int maxCount, uint32_t holdMs) {
  raw.trim();
  String s = toLowerCopy(raw);

  ParsedAction found[20];
  int foundCount = 0;

  // 单眼动作 / 眨眼 先于通用“睁眼”匹配
  addTermMatches(s, "left_open", "left_open", found, foundCount, 20);
  addTermMatches(s, "左睁眼", "left_open", found, foundCount, 20);
  addTermMatches(s, "睁左眼", "left_open", found, foundCount, 20);
  addTermMatches(s, "左眼睁开", "left_open", found, foundCount, 20);

  addTermMatches(s, "right_open", "right_open", found, foundCount, 20);
  addTermMatches(s, "右睁眼", "right_open", found, foundCount, 20);
  addTermMatches(s, "睁右眼", "right_open", found, foundCount, 20);
  addTermMatches(s, "右眼睁开", "right_open", found, foundCount, 20);

  addTermMatches(s, "blink", "blink", found, foundCount, 20);
  addTermMatches(s, "眨眼", "blink", found, foundCount, 20);
  addTermMatches(s, "眨一下", "blink", found, foundCount, 20);

  addTermMatches(s, "init", "init", found, foundCount, 20);
  // 通用“睁眼”归 init，但“左睁眼/右睁眼”已单独识别，避免重复计入 init
  {
    int start = 0;
    while (foundCount < 20) {
      int p = s.indexOf("睁眼", start);
      if (p < 0) break;
      String prev = (p >= 3) ? s.substring(p - 3, p) : String("");
      bool singleEye = (prev == "左" || prev == "右");
      if (!singleEye && !alreadyHasNear(found, foundCount, p, "init")) {
        found[foundCount].pos = p;
        found[foundCount].expr = "init";
        foundCount++;
      }
      start = p + 2;
    }
  }
  addTermMatches(s, "正常", "init", found, foundCount, 20);
  addTermMatches(s, "初始", "init", found, foundCount, 20);
  addTermMatches(s, "复位", "init", found, foundCount, 20);
  addTermMatches(s, "重置", "init", found, foundCount, 20);
  addTermMatches(s, "恢复", "init", found, foundCount, 20);

  addTermMatches(s, "close", "close", found, foundCount, 20);
  addTermMatches(s, "闭眼", "close", found, foundCount, 20);
  addTermMatches(s, "睡觉", "close", found, foundCount, 20);
  addTermMatches(s, "闭上", "close", found, foundCount, 20);

  addTermMatches(s, "dis", "dis", found, foundCount, 20);
  addTermMatches(s, "鄙视", "dis", found, foundCount, 20);
  addTermMatches(s, "不屑", "dis", found, foundCount, 20);
  addTermMatches(s, "嫌弃", "dis", found, foundCount, 20);
  addTermMatches(s, "白眼", "dis", found, foundCount, 20);

  addTermMatches(s, "surprise", "surprise", found, foundCount, 20);
  addTermMatches(s, "惊讶", "surprise", found, foundCount, 20);
  addTermMatches(s, "吃惊", "surprise", found, foundCount, 20);
  addTermMatches(s, "震惊", "surprise", found, foundCount, 20);

  addTermMatches(s, "tired", "tired", found, foundCount, 20);
  addTermMatches(s, "疲倦", "tired", found, foundCount, 20);
  addTermMatches(s, "疲惫", "tired", found, foundCount, 20);
  addTermMatches(s, "想睡", "tired", found, foundCount, 20);
  addTermMatches(s, "困", "tired", found, foundCount, 20);
  addTermMatches(s, "累", "tired", found, foundCount, 20);

  addTermMatches(s, "smile", "smile", found, foundCount, 20);
  addTermMatches(s, "微笑", "smile", found, foundCount, 20);
  addTermMatches(s, "开心", "smile", found, foundCount, 20);
  addTermMatches(s, "高兴", "smile", found, foundCount, 20);
  addTermMatches(s, "笑", "smile", found, foundCount, 20);

  addTermMatches(s, "wink", "wink", found, foundCount, 20);
  addTermMatches(s, "媚眼", "wink", found, foundCount, 20);
  addTermMatches(s, "抛媚眼", "wink", found, foundCount, 20);

  if (foundCount == 0) {
    const char* one = canonicalizeExpr(raw);
    if (strlen(one) == 0) return 0;

    strncpy(out[0].expr, one, sizeof(out[0].expr) - 1);
    out[0].expr[sizeof(out[0].expr) - 1] = '\0';
    out[0].holdMs = holdMs;
    return 1;
  }

  sortActionsByPosition(found, foundCount);

  int outCount = 0;
  const char* lastExpr = "";

  for (int i = 0; i < foundCount && outCount < maxCount; i++) {
    if (strcmp(found[i].expr, lastExpr) == 0) continue;

    strncpy(out[outCount].expr, found[i].expr, sizeof(out[outCount].expr) - 1);
    out[outCount].expr[sizeof(out[outCount].expr) - 1] = '\0';
    out[outCount].holdMs = holdMs;

    lastExpr = found[i].expr;
    outCount++;
  }

  return outCount;
}

// ==================== 动作流名称解析（中英文别名 → 流程名） ====================
String resolveFlowName(String raw) {
  raw.trim();
  String s = toLowerCopy(raw);
  if (s.length() == 0) return "";

  if (s == "selftest" || s.indexOf("自检") >= 0) return "selftest";
  if (s == "greet" || s.indexOf("打招呼") >= 0 || s.indexOf("问候") >= 0 || s.indexOf("欢迎") >= 0) return "greet";
  if (s == "alert" || s.indexOf("告警") >= 0 || s.indexOf("报警") >= 0 || s.indexOf("警报") >= 0) return "alert";
  if (s == "sleep" || s.indexOf("休眠") >= 0 || s.indexOf("睡觉") >= 0 || s.indexOf("睡眠") >= 0) return "sleep";
  if (s == "wake" || s.indexOf("唤醒") >= 0) return "wake";
  if (s == "scan" || s.indexOf("巡视") >= 0 || s.indexOf("扫描") >= 0 || s.indexOf("扫视") >= 0) return "scan";
  if (s == "demo" || s.indexOf("演示") >= 0) return "demo";

  for (int i = 0; i < flowCount(); i++) {
    if (s == String(flowNameAt(i))) return String(flowNameAt(i));
  }
  return "";
}

// ==================== 统一指令分发（MQTT / 蓝牙共用） ====================
void dispatchCommand(const String& cmd) {
  String c = cmd;
  c.trim();
  String lc = toLowerCopy(c);

  // ---- 待机微动开关（原“动作流”，已改名避免与新动作流混淆） ----
  if (lc == "behavior on" || lc == "micro on" || lc == "behavior_on" || lc == "micro_on" ||
      c.indexOf("微动开") >= 0 || c.indexOf("开启微动") >= 0 || c.indexOf("打开微动") >= 0 ||
      c.indexOf("开启待机微动") >= 0) {
    setBehaviorEnabled(true);
    markStatusPending("behavior_on");
    bleNotify("behavior_on\n");
    Serial.println("✅ 微动已开启");
    return;
  }
  if (lc == "behavior off" || lc == "micro off" || lc == "behavior_off" || lc == "micro_off" ||
      c.indexOf("微动关") >= 0 || c.indexOf("关闭微动") >= 0 || c.indexOf("停止微动") >= 0 ||
      c.indexOf("关闭待机微动") >= 0) {
    setBehaviorEnabled(false);
    markStatusPending("behavior_off");
    bleNotify("behavior_off\n");
    Serial.println("✅ 微动已关闭");
    return;
  }

  // ---- 继电器 / 温度联动 ----
  if (lc.startsWith("relay") || c.indexOf("继电器") >= 0 ||
      c.indexOf("温控") >= 0 || c.indexOf("温度阈值") >= 0) {
    // 自动温控开关
    if (lc == "relay auto on" || lc == "relay_auto_on" ||
        c.indexOf("开启自动温控") >= 0 || c.indexOf("打开自动温控") >= 0) {
      relaySetAuto(true);
      bleNotify("relay:auto:on\n");
      relayAnnounce();
      return;
    }
    if (lc == "relay auto off" || lc == "relay_auto_off" ||
        c.indexOf("关闭自动温控") >= 0 || c.indexOf("停止自动温控") >= 0) {
      relaySetAuto(false);
      bleNotify("relay:auto:off\n");
      relayAnnounce();
      return;
    }

    // 温度阈值
    if (lc.startsWith("relay threshold") || c.indexOf("温度阈值") >= 0) {
      String num = c;
      int k = num.indexOf("阈值");
      if (k >= 0) {
        num = num.substring(k + 2);
      } else {
        num = c.substring(strlen("relay threshold"));
      }
      float v = num.toFloat();
      if (v > 0.0f) {
        relaySetThreshold(v);
        bleNotify("relay:threshold:" + String(v, 1) + "\n");
        relayAnnounce();
      } else {
        bleNotify("relay:threshold:invalid\n");
      }
      return;
    }

    // 手动开 / 关
    if (lc == "relay on" || lc == "relay_on" ||
        c.indexOf("打开继电器") >= 0 || c.indexOf("开启继电器") >= 0 || c.indexOf("继电器开") >= 0) {
      relaySet(true, "manual_on");
      bleNotify("relay:on:manual\n");
      relayAnnounce();
      return;
    }
    if (lc == "relay off" || lc == "relay_off" ||
        c.indexOf("关闭继电器") >= 0 || c.indexOf("断开继电器") >= 0 || c.indexOf("继电器关") >= 0) {
      relaySet(false, "manual_off");
      bleNotify("relay:off:manual\n");
      relayAnnounce();
      return;
    }
    if (lc == "relay toggle") {
      bool target = !relayGet();
      relaySet(target, target ? "manual_on" : "manual_off");
      bleNotify(target ? "relay:on:manual\n" : "relay:off:manual\n");
      relayAnnounce();
      return;
    }

    // 默认：查询状态
    String st = relayStatusJson();
    Serial.println(st);
    bleNotify(st + "\n");
    return;
  }

  // ---- 动作流（程序） ----
  if (lc.startsWith("flow") || c.indexOf("动作流") >= 0) {
    if (lc == "flow list" || lc == "flow_list" ||
        c.indexOf("动作流列表") >= 0 || c.indexOf("列出动作流") >= 0) {
      String list = "flows:";
      for (int i = 0; i < flowCount(); i++) {
        if (i) list += ",";
        list += flowNameAt(i);
      }
      Serial.println(list);
      bleNotify(list + "\n");
      return;
    }

    if (lc == "flow status" || lc == "flow_status" || c.indexOf("动作流状态") >= 0) {
      String st = "flow_status:";
      st += flowIsRunning() ? flowCurrentName() : "idle";
      st += ":";
      st += String(flowProgress());
      Serial.println(st);
      bleNotify(st + "\n");
      return;
    }

    if (lc == "flow stop" || lc == "flow_stop" ||
        c.indexOf("停止动作流") >= 0 || c.indexOf("结束动作流") >= 0) {
      flowStop(true);
      Serial.println("✅ 请求停止动作流");
      bleNotify("flow:stopped\n");
      return;
    }

    String name = "";
    if (lc.startsWith("flow start ")) {
      name = c.substring(strlen("flow start "));
    } else if (lc.startsWith("flow start")) {
      name = c.substring(strlen("flow start"));
    } else {
      int k = c.indexOf("动作流");
      if (k >= 0) {
        name = c.substring(k + String("动作流").length());
      } else if (lc.startsWith("flow ")) {
        name = c.substring(strlen("flow "));
      }
    }
    name.trim();

    String resolved = resolveFlowName(name);
    if (resolved.length() == 0) {
      Serial.println("⚠️ 未知动作流：" + name);
      bleNotify("flow:unknown\n");
      markFlowEvent("flow|unknown|error");
      return;
    }

    if (flowStart(resolved.c_str())) {
      Serial.println("✅ 动作流已启动：" + resolved);
      bleNotify("flow:" + resolved + ":start\n");
    } else {
      Serial.println("⛔ 动作流启动失败（忙或未知）：" + resolved);
      bleNotify("flow:busy\n");
      markFlowEvent("flow|busy|error");
    }
    return;
  }

  ActionItem items[8];
  int count = parseActionsFromText(c, items, 8, DEFAULT_ACTION_HOLD_MS);

  if (count <= 0) {
    Serial.println("⚠️ 无法识别指令：" + c);
    return;
  }

  if (count == 1) {
    if (!startSingleExpr(items[0].expr)) {
      Serial.println("⛔ 动作流运行中，忽略单表情：" + String(items[0].expr));
    }
  } else {
    if (!startSequence(items, count)) {
      Serial.println("⛔ 动作流运行中，忽略动作序列");
    }
  }
}
