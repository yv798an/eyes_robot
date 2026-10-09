#include "app.h"

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

char pendingFlowEvent[32] = "";
volatile bool hasFlowEvent = false;
portMUX_TYPE flowEventMux = portMUX_INITIALIZER_UNLOCKED;

char pendingTelemetry[128] = "";
volatile bool hasTelemetry = false;
portMUX_TYPE telemetryMux = portMUX_INITIALIZER_UNLOCKED;

volatile bool wifi_ready = false;
volatile bool wifi_online = false;
volatile bool manualLocked = false;

// ==================== 蓝牙状态 ====================
volatile bool bleActive = false;
volatile bool bleConnected = false;
bool bleInited = false;
BLEServer* bleServer = nullptr;
BLECharacteristic* bleTxChar = nullptr;

// ==================== 本地调试状态 ====================
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
Frame LEFT_OPEN  = {95, 80, 120, 60, 85, 90};
Frame RIGHT_OPEN = {70, 110, 90, 90, 85, 90};

// ==================== Motion Planner 状态 ====================
Frame planStart   = {95, 80, 90, 90, 85, 90};
Frame planTarget  = {95, 80, 90, 90, 85, 90};
Frame planCurrent = {95, 80, 90, 90, 85, 90};
unsigned long planStartTime = 0;
unsigned long planDuration  = 0;
bool planActive = false;

// ==================== 行为层状态 ====================
volatile bool behaviorEnabled = BEHAVIOR_ENABLED;
float behaviorEyeOffX = 0.0f;
float behaviorEyeOffY = 0.0f;
float behaviorBlink   = 0.0f;

// ==================== Servo Controller 状态 ====================
Frame lastWritten = {0, 0, 0, 0, 0, 0};
bool hasWritten = false;

// ==================== A/B 表情帧交替 ====================
bool useB = false;

// ==================== 动作队列 ====================
QueueHandle_t actionQueue = NULL;
volatile bool sequenceRunning = false;
volatile unsigned long nextActionTime = 0;

// ==================== 手动模式状态 ====================
Frame manualTarget;
bool manualClosed = false;
float manualVel[6] = {0, 0, 0, 0, 0, 0};
unsigned long lastManualSample = 0;
