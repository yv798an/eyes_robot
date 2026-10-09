#include "app.h"
#include "relay.h"

// ==================== 继电器状态 ====================
// 输出：RELAY_ACTIVE_HIGH ? 高=吸合 : 低=吸合
// 控制源：自动温控 / 人工
static bool        s_on = false;
static bool        s_auto = (RELAY_TEMP_AUTO_ENABLED != 0);
static bool        s_forceOn = false;    // 人工开启优先，保持到人工关闭
static bool        s_forceOff = false;   // 人工关闭优先，降温到复归点后解除
static const char* s_reason = "boot";
static float       s_threshold = RELAY_TEMP_THRESHOLD;
static uint32_t    s_firstOverMs = 0;    // 连续超阈起始（0=未超阈）
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static inline int relayLevel(bool on) {
#if RELAY_ACTIVE_HIGH
  return on ? HIGH : LOW;
#else
  return on ? LOW : HIGH;
#endif
}

static inline void relayApplyLocked(bool on, const char* reason) {
  s_on = on;
  s_reason = reason;
  digitalWrite(RELAY_PIN, relayLevel(on));
}

// ==================== 初始化 ====================
void relayInit() {
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, relayLevel(false));

  portENTER_CRITICAL(&s_mux);
  s_on = false;
  s_forceOn = false;
  s_forceOff = false;
  s_reason = "boot";
  s_firstOverMs = 0;
  portEXIT_CRITICAL(&s_mux);

  Serial.printf("🔌 继电器初始化: GPIO%d，%s触发，默认断开\n",
                RELAY_PIN, RELAY_ACTIVE_HIGH ? "高电平" : "低电平");
}

// ==================== 手动控制 ====================
void relaySet(bool on, const char* reason) {
  portENTER_CRITICAL(&s_mux);
  s_forceOn = on;
  s_forceOff = !on;
  if (s_on != on) {
    relayApplyLocked(on, reason);
  } else {
    s_reason = reason;
  }
  portEXIT_CRITICAL(&s_mux);

  Serial.printf("🔌 继电器手动%s（%s）\n", on ? "开启" : "关闭", reason ? reason : "");
}

void relaySetAuto(bool on) {
  portENTER_CRITICAL(&s_mux);
  s_auto = on;
  portEXIT_CRITICAL(&s_mux);
  Serial.printf("🌡 自动温控已%s\n", on ? "开启" : "关闭");
}

bool relayGetAuto() {
  bool v;
  portENTER_CRITICAL(&s_mux);
  v = s_auto;
  portEXIT_CRITICAL(&s_mux);
  return v;
}

void relaySetThreshold(float t) {
  if (t < 0.0f) t = 0.0f;
  if (t > 100.0f) t = 100.0f;
  portENTER_CRITICAL(&s_mux);
  s_threshold = t;
  portEXIT_CRITICAL(&s_mux);
  Serial.printf("🌡 温度阈值已设为 %.1f °C\n", t);
}

float relayGetThreshold() {
  float v;
  portENTER_CRITICAL(&s_mux);
  v = s_threshold;
  portEXIT_CRITICAL(&s_mux);
  return v;
}

bool relayGet() {
  bool v;
  portENTER_CRITICAL(&s_mux);
  v = s_on;
  portEXIT_CRITICAL(&s_mux);
  return v;
}

const char* relayGetReason() {
  const char* v;
  portENTER_CRITICAL(&s_mux);
  v = s_reason;
  portEXIT_CRITICAL(&s_mux);
  return v;
}

// ==================== 温度联动评估 ====================
void relayUpdate(unsigned long now, float temperature, bool valid) {
  portENTER_CRITICAL(&s_mux);
  bool  autoOn = s_auto;
  bool  forceOn = s_forceOn;
  float thr = s_threshold;
  portEXIT_CRITICAL(&s_mux);

  if (!autoOn) return;

  if (!valid) {
    portENTER_CRITICAL(&s_mux);
    s_firstOverMs = 0;
    s_reason = "fault";
    portEXIT_CRITICAL(&s_mux);
    return;
  }

  // 传感器恢复正常后清除上电/故障提示
  portENTER_CRITICAL(&s_mux);
  if (strcmp(s_reason, "fault") == 0 || strcmp(s_reason, "boot") == 0) {
    s_reason = s_on ? "hot" : "idle";
  }
  portEXIT_CRITICAL(&s_mux);

  float stopThr = thr - RELAY_TEMP_HYSTERESIS;

  // 降温到复归点：解除人工关闭（人工开启保持不变）
  if (temperature <= stopThr) {
    portENTER_CRITICAL(&s_mux);
    s_forceOff = false;
    portEXIT_CRITICAL(&s_mux);
  }

  // 人工开启优先，保持开启
  if (forceOn) return;

  // 超阈防抖计时
  portENTER_CRITICAL(&s_mux);
  if (temperature > thr) {
    if (s_firstOverMs == 0) s_firstOverMs = now;
  } else {
    s_firstOverMs = 0;
  }
  uint32_t overStart = s_firstOverMs;
  bool curOn = s_on;
  bool forceOff = s_forceOff;
  portEXIT_CRITICAL(&s_mux);

  bool confirmedOver = (overStart != 0) && (now - overStart >= RELAY_TEMP_DEBOUNCE_MS);

  // 降温复归：关闭
  if (temperature <= stopThr) {
    if (curOn) {
      portENTER_CRITICAL(&s_mux);
      relayApplyLocked(false, "cool");
      portEXIT_CRITICAL(&s_mux);
      Serial.printf("🌡 温度 %.1f°C ≤ %.1f°C，继电器自动关闭\n", temperature, stopThr);
    }
    return;
  }

  // 高温确认：启动（人工关闭优先则保持关闭）
  if (confirmedOver && !forceOff && !curOn) {
    portENTER_CRITICAL(&s_mux);
    relayApplyLocked(true, "hot");
    portEXIT_CRITICAL(&s_mux);
    Serial.printf("🌡 温度 %.1f°C > %.1f°C，继电器自动启动\n", temperature, thr);
  }
}

// ==================== 状态 JSON / 遥测 ====================
String relayStatusJson() {
  char buf[128];
  snprintf(buf, sizeof(buf),
           "{\"relay\":\"%s\",\"auto\":%s,\"reason\":\"%s\",\"thr\":%.1f}",
           relayGet() ? "on" : "off",
           relayGetAuto() ? "true" : "false",
           relayGetReason(),
           relayGetThreshold());
  return String(buf);
}

void relayFormatTelemetry(float temperature, float humidity, char* out, size_t outSize) {
  snprintf(out, outSize,
           "{\"temp\":%.1f,\"hum\":%.1f,\"relay\":\"%s\",\"auto\":%s,\"reason\":\"%s\",\"thr\":%.1f}",
           temperature, humidity,
           relayGet() ? "on" : "off",
           relayGetAuto() ? "true" : "false",
           relayGetReason(),
           relayGetThreshold());
}

void relayAnnounce() {
  float t = 0.0f, h = 0.0f;
  bool valid = false;
  unsigned long age = 0;
  sensorGet(t, h, valid, age);

  char buf[128];
  relayFormatTelemetry(t, h, buf, sizeof(buf));
  markTelemetry(buf);
}
