#ifndef APP_H
#define APP_H

#include <Arduino.h>
#include <Wire.h>
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
#include <freertos/queue.h>
#include <esp_system.h>
#include <math.h>
#include <string.h>

#include "config.h"
#include "flow.h"
#include "sensor.h"
#include "relay.h"

// ==================== 数据结构 ====================
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

// ==================== 全局对象 ====================
extern WiFiClient wifiClientMQTT;
extern PubSubClient mqttClient;
extern WebSocketMCP mcpClient;
extern Servo tl, bl, tr, br, Xarm, Yarm;

// ==================== 当前状态 ====================
extern char currentExpr[16];
extern portMUX_TYPE exprMux;

// 动作所有权 / 优先级：P2 动作流运行时会挡住 P1 单动作
enum ActionPriority : uint8_t { AP_IDLE = 0, AP_EXPR = 1, AP_FLOW = 2, AP_ALARM = 3, AP_EMERGENCY = 4 };
enum ActionSource   : uint8_t { AS_USER = 0, AS_FLOW, AS_ALARM, AS_SYSTEM, AS_MANUAL };

extern char pendingExpr[16];
extern volatile bool newExprPending;
extern portMUX_TYPE reqMux;

extern char pendingStatus[16];
extern volatile bool hasPendingStatus;
extern portMUX_TYPE statusMux;

extern char pendingFlowEvent[32];
extern volatile bool hasFlowEvent;
extern portMUX_TYPE flowEventMux;

extern char pendingTelemetry[128];
extern volatile bool hasTelemetry;
extern portMUX_TYPE telemetryMux;

extern volatile bool wifi_ready;
extern volatile bool wifi_online;
extern volatile bool manualLocked;

// ==================== 蓝牙状态 ====================
extern volatile bool bleActive;
extern volatile bool bleConnected;
extern bool bleInited;
extern BLEServer* bleServer;
extern BLECharacteristic* bleTxChar;

// ==================== 本地调试状态 ====================
extern int calValues[6];
extern int roldValue;
extern int lastBtnStatus;

// ==================== 表情帧定义 ====================
extern Frame INIT, CLOSE, DIS, SUR_A, SUR_B, TIRE_A, TIRE_B, SMIL_A, SMIL_B, WINK_A, WINK_B;
extern Frame LEFT_OPEN, RIGHT_OPEN;

// ==================== Motion Planner 状态 ====================
extern Frame planStart, planTarget, planCurrent;
extern unsigned long planStartTime;
extern unsigned long planDuration;
extern bool planActive;

// ==================== 行为层状态（动作流） ====================
extern volatile bool behaviorEnabled;   // 运行时可开关
extern float behaviorEyeOffX;   // 微扫视 + 漂移 合成的眼球偏移
extern float behaviorEyeOffY;
extern float behaviorBlink;     // 0..1 眨眼闭合度

// ==================== Servo Controller 状态 ====================
extern Frame lastWritten;
extern bool hasWritten;

// ==================== A/B 表情帧交替 ====================
extern bool useB;

// ==================== 动作队列 ====================
extern QueueHandle_t actionQueue;
extern volatile bool sequenceRunning;
extern volatile unsigned long nextActionTime;

// ==================== 手动模式状态 ====================
extern Frame manualTarget;
extern bool manualClosed;
extern float manualVel[6];
extern unsigned long lastManualSample;

// ==================== 工具函数 ====================
void setCurrentExpr(const char* e);
void getCurrentExpr(char* out, size_t outSize);
void markStatusPending(const char* e);
void markFlowEvent(const String& s);
void markTelemetry(const String& s);
void publishPendingStatus();
bool isValidExpr(const char* e);
bool sameFrame(Frame a, Frame b);
float lerpFloat(float a, float b, float t);
Frame lerpFrame(Frame a, Frame b, float t);
float smootherStep(float t);

// ==================== Servo Controller ====================
int angleToUs(float deg);
void servoWriteAll(Frame f);
void servoWriteIfChanged(Frame f);

// ==================== Motion Planner ====================
void planTo(Frame target, unsigned long minDur, unsigned long maxDur);
void updatePlanner(unsigned long now);

// ==================== 行为层（动作流） ====================
void behaviorInit();
void behaviorUpdate(unsigned long now);
void setBehaviorEnabled(bool on);
void triggerBlink();

// ==================== 输出合成 ====================
Frame composeOutputFrame();

// ==================== 表情控制 ====================
Frame getFrame();
void applyExpr(const char* e);
void handlePendingExpr();
bool requestExpr(const char* e, ActionPriority prio, ActionSource src);
void actionSetOwner(uint8_t prio);
uint8_t actionGetOwner();
bool startSingleExpr(const char* e);
bool startSequence(ActionItem* items, int count);
void processActionQueue();

// ==================== 表情识别与映射 ====================
String toLowerCopy(String s);
const char* canonicalizeExpr(String raw);
String resolveFlowName(String raw);
int parseActionsFromText(String raw, ActionItem* out, int maxCount, uint32_t holdMs);

// ==================== 统一指令分发 ====================
void dispatchCommand(const String& cmd);

// ==================== 手动模式 ====================
int readFilteredADC(int pin, int samples = 8);
void manualSampleInputs();
void manualFollowTarget();

// ==================== I2C ====================
void scanI2C();

// ==================== 蓝牙 BLE ====================
void bleNotify(const String& msg);
void startBLE();
void stopBLE();
void blePhase(unsigned long idleTimeoutMs);

// ==================== MQTT ====================
void mqttCallback(char* topic, byte* payload, unsigned int len);

// ==================== MCP 工具 ====================
void registerTools();
void onConn(bool ok);

// ==================== 任务 ====================
void wifiEvent(WiFiEvent_t event, WiFiEventInfo_t info);
void servoCoreTask(void* pvParameters);
void networkAuxTask(void* pvParameters);
void mcpTask(void* pvParameters);

#endif // APP_H
