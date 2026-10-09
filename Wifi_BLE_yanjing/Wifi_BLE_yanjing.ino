#include "app.h"

// ==================== 主入口 ====================
// 本 sketch 已模块化拆分：
//   config.h        引脚 / 云平台 / 调参 / 开关
//   app.h/.cpp      类型、全局状态、表情帧表
//   motion.cpp      数学、运动规划、舵机输出、输出合成
//   behavior.cpp    行为层（待机微动：眨眼 / 微扫视 / 漂移）
//   flow.cpp        动作流引擎（预置程序，步骤表见 flows.h）
//   sensor.cpp      DHT20 温湿度采集任务
//   relay.cpp       继电器 + 温度联动控制
//   expression.cpp  表情控制、中文解析、动作队列、指令分发
//   control.cpp     手动摇杆、FreeRTOS 任务、WiFi 事件
//   links.cpp       BLE(NUS) + MQTT
//   mcp_tools.cpp   小智 MCP 工具注册

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

  // ==================== I2C 总线初始化（DHT20 温湿度） ====================
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, I2C_CLOCK_HZ);
  Serial.println("🔌 I2C 已初始化: SCL=GPIO" + String(I2C_SCL_PIN) + " SDA=GPIO" + String(I2C_SDA_PIN));
  scanI2C();

  // ==================== 继电器初始化（上电默认断开） ====================
  relayInit();

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

  xTaskCreatePinnedToCore(
    sensorTask,
    "SensorTask",
    4096,
    NULL,
    1,
    NULL,
    1
  );
}

void loop() {}
