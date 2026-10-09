#include "app.h"
#include "sensor.h"

// ==================== DHT20 / AHT20 温湿度传感器 ====================
// 协议：I2C 地址 0x38。
//   0x71 读状态；0xBE 初始化；0xAC 触发测量；随后读 7 字节（含 CRC8）。

static float           s_temp = 0.0f;
static float           s_hum  = 0.0f;
static bool            s_valid = false;
static unsigned long   s_ts = 0;
static portMUX_TYPE    s_mux = portMUX_INITIALIZER_UNLOCKED;

// CRC8：多项式 0x31，初值 0xFF（AHT20 数据手册）
static uint8_t dht20Crc8(const uint8_t* data, uint8_t len) {
  uint8_t crc = 0xFF;
  for (uint8_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; b++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

void sensorInit() {
  // 上电后需等待 >100ms 再访问
  delay(150);

  // 读状态寄存器，未校准则执行初始化
  Wire.beginTransmission(DHT20_ADDR);
  Wire.write(0x71);
  if (Wire.endTransmission(false) == 0) {
    if (Wire.requestFrom((uint8_t)DHT20_ADDR, (uint8_t)1) == 1) {
      uint8_t status = Wire.read();
      if ((status & 0x18) != 0x18) {
        Wire.beginTransmission(DHT20_ADDR);
        Wire.write(0xBE);
        Wire.write(0x08);
        Wire.write(0x00);
        Wire.endTransmission();
        delay(10);
      }
    }
  }

  Serial.println("🌡 DHT20 初始化完成");
}

bool dht20Read(float& temperature, float& humidity) {
  // 触发一次测量
  Wire.beginTransmission(DHT20_ADDR);
  Wire.write(0xAC);
  Wire.write(0x33);
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;

  delay(80);  // 测量时间约 80ms

  if (Wire.requestFrom((uint8_t)DHT20_ADDR, (uint8_t)7) != 7) return false;

  uint8_t d[7];
  for (uint8_t i = 0; i < 7; i++) d[i] = Wire.read();

  if (d[0] & 0x80) return false;              // 忙碌
  if (dht20Crc8(d, 6) != d[6]) return false;  // CRC 校验

  uint32_t rawHum = ((uint32_t)d[1] << 12) | ((uint32_t)d[2] << 4) | (d[3] >> 4);
  uint32_t rawTmp = ((uint32_t)(d[3] & 0x0F) << 16) | ((uint32_t)d[4] << 8) | d[5];

  humidity    = rawHum * 100.0f / 1048576.0f;
  temperature = rawTmp * 200.0f / 1048576.0f - 50.0f;
  return true;
}

bool sensorGet(float& temperature, float& humidity, bool& valid, unsigned long& ageMs) {
  portENTER_CRITICAL(&s_mux);
  temperature = s_temp;
  humidity    = s_hum;
  valid       = s_valid;
  ageMs       = (s_ts != 0) ? (millis() - s_ts) : 0;
  portEXIT_CRITICAL(&s_mux);
  return valid;
}

// ==================== 采集任务（core1，低优先级，不干扰舵机实时任务） ====================
// DHT20 每 DHT20_READ_MS 采样一次并更新缓存；继电器每 RELAY_EVAL_MS 用缓存快速评估，
// 避免"读取周期 + 防抖"叠加造成反应迟钝。
void sensorTask(void* pvParameters) {
  sensorInit();

  TickType_t lastWake = xTaskGetTickCount();
  unsigned long lastRead = 0;

  while (1) {
    unsigned long now = millis();
    bool newReading = false;

    // ---- 到期才真正读一次传感器（DHT20 测量约 80ms） ----
    if (now - lastRead >= DHT20_READ_MS) {
      lastRead = now;

      float t, h;
      if (dht20Read(t, h)) {
        portENTER_CRITICAL(&s_mux);
        s_temp  = t;
        s_hum   = h;
        s_valid = true;
        s_ts    = millis();
        portEXIT_CRITICAL(&s_mux);

        Serial.printf("🌡 DHT20  温度: %.1f °C  湿度: %.1f %%RH\n", t, h);
        newReading = true;
      } else {
        portENTER_CRITICAL(&s_mux);
        s_valid = false;
        portEXIT_CRITICAL(&s_mux);
        Serial.println("⚠️ DHT20 读取失败");
      }
    }

    // ---- 继电器快速评估：始终用最新缓存温度，RELAY_EVAL_MS 节拍 ----
    float t = 0.0f, h = 0.0f;
    bool valid = false;
    unsigned long age = 0;
    sensorGet(t, h, valid, age);
    relayUpdate(now, t, valid && age <= RELAY_SENSOR_STALE_MS);

    // ---- 有新读数时上报（此时继电器状态已更新，保证遥测准确） ----
    if (newReading) {
      char buf[128];
      relayFormatTelemetry(t, h, buf, sizeof(buf));
      markTelemetry(buf);
    }

    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(RELAY_EVAL_MS));
  }
}
