#ifndef RELAY_H
#define RELAY_H

#include <Arduino.h>

// ==================== 继电器 + 温度联动 ====================
// 高电平/低电平触发由 RELAY_ACTIVE_HIGH 决定；温度高于阈值启动，降温回落后关闭。
void relayInit();

// 手动控制（网页 / MQTT / BLE / MCP）。on=true 人工开启，false 人工关闭（人工优先）。
void relaySet(bool on, const char* reason);

// 自动温控总开关
void relaySetAuto(bool on);
bool relayGetAuto();

// 温度阈值（°C）
void relaySetThreshold(float t);
float relayGetThreshold();

// 周期评估：在采集任务里用最新温度调用
void relayUpdate(unsigned long now, float temperature, bool valid);

// 状态查询
bool relayGet();
const char* relayGetReason();
String relayStatusJson();

// 遥测 JSON：供传感器任务与命令回执共用
void relayFormatTelemetry(float temperature, float humidity, char* out, size_t outSize);
void relayAnnounce();

#endif // RELAY_H
