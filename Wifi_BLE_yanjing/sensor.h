#ifndef SENSOR_H
#define SENSOR_H

#include <Arduino.h>

// ==================== 环境传感器（DHT20 / AHT20） ====================
void sensorInit();
bool dht20Read(float& temperature, float& humidity);
bool sensorGet(float& temperature, float& humidity, bool& valid, unsigned long& ageMs);
void sensorTask(void* pvParameters);

#endif // SENSOR_H
