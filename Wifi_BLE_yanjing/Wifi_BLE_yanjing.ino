#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketMCP.h>
#include <ESP32Servo.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_system.h>
#include <math.h>
#include <string.h>

// ==================== 配置：保持原样 ====================
const char* ssid         = "oppofindx8";
const char* password     = "eiku8997";
const char* mcpEndpoint  = "ws://api.xiaozhi.me/mcp/?token=eyJhbGciOiJFUzI1NiIsInR5cCI6IkpXVCJ9.eyJ1c2VySWQiOjkwNTU2MCwiYWdlbnRJZCI6MTczMjg2NywiZW5kcG9pbnRJZCI6ImFnZW50XzE3MzI4NjciLCJwdXJwb3NlIjoibWNwLWVuZHBvaW50IiwiaWF0IjoxNzc2ODc0OTM1LCJleHAiOjE4MDg0MzI1MzV9.YHz8fjoOcp0W1FqgmuLMtS-8IC06r35QYN_nN3Hvi4gnO20m8eACLkGniKik96LD_ztNJRQNk8Dfa9X7JVSQlg";

#define MQTT_SERVER       "119.91.109.180"
#define MQTT_PORT         9501
#define MQTT_CLIENT_ID    "0cf0a6fc7b174a1cb6c716a17b671472"
#define MQTT_USERNAME     MQTT_CLIENT_ID
#define MQTT_PASSWORD     MQTT_CLIENT_ID

#define MQTT_TOPIC_CMD    "BMP006"   // 硬件【接收指令】
#define MQTT_TOPIC_STATUS "BMP004"   // 硬件【上报状态】

// ==================== 引脚：保持原样 ====================
#define TL_PIN 16
#define BL_PIN 17
#define TR_PIN 22
#define BR_PIN 23
#define X_PIN 18
#define Y_PIN 19

// ==================== 本地调试输入引脚 ====================
#define PINX     33      // 摇杆X轴
#define PINY     32      // 摇杆Y轴
#define PIN_BTN  13      // 摇杆按键SW
#define PIN_R    34      // 电位器中间位

#define USE_POT_CONTROL 1
#define ADC_RESOLUTION  12

// ==================== 本地调试角度参数 ====================
#define TOP_LEFT_CLOSE     65
#define TOP_LEFT_OPEN      110
#define BOTTOM_LEFT_CLOSE  115
#define BOTTOM_LEFT_OPEN   70
#define TOP_RIGHT_CLOSE    115
#define TOP_RIGHT_OPEN     70
#define BOTTOM_RIGHT_CLOSE 60
#define BOTTOM_RIGHT_OPEN  125

#define XARM_LEFT    53
#define XARM_RIGHT   117
#define YARM_TOP     74
#define YARM_BOTTOM  111

#define WIFI_ARGX -5
#define WIFI_ARGY 0

#define POT_LEFT_TOP      0
#define POT_LEFT_BOTTOM   0
#define POT_RIGHT_TOP     0
#define POT_RIGHT_BOTTOM  0

#define R_THVALUE 200

// ==================== 结构体 ====================
struct Frame {
  float tl, bl, tr, br, x, y;
};

struct ActionItem {
  char expr[16];
  uint32_t holdMs;
};

struct ParsedAction {
  int pos;
  const char* expr;
};

struct MicroCfg {
  float ampX;
  float ampY;
};

// ==================== 全局对象 ====================
WiFiClient wifiClientMQTT;
PubSubClient mqttClient(wifiClientMQTT);
WebSocketMCP mcpClient;
Servo tl, bl, tr, br, Xarm, Yarm;

// ==================== 当前状态 ====================
char currentExpr[16] = "init";
portMUX_TYPE exprMux = portMUX_INITIALIZER_UNLOCKED;

char pendingExpr[16] = "init";
volatile bool newExprPending = false;
portMUX_TYPE reqMux = portMUX_INITIALIZER_UNLOCKED;

char pendingStatus[16] = "init";
volatile bool hasPendingStatus = false;
portMUX_TYPE statusMux = portMUX_INITIALIZER_UNLOCKED;

volatile bool wifi_ready = false;
volatile bool wifi_online = false;   // WiFi 当前是否在线（动态，掉线立即为 false）
volatile bool manualLocked = false;  // 开机长按摇杆键 = 锁定手动模式（关闭 WiFi/蓝牙）

// ==================== 蓝牙（BLE NUS）状态 ====================
volatile bool bleActive = false;     // 蓝牙广播是否开启
volatile bool bleConnected = false;  // 是否有客户端连接
bool bleInited = false;
BLEServer* bleServer = nullptr;
BLECharacteristic* bleTxChar = nullptr;

// 本地调试状态变量
int calValues[] = {0, 0, 0, 0, 0, 0};
int roldValue = 0;
int lastBtnStatus = HIGH;

// ==================== 表情帧定义 ====================
Frame INIT    = {95, 80, 90, 90, 85, 90};
Frame CLOSE   = {70, 110, 120, 60, 85, 90};
Frame DIS     = {85, 60, 100, 110, 100, 100};
Frame SUR_A   = {130, 60, 60, 110, 85, 90};
Frame SUR_B   = {125, 65, 60, 105, 85, 90};
Frame TIRE_A  = {70, 80, 120, 95, 85, 75};
Frame TIRE_B  = {75, 75, 115, 100, 85, 75};
Frame SMIL_A  = {100, 110, 80, 60, 85, 100};
Frame SMIL_B  = {95, 115, 90, 55, 85, 100};
Frame WINK_A  = {70, 110, 80, 60, 88, 90};
Frame WINK_B  = {70, 110, 70, 60, 88, 100};

// ==================== 运动规划参数 ====================
const float EYELID_MAX_SPEED = 110.0f;        // 眼皮轴最大速度 deg/s
const float EYE_MAX_SPEED    = 80.0f;         // 眼球 X/Y 轴最大速度 deg/s
const unsigned long PLAN_MIN_DURATION = 300;  // 表情过渡最短时长 ms
const unsigned long PLAN_MAX_DURATION = 1000; // 表情过渡最长时长 ms
const unsigned long SERVO_TICK_MS = 10;       // 舵机输出固定节拍 ms
const unsigned long MANUAL_SAMPLE_MS = 20;    // 手动输入采样周期 ms

const float MANUAL_MAX_SPEED = 180.0f;        // 手动跟随最大速度 deg/s
const float MANUAL_MAX_ACCEL = 2400.0f;       // 手动跟随最大加速度 deg/s^2

const float MICRO_PERIOD_MS = 4000.0f;        // 微动正弦周期 ms（放慢更自然，减少"抖动感"）
const float MICRO_FADE_MS   = 600.0f;         // 微动淡入/淡出包络时间 ms

// ==================== Motion Planner 状态 ====================
Frame planStart   = {95, 80, 90, 90, 85, 90};
Frame planTarget  = {95, 80, 90, 90, 85, 90};
Frame planCurrent = {95, 80, 90, 90, 85, 90};
unsigned long planStartTime = 0;
unsigned long planDuration  = 0;
bool planActive = false;

// ==================== Micro Motion 状态 ====================
float microAmpTargetX = 0.0f;
float microAmpTargetY = 0.0f;
float microAmpX = 0.0f;
float microAmpY = 0.0f;
float microX = 0.0f;
float microY = 0.0f;

// ==================== Servo Controller 状态 ====================
Frame lastWritten = {0, 0, 0, 0, 0, 0};
bool hasWritten = false;

// A/B 表情帧交替
bool useB = false;

// ==================== 动作队列 ====================
#define ACTION_QUEUE_SIZE 12
QueueHandle_t actionQueue = NULL;

volatile bool sequenceRunning = false;
volatile unsigned long nextActionTime = 0;
const uint32_t DEFAULT_ACTION_HOLD_MS = 3000;

// ==================== 工具函数 ====================
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

void publishPendingStatus() {
  if (!mqttClient.connected()) return;

  char localStatus[16];
  bool needPublish = false;

  portENTER_CRITICAL(&statusMux);
  if (hasPendingStatus) {
    strncpy(localStatus, pendingStatus, sizeof(localStatus) - 1);
    localStatus[sizeof(localStatus) - 1] = '\0';
    hasPendingStatus = false;
    needPublish = true;
  }
  portEXIT_CRITICAL(&statusMux);

  if (needPublish) {
    mqttClient.publish(MQTT_TOPIC_STATUS, localStatus);
  }
}

bool isValidExpr(const char* e) {
  return strcmp(e, "init") == 0 || strcmp(e, "close") == 0 ||
         strcmp(e, "dis") == 0  || strcmp(e, "surprise") == 0 ||
         strcmp(e, "tired") == 0|| strcmp(e, "smile") == 0 ||
         strcmp(e, "wink") == 0;
}

bool sameFrame(Frame a, Frame b) {
  return (fabs(a.tl - b.tl) < 0.1f) && (fabs(a.bl - b.bl) < 0.1f) &&
         (fabs(a.tr - b.tr) < 0.1f) && (fabs(a.br - b.br) < 0.1f) &&
         (fabs(a.x - b.x) < 0.1f)   && (fabs(a.y - b.y) < 0.1f);
}

float lerpFloat(float a, float b, float t) {
  return a + (b - a) * t;
}

Frame lerpFrame(Frame a, Frame b, float t) {
  Frame r;
  r.tl = lerpFloat(a.tl, b.tl, t);
  r.bl = lerpFloat(a.bl, b.bl, t);
  r.tr = lerpFloat(a.tr, b.tr, t);
  r.br = lerpFloat(a.br, b.br, t);
  r.x  = lerpFloat(a.x,  b.x,  t);
  r.y  = lerpFloat(a.y,  b.y,  t);
  return r;
}

// 五次 S 型缓动（速度与加速度都平滑，零急动）
float smootherStep(float t) {
  t = constrain(t, 0.0f, 1.0f);
  return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

// ==================== Servo Controller（唯一舵机输出入口） ====================
int angleToUs(float deg) {
  deg = constrain(deg, 0.0f, 180.0f);
  return (int)round(500.0f + deg * (2000.0f / 180.0f));
}

void servoWriteAll(Frame f) {
  tl.writeMicroseconds(angleToUs(f.tl));
  bl.writeMicroseconds(angleToUs(f.bl));
  tr.writeMicroseconds(angleToUs(f.tr));
  br.writeMicroseconds(angleToUs(f.br));
  Xarm.writeMicroseconds(angleToUs(f.x));
  Yarm.writeMicroseconds(angleToUs(f.y));
}

void servoWriteIfChanged(Frame f) {
  if (hasWritten && sameFrame(f, lastWritten)) return;
  servoWriteAll(f);
  lastWritten = f;
  hasWritten = true;
}

// ==================== Motion Planner（轨迹规划） ====================
// 六轴同步：取最慢轴决定总时长，再用 S 曲线同步到终点
void planTo(Frame target, unsigned long minDur, unsigned long maxDur) {
  planStart = planCurrent;
  planTarget = target;

  float lidMax = fabs(target.tl - planStart.tl);
  lidMax = max(lidMax, fabs(target.bl - planStart.bl));
  lidMax = max(lidMax, fabs(target.tr - planStart.tr));
  lidMax = max(lidMax, fabs(target.br - planStart.br));
  float lidTime = lidMax / EYELID_MAX_SPEED * 1000.0f;

  float xyMax = max(fabs(target.x - planStart.x), fabs(target.y - planStart.y));
  float xyTime = xyMax / EYE_MAX_SPEED * 1000.0f;

  float tMax = max(lidTime, xyTime);
  planDuration = constrain((unsigned long)round(tMax), minDur, maxDur);
  planStartTime = millis();
  planActive = true;
}

void updatePlanner(unsigned long now) {
  if (!planActive) return;

  float t = (float)(now - planStartTime) / (float)planDuration;

  if (t >= 1.0f) {
    planCurrent = planTarget;
    planActive = false;
  } else {
    planCurrent = lerpFrame(planStart, planTarget, smootherStep(t));
  }
}

// ==================== Micro Motion Layer（微动叠加，可淡入/淡出） ====================
// 微动总开关：0 = 表情保持期完全静止；1 = 开启仿生微动
#define MICRO_MOTION_ENABLED 0

MicroCfg microCfgFor(const char* expr) {
#if MICRO_MOTION_ENABLED
  if (strcmp(expr, "surprise") == 0) return {0.0f, 2.0f};
  if (strcmp(expr, "tired")    == 0) return {3.5f, 0.0f};
  if (strcmp(expr, "smile")    == 0) return {0.0f, 3.0f};
  if (strcmp(expr, "dis")      == 0) return {3.0f, 0.0f};
#endif
  return {0.0f, 0.0f};
}

void updateMicroMotion(unsigned long now) {
  // 包络：幅值指数趋近目标，切换表情时自然淡入/淡出，无跳变
  const float k = 1.0f - expf(-(float)SERVO_TICK_MS / MICRO_FADE_MS);
  microAmpX += (microAmpTargetX - microAmpX) * k;
  microAmpY += (microAmpTargetY - microAmpY) * k;

  float rad = (float)(now % (unsigned long)MICRO_PERIOD_MS) / MICRO_PERIOD_MS * 2.0f * M_PI;
  float wave = sinf(rad);

  microX = wave * microAmpX;
  microY = wave * microAmpY;
}

// ==================== 输出合成 ====================
Frame composeOutputFrame() {
  Frame o = planCurrent;
  o.x += microX;
  o.y += microY;
  o.x = constrain(o.x, 50.0f, 120.0f);
  o.y = constrain(o.y, 75.0f, 120.0f);
  return o;
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

  MicroCfg cfg = microCfgFor(e);
  microAmpTargetX = cfg.ampX;
  microAmpTargetY = cfg.ampY;

  useB = !useB;
  planTo(getFrame(), PLAN_MIN_DURATION, PLAN_MAX_DURATION);
}

void startSingleExpr(const char* e) {
  if (actionQueue != NULL) {
    xQueueReset(actionQueue);
  }

  sequenceRunning = false;
  nextActionTime = 0;

  applyExpr(e);
}

bool startSequence(ActionItem* items, int count) {
  if (actionQueue == NULL || count <= 0) return false;

  xQueueReset(actionQueue);

  int realCount = 0;

  for (int i = 0; i < count; i++) {
    if (!isValidExpr(items[i].expr)) continue;

    xQueueSend(actionQueue, &items[i], 0);
    realCount++;
  }

  if (realCount <= 0) return false;

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

// ==================== ADC 滑动平均滤波 (防抖) ====================
int readFilteredADC(int pin, int samples = 8) {
  long sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(pin);
  }
  return sum / samples;
}

// ==================== 手动模式（速度/加速度受限跟随器） ====================
Frame manualTarget;
bool manualClosed = false;
float manualVel[6] = {0, 0, 0, 0, 0, 0};
unsigned long lastManualSample = 0;

// 单轴逼近：梯形速度轮廓（加速-匀速-缓停），目标随时可改，不丢速度
float approachAxis(float cur, float target, float dt, float& vel) {
  float dist = target - cur;

  // 距目标越近，允许速度越低 → 自然缓停
  float vLimit = min(MANUAL_MAX_SPEED, fabs(dist) / dt);
  float vTarget = (dist > 0.0f) ? vLimit : -vLimit;

  float dv = MANUAL_MAX_ACCEL * dt;
  if (vTarget > vel + dv) vel += dv;
  else if (vTarget < vel - dv) vel -= dv;
  else vel = vTarget;

  float next = cur + vel * dt;

  if ((dist >= 0.0f && next >= target) || (dist <= 0.0f && next <= target)) {
    vel = 0.0f;
    return target;
  }
  return next;
}

void manualSampleInputs() {
  int btnStatus = digitalRead(PIN_BTN);

  if (btnStatus != lastBtnStatus) {
    delay(10);
    btnStatus = digitalRead(PIN_BTN);

    if (btnStatus != lastBtnStatus) {
      lastBtnStatus = btnStatus;
      manualClosed = (btnStatus == LOW);

      if (btnStatus == LOW) {
        Serial.println("摇杆按键：闭眼");
      } else {
        Serial.println("摇杆按键：睁眼");
      }
    }
  }

  int xvalue = map(readFilteredADC(PINX, 12), 0, (1 << ADC_RESOLUTION) - 1, XARM_LEFT + calValues[4], XARM_RIGHT + calValues[4]);
  int yvalue = map(readFilteredADC(PINY, 12), 0, (1 << ADC_RESOLUTION) - 1, YARM_BOTTOM + calValues[5], YARM_TOP + calValues[5]);

  xvalue = constrain(xvalue, XARM_LEFT, XARM_RIGHT);
  yvalue = constrain(yvalue, YARM_TOP, YARM_BOTTOM);

  // 椭圆限幅：斜角拉满时两轴同时到极限会超出眼球机械行程导致卡顿，
  // 将斜向输出压回椭圆边界，轴向满幅不受影响
  float cx = (XARM_LEFT + XARM_RIGHT) / 2.0f;
  float cy = (YARM_TOP + YARM_BOTTOM) / 2.0f;
  float rx = (XARM_RIGHT - XARM_LEFT) / 2.0f;
  float ry = (YARM_BOTTOM - YARM_TOP) / 2.0f;
  float dx = xvalue - cx;
  float dy = yvalue - cy;
  float nx = dx / rx;
  float ny = dy / ry;
  float len = sqrtf(nx * nx + ny * ny);
  if (len > 1.0f) {
    nx /= len;
    ny /= len;
    xvalue = (int)round(cx + nx * rx);
    yvalue = (int)round(cy + ny * ry);
  }

  Frame target;
  target.x = xvalue;
  target.y = yvalue;

  if (manualClosed) {
    target.tl = TOP_LEFT_CLOSE;
    target.bl = BOTTOM_LEFT_CLOSE;
    target.tr = TOP_RIGHT_CLOSE;
    target.br = BOTTOM_RIGHT_CLOSE;
  } else {
#if USE_POT_CONTROL
    // 电位器始终生效：旋钮实时调节眼皮开合
    int rValue = readFilteredADC(PIN_R);

    if (abs(roldValue - rValue) > R_THVALUE) {
      roldValue = rValue;
    }

    target.tl = map(roldValue, 0, (1 << ADC_RESOLUTION) - 1, TOP_LEFT_CLOSE, TOP_LEFT_OPEN);
    target.bl = map(roldValue, 0, (1 << ADC_RESOLUTION) - 1, BOTTOM_LEFT_CLOSE, BOTTOM_LEFT_OPEN);
    target.tr = map(roldValue, 0, (1 << ADC_RESOLUTION) - 1, TOP_RIGHT_CLOSE, TOP_RIGHT_OPEN);
    target.br = map(roldValue, 0, (1 << ADC_RESOLUTION) - 1, BOTTOM_RIGHT_CLOSE, BOTTOM_RIGHT_OPEN);
#else
    target.tl = INIT.tl;
    target.bl = INIT.bl;
    target.tr = INIT.tr;
    target.br = INIT.br;
#endif
  }

  // 0.3° 死区，滤掉 ADC 噪声
  if (fabs(target.tl - manualTarget.tl) >= 0.3f || fabs(target.bl - manualTarget.bl) >= 0.3f ||
      fabs(target.tr - manualTarget.tr) >= 0.3f || fabs(target.br - manualTarget.br) >= 0.3f ||
      fabs(target.x - manualTarget.x) >= 0.3f || fabs(target.y - manualTarget.y) >= 0.3f) {
    manualTarget = target;
  }
}

void manualFollowTarget() {
  float dt = (float)SERVO_TICK_MS / 1000.0f;

  planCurrent.tl = approachAxis(planCurrent.tl, manualTarget.tl, dt, manualVel[0]);
  planCurrent.bl = approachAxis(planCurrent.bl, manualTarget.bl, dt, manualVel[1]);
  planCurrent.tr = approachAxis(planCurrent.tr, manualTarget.tr, dt, manualVel[2]);
  planCurrent.br = approachAxis(planCurrent.br, manualTarget.br, dt, manualVel[3]);
  planCurrent.x  = approachAxis(planCurrent.x,  manualTarget.x,  dt, manualVel[4]);
  planCurrent.y  = approachAxis(planCurrent.y,  manualTarget.y,  dt, manualVel[5]);
}

// ==================== 表情识别与映射 ====================
String toLowerCopy(String s) {
  s.toLowerCase();
  return s;
}

const char* canonicalizeExpr(String raw) {
  raw.trim();
  String s = toLowerCopy(raw);

  if (s == "init" || s.indexOf("睁眼") >= 0 || s.indexOf("正常") >= 0 || s.indexOf("初始") >= 0 || s.indexOf("复位") >= 0 || s.indexOf("重置") >= 0 || s.indexOf("恢复") >= 0) return "init";
  if (s == "close" || s.indexOf("闭眼") >= 0 || s.indexOf("睡觉") >= 0 || s.indexOf("闭上") >= 0) return "close";
  if (s == "dis" || s.indexOf("鄙视") >= 0 || s.indexOf("不屑") >= 0 || s.indexOf("嫌弃") >= 0 || s.indexOf("白眼") >= 0) return "dis";
  if (s == "surprise" || s.indexOf("惊讶") >= 0 || s.indexOf("吃惊") >= 0 || s.indexOf("震惊") >= 0) return "surprise";
  if (s == "tired" || s.indexOf("困") >= 0 || s.indexOf("累") >= 0 || s.indexOf("想睡") >= 0 || s.indexOf("疲倦") >= 0 || s.indexOf("疲惫") >= 0) return "tired";
  if (s == "smile" || s.indexOf("笑") >= 0 || s.indexOf("开心") >= 0 || s.indexOf("高兴") >= 0 || s.indexOf("微笑") >= 0) return "smile";
  if (s == "wink" || s.indexOf("媚眼") >= 0 || s.indexOf("眨眼") >= 0 || s.indexOf("wink") >= 0 || s.indexOf("抛媚眼") >= 0) return "wink";

  return "";
}

bool alreadyHasNear(ParsedAction* arr, int count, int pos, const char* expr) {
  for (int i = 0; i < count; i++) {
    if (arr[i].pos == pos && strcmp(arr[i].expr, expr) == 0) return true;
  }
  return false;
}

void addTermMatches(String s, const char* term, const char* expr, ParsedAction* out, int& count, int maxCount) {
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

void sortActionsByPosition(ParsedAction* arr, int count) {
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

  addTermMatches(s, "init", "init", found, foundCount, 20);
  addTermMatches(s, "睁眼", "init", found, foundCount, 20);
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
  addTermMatches(s, "眨眼", "wink", found, foundCount, 20);
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

// ==================== 统一指令分发（MQTT / 蓝牙共用） ====================
void dispatchCommand(const String& cmd) {
  String c = cmd;
  c.trim();

  ActionItem items[8];
  int count = parseActionsFromText(c, items, 8, DEFAULT_ACTION_HOLD_MS);

  if (count <= 0) {
    Serial.println("⚠️ 无法识别指令：" + c);
    return;
  }

  if (count == 1) {
    startSingleExpr(items[0].expr);
  } else {
    startSequence(items, count);
  }
}

// ==================== 蓝牙 BLE（Nordic UART Service） ====================
#define BLE_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define BLE_CHAR_RX_UUID "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define BLE_CHAR_TX_UUID "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

class MyBleServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    bleConnected = true;
    Serial.println("📱 蓝牙客户端已连接");
  }
  void onDisconnect(BLEServer* pServer) {
    bleConnected = false;
    Serial.println("📱 蓝牙客户端已断开");
    if (bleActive) {
      BLEDevice::startAdvertising();
    }
  }
};

class MyBleRxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) {
    String cmd = String(pChar->getValue().c_str());
    cmd.trim();
    Serial.println("📥 蓝牙收到指令：" + cmd);
    dispatchCommand(cmd);
  }
};

void bleNotify(const String& msg) {
  if (bleActive && bleConnected && bleTxChar != nullptr) {
    bleTxChar->setValue(msg.c_str());
    bleTxChar->notify();
  }
}

void startBLE() {
  if (!bleInited) {
    BLEDevice::init("EyeRobot");
    bleServer = BLEDevice::createServer();
    bleServer->setCallbacks(new MyBleServerCallbacks());

    BLEService* svc = bleServer->createService(BLE_SERVICE_UUID);
    BLECharacteristic* rx = svc->createCharacteristic(BLE_CHAR_RX_UUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    rx->setCallbacks(new MyBleRxCallbacks());
    bleTxChar = svc->createCharacteristic(BLE_CHAR_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
    bleTxChar->addDescriptor(new BLE2902());
    svc->start();
    bleInited = true;
  }

  bleActive = true;
  BLEDevice::startAdvertising();
  Serial.println("📡 蓝牙广播已开启（设备名 EyeRobot，NUS 串口服务）");
}

void stopBLE() {
  if (!bleActive) return;
  BLEDevice::stopAdvertising();
  bleActive = false;
  bleConnected = false;
  Serial.println("📡 蓝牙广播已关闭");
}

// 蓝牙阶段：广播并等待连接；有客户端连接则保持到断开，无客户端则超时返回
void blePhase(unsigned long idleTimeoutMs) {
  startBLE();

  unsigned long t0 = millis();
  while (1) {
    if (bleConnected) {
      while (bleConnected) {
        vTaskDelay(200);
      }
      break;
    }
    if (millis() - t0 >= idleTimeoutMs) {
      break;
    }
    vTaskDelay(100);
  }

  stopBLE();
}

// ==================== MQTT 指令接收 ====================
void mqttCallback(char* topic, byte* payload, unsigned int len) {
  String cmd;

  for (int i = 0; i < len; i++) {
    cmd += (char)payload[i];
  }

  cmd.trim();
  dispatchCommand(cmd);
}

// ==================== MCP 工具注册 ====================
void registerTools() {
  mcpClient.registerTool(
    "eye_expression",
    "控制仿生眼的单个表情。用户表达笑、开心、困、累、想睡、惊讶、吃惊、媚眼、眨眼、鄙视、不屑、闭眼、睡觉、睁眼、正常、初始、复位等单个动作时，必须调用本工具。一次只执行一个表情。",
    R"({
      "type": "object",
      "properties": {
        "expression": {
          "type": "string",
          "description": "目标表情，只能选择：init=睁眼/正常/初始/复位；close=闭眼/睡觉；dis=鄙视/不屑；surprise=惊讶/吃惊；tired=困/累/想睡；smile=笑/开心；wink=媚眼/眨眼。",
          "enum": ["init", "close", "dis", "surprise", "tired", "smile", "wink"]
        }
      },
      "required": ["expression"],
      "additionalProperties": false
    })",
    [](const String& args) {
      DynamicJsonDocument doc(1024);
      DeserializationError err = deserializeJson(doc, args);

      if (err) {
        return WebSocketMCP::ToolResponse("{\"status\":\"error\",\"message\":\"JSON解析失败\"}");
      }

      String raw = doc["expression"].as<String>();
      raw.trim();

      ActionItem items[8];
      int count = parseActionsFromText(raw, items, 8, DEFAULT_ACTION_HOLD_MS);

      if (count <= 0) {
        return WebSocketMCP::ToolResponse("{\"status\":\"error\",\"message\":\"无法识别表情\"}");
      }

      if (count == 1) {
        startSingleExpr(items[0].expr);

        String res = "{\"status\":\"ok\",\"mode\":\"single\",\"expression\":\"";
        res += items[0].expr;
        res += "\"}";
        return WebSocketMCP::ToolResponse(res);
      }

      startSequence(items, count);

      String res = "{\"status\":\"ok\",\"mode\":\"sequence\",\"count\":";
      res += String(count);
      res += "}";
      return WebSocketMCP::ToolResponse(res);
    }
  );

  mcpClient.registerTool(
    "eye_expression_sequence",
    "按顺序控制仿生眼执行多个表情。用户说“先...再...然后...”“连续做几个动作”“眨眼后微笑”“闭眼再睁眼”等复合动作时，必须调用本工具，而不是只调用单动作工具。",
    R"({
      "type": "object",
      "properties": {
        "actions": {
          "type": "array",
          "description": "需要按顺序执行的表情数组。每个元素只能是 init, close, dis, surprise, tired, smile, wink。",
          "items": {
            "type": "string",
            "enum": ["init", "close", "dis", "surprise", "tired", "smile", "wink"]
          },
          "minItems": 1,
          "maxItems": 8
        },
        "interval_ms": {
          "type": "integer",
          "description": "每个动作保持时间，默认3000毫秒。建议范围1000到5000。",
          "minimum": 300,
          "maximum": 5000,
          "default": 3000
        }
      },
      "required": ["actions"],
      "additionalProperties": false
    })",
    [](const String& args) {
      DynamicJsonDocument doc(2048);
      DeserializationError err = deserializeJson(doc, args);

      if (err) {
        return WebSocketMCP::ToolResponse("{\"status\":\"error\",\"message\":\"JSON解析失败\"}");
      }

      uint32_t intervalMs = DEFAULT_ACTION_HOLD_MS;

      if (doc.containsKey("interval_ms")) {
        intervalMs = doc["interval_ms"].as<uint32_t>();
        intervalMs = constrain(intervalMs, 300, 5000);
      }

      ActionItem items[8];
      int count = 0;

      if (doc["actions"].is<JsonArray>()) {
        JsonArray arr = doc["actions"].as<JsonArray>();

        for (JsonVariant v : arr) {
          if (count >= 8) break;

          String raw = v.as<String>();
          const char* expr = canonicalizeExpr(raw);

          if (strlen(expr) == 0 || !isValidExpr(expr)) continue;

          strncpy(items[count].expr, expr, sizeof(items[count].expr) - 1);
          items[count].expr[sizeof(items[count].expr) - 1] = '\0';
          items[count].holdMs = intervalMs;
          count++;
        }
      }

      if (count <= 0) {
        return WebSocketMCP::ToolResponse("{\"status\":\"error\",\"message\":\"动作数组为空或无法识别\"}");
      }

      if (count == 1) {
        startSingleExpr(items[0].expr);
      } else {
        startSequence(items, count);
      }

      String res = "{\"status\":\"ok\",\"mode\":\"sequence\",\"count\":";
      res += String(count);
      res += ",\"interval_ms\":";
      res += String(intervalMs);
      res += "}";
      return WebSocketMCP::ToolResponse(res);
    }
  );

  Serial.println("✅ MCP 工具已注册：eye_expression / eye_expression_sequence");
}

void onConn(bool ok) {
  if (ok) registerTools();
}

// ==================== 舵机核心任务（固定节拍，最高实时性） ====================
void servoCoreTask(void *pvParameters) {
  analogReadResolution(ADC_RESOLUTION);

  pinMode(PINX, INPUT);
  pinMode(PINY, INPUT);
  pinMode(PIN_BTN, INPUT_PULLUP);
  pinMode(PIN_R, INPUT);

  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);

  tl.setPeriodHertz(50);
  bl.setPeriodHertz(50);
  tr.setPeriodHertz(50);
  br.setPeriodHertz(50);
  Xarm.setPeriodHertz(50);
  Yarm.setPeriodHertz(50);

  tl.attach(TL_PIN, 500, 2500);
  bl.attach(BL_PIN, 500, 2500);
  tr.attach(TR_PIN, 500, 2500);
  br.attach(BR_PIN, 500, 2500);
  Xarm.attach(X_PIN, 500, 2500);
  Yarm.attach(Y_PIN, 500, 2500);

  // 上电软启动：先写中立位 90°，随后由规划器平滑过渡，避免 6 个舵机同时猛冲造成电源跌落
  Frame neutral = {90, 90, 90, 90, 90, 90};
  planCurrent = neutral;
  planStart = neutral;
  planTarget = neutral;
  planActive = false;

  microAmpX = 0.0f;
  microAmpY = 0.0f;
  microX = 0.0f;
  microY = 0.0f;

  servoWriteAll(neutral);
  lastWritten = neutral;
  hasWritten = true;

  roldValue = readFilteredADC(PIN_R);
  lastBtnStatus = digitalRead(PIN_BTN);
  manualTarget = planCurrent;

  // 开机按住摇杆键 2 秒 → 锁定手动模式（不受 WiFi 连接切换影响，重启解除）
  bool manualLock = false;
  if (digitalRead(PIN_BTN) == LOW) {
    unsigned long holdStart = millis();
    while (digitalRead(PIN_BTN) == LOW && millis() - holdStart < 2000) {
      vTaskDelay(50);
    }
    if (digitalRead(PIN_BTN) == LOW) {
      manualLock = true;
      manualLocked = true;
      Serial.println("🔒 摇杆键长按，锁定手动控制模式（重启解除）");
    }
  }

  TickType_t lastWake = xTaskGetTickCount();

  bool lastOnline = false;

  while (1) {
    unsigned long now = millis();

    bool online = manualLock ? false : (wifi_online || bleConnected);

    if (online && !lastOnline) {
      // 连接恢复（WiFi 或蓝牙）→ 切回控制模式
      Serial.println("✅ 进入控制模式（WiFi/蓝牙）");
      applyExpr("init");
    } else if (!online && lastOnline) {
      // 连接断开 → 立即切回本地摇杆/电位器控制
      Serial.println("⚠️ 连接断开，切换到本地摇杆控制");
      planActive = false;
      if (actionQueue != NULL) xQueueReset(actionQueue);
      sequenceRunning = false;
      nextActionTime = 0;
      manualTarget = planCurrent;
      for (int i = 0; i < 6; i++) manualVel[i] = 0;
      microAmpTargetX = 0.0f;
      microAmpTargetY = 0.0f;
      microAmpX = 0.0f;
      microAmpY = 0.0f;
      microX = 0.0f;
      microY = 0.0f;
      roldValue = readFilteredADC(PIN_R);
      lastManualSample = 0;
    }
    lastOnline = online;

    if (!online) {
      if (now - lastManualSample >= MANUAL_SAMPLE_MS) {
        lastManualSample = now;
        manualSampleInputs();
      }
      manualFollowTarget();
    }

    processActionQueue();
    handlePendingExpr();
    updatePlanner(now);
    updateMicroMotion(now);
    servoWriteIfChanged(composeOutputFrame());

    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(SERVO_TICK_MS));
  }
}

// ==================== 网络任务（WiFi + MQTT） ====================
void wifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    uint8_t reason = info.wifi_sta_disconnected.reason;
    const char* hint = "";
    switch (reason) {
      case WIFI_REASON_ASSOC_LEAVE:            hint = "AP 主动断开（热点省电/断流策略）"; break;
      case WIFI_REASON_BEACON_TIMEOUT:         hint = "AP 无响应（信号弱或 AP 挂了）"; break;
      case WIFI_REASON_NO_AP_FOUND:            hint = "找不到热点（热点已关闭）"; break;
      case WIFI_REASON_AUTH_FAIL:              hint = "密码错误"; break;
      case WIFI_REASON_HANDSHAKE_TIMEOUT:      hint = "握手超时（信号不稳）"; break;
      case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: hint = "四次握手超时（信道不稳/密码错）"; break;
      case WIFI_REASON_AUTH_EXPIRE:            hint = "认证过期，AP 要求重新连接"; break;
      default: break;
    }
    Serial.println("⚠️ WiFi 已断开, 原因码: " + String(reason) + " " + hint);
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    Serial.println("✅ WiFi 重新获取到 IP, 信号强度: " + String(WiFi.RSSI()) + " dBm");
  }
}

void networkAuxTask(void *pvParameters) {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);              // 关闭调制解调器休眠，防止路由/热点兼容性掉线
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.onEvent(wifiEvent);

  // 等待舵机任务完成"摇杆键长按锁定手动模式"检测
  vTaskDelay(2500);

  if (manualLocked) {
    // 手动锁定：彻底关闭射频，省电且不干扰摇杆控制
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("🔋 手动模式：WiFi/蓝牙均关闭，仅本地摇杆控制");
    while (1) {
      vTaskDelay(1000);
    }
  }

  // ===== 初次连接：优先 WiFi，失败则切蓝牙（二者互斥，避免 2.4G 冲突） =====
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(ssid, password);

  unsigned long tryStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - tryStart > 15000) {
      // WiFi 失败：关闭 WiFi 射频，切蓝牙广播 30 秒
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      blePhase(30000);
      WiFi.mode(WIFI_STA);
      WiFi.setSleep(false);
      WiFi.begin(ssid, password);
      tryStart = millis();
    }
    vTaskDelay(500);
  }

  stopBLE();
  Serial.println("✅ WiFi 已连接");
  wifi_ready = true;
  wifi_online = true;

  vTaskDelay(10000);

  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(1024);
  mqttClient.setKeepAlive(60);           // 心跳频率减半，减少 WiFi 发射电流尖峰
  mqttClient.setSocketTimeout(5);

  while (1) {
    // 动态同步 WiFi 在线状态，掉线时舵机任务立即切回手动/蓝牙控制
    wifi_online = (WiFi.status() == WL_CONNECTED);

    // ===== WiFi 掉线检测与自动重连 =====
    if (!wifi_online) {
      Serial.println("⚠️ 检测到 WiFi 断开，重新连接...");
      WiFi.disconnect(true);              // 彻底关闭 STA，清除驱动残留状态
      vTaskDelay(500);
      WiFi.mode(WIFI_STA);
      WiFi.setSleep(false);
      WiFi.begin(ssid, password);

      unsigned long reconnectStart = millis();
      bool reconnectOk = false;
      while (millis() - reconnectStart < 15000) {
        if (WiFi.status() == WL_CONNECTED) {
          reconnectOk = true;
          break;
        }
        vTaskDelay(500);
      }

      if (reconnectOk) {
        wifi_online = true;
        stopBLE();
        wifiClientMQTT.stop();            // 清掉 MQTT 的陈旧 socket，保证 MQTT 能重新连上
        Serial.println("✅ WiFi 重连成功, 信号强度: " + String(WiFi.RSSI()) + " dBm");
      } else {
        // WiFi 重连失败：关闭 WiFi 射频，切蓝牙（有客户端则保持到断开）
        Serial.println("❌ WiFi 重连失败，切蓝牙广播");
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        blePhase(30000);
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);
        WiFi.begin(ssid, password);
      }
      continue;
    }

    if (!mqttClient.connected()) {
      bool ok = mqttClient.connect(
        MQTT_CLIENT_ID,
        MQTT_USERNAME,
        MQTT_PASSWORD,
        NULL,
        0,
        0,
        NULL,
        true
      );

      if (ok) {
        mqttClient.subscribe(MQTT_TOPIC_CMD);
        Serial.println("✅ MQTT 已连接");

        char expr[16];
        getCurrentExpr(expr, sizeof(expr));
        markStatusPending(expr);
      } else {
        Serial.println("⚠️ MQTT 连接失败，稍后重试");
        vTaskDelay(2000);
      }
    }

    mqttClient.loop();
    publishPendingStatus();

    vTaskDelay(10);
  }
}

// ==================== MCP 任务（独立任务，避免与 MQTT 争抢栈空间） ====================
void mcpTask(void *pvParameters) {
  while (!wifi_ready) {
    vTaskDelay(200);
  }

  mcpClient.begin(mcpEndpoint, onConn);

  bool lastWifiOnline = true;

  while (1) {
    mcpClient.loop();

    // WiFi 掉线恢复后主动重置 MCP 连接，不等库内部最长 60 秒的退避
    bool online = wifi_online;
    if (online && !lastWifiOnline) {
      Serial.println("[xiaozhi-mcp] WiFi 已恢复，主动重建 MCP 连接");
      mcpClient.begin(mcpEndpoint, onConn);
    }
    lastWifiOnline = online;

    vTaskDelay(5);
  }
}

// ==================== 主函数 ====================
void setup() {
  Serial.begin(115200);
  delay(1000);

  // 打印复位原因，用于诊断重启问题
  esp_reset_reason_t reason = esp_reset_reason();
  const char* reasonName = "UNKNOWN";
  switch (reason) {
    case ESP_RST_POWERON:   reasonName = "POWERON(上电)"; break;
    case ESP_RST_EXT:       reasonName = "EXT(外部复位)"; break;
    case ESP_RST_SW:        reasonName = "SW(软件复位)"; break;
    case ESP_RST_PANIC:     reasonName = "PANIC(程序崩溃)"; break;
    case ESP_RST_INT_WDT:   reasonName = "INT_WDT(中断看门狗)"; break;
    case ESP_RST_TASK_WDT:  reasonName = "TASK_WDT(任务看门狗)"; break;
    case ESP_RST_WDT:       reasonName = "WDT(看门狗)"; break;
    case ESP_RST_DEEPSLEEP: reasonName = "DEEPSLEEP(深度睡眠)"; break;
    case ESP_RST_BROWNOUT:  reasonName = "BROWNOUT(电源电压过低!)"; break;
    case ESP_RST_SDIO:      reasonName = "SDIO"; break;
    default: break;
  }
  Serial.println("⚙️ 复位原因: " + String((int)reason) + " " + reasonName);
  Serial.println("⚙️ 空闲内存: " + String(ESP.getFreeHeap()) + " B");

  actionQueue = xQueueCreate(ACTION_QUEUE_SIZE, sizeof(ActionItem));

  if (actionQueue == NULL) {
    Serial.println("❌ 动作队列创建失败");
  }

  xTaskCreatePinnedToCore(
    servoCoreTask,
    "ServoTask",
    6144,
    NULL,
    3,
    NULL,
    0
  );

  xTaskCreatePinnedToCore(
    networkAuxTask,
    "NetTask",
    8192,
    NULL,
    1,
    NULL,
    1
  );

  xTaskCreatePinnedToCore(
    mcpTask,
    "McpTask",
    10240,
    NULL,
    1,
    NULL,
    1
  );
}

void loop() {}
