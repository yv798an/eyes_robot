#include "app.h"

// ==================== ADC 滑动平均滤波 (防抖) ====================
int readFilteredADC(int pin, int samples) {
  long sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(pin);
  }
  return sum / samples;
}

// ==================== I2C 扫描（确认 DHT20 等设备是否在线） ====================
void scanI2C() {
  Serial.print("🔍 I2C 扫描: ");
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.print("0x");
      if (addr < 16) Serial.print("0");
      Serial.print(addr, HEX);
      Serial.print(" ");
      found++;
    }
  }
  if (found == 0) Serial.print("(未发现设备)");
  Serial.println();
}

// ==================== 手动模式（速度/加速度受限跟随器） ====================
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

// ==================== 动作流人工中断检测 ====================
#if FLOW_ENABLED
static unsigned long lastFlowInterruptCheck = 0;

static bool flowManualInterruptRequested() {
  if (digitalRead(PIN_BTN) == LOW) return true;
  int center = 1 << (ADC_RESOLUTION - 1);
  int x = readFilteredADC(PINX, 4);
  int y = readFilteredADC(PINY, 4);
  if (abs(x - center) > 600 || abs(y - center) > 600) return true;
  return false;
}
#endif

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

  servoWriteAll(neutral);
  lastWritten = neutral;
  hasWritten = true;

  roldValue = readFilteredADC(PIN_R);
  lastBtnStatus = digitalRead(PIN_BTN);
  manualTarget = planCurrent;

  behaviorInit();
  flowInit();

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
#if FLOW_ENABLED
      if (flowIsRunning() && FLOW_ABORT_ON_DISCONNECT) flowStop(true);
#endif
      manualTarget = planCurrent;
      for (int i = 0; i < 6; i++) manualVel[i] = 0;
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
    flowUpdate(now);

#if FLOW_ENABLED
    // 动作流运行中允许人工中断：摇杆偏移或按键按下即停止
    if (flowIsRunning() && now - lastFlowInterruptCheck >= 100) {
      lastFlowInterruptCheck = now;
      if (flowManualInterruptRequested()) {
        Serial.println("🕹 检测到人工输入，中断动作流");
        flowStop(false);
      }
    }
#endif

    updatePlanner(now);
    behaviorUpdate(now);
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

        // 同步动作流开关状态给上位机
        mqttClient.publish(MQTT_TOPIC_STATUS, behaviorEnabled ? "behavior_on" : "behavior_off");
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
