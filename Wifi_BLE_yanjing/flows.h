#ifndef FLOWS_H
#define FLOWS_H

#include "config.h"
#include "flow.h"

// ==================== 预置动作流表 ====================
// 新增流程：加一个步骤数组 + 在 FLOW_TABLE 加一行即可。

static const FlowStep SELFTEST_STEPS[] = {
  {FS_EXPR, "close",    0, 0, 1200},
  {FS_EXPR, "init",     0, 0, 1000},
  {FS_EXPR, "surprise", 0, 0, 1500},
  {FS_EXPR, "smile",    0, 0, 1500},
  {FS_EXPR, "init",     0, 0,  800},
};

static const FlowStep GREET_STEPS[] = {
  {FS_EXPR, "smile",      0, 0, 1500},
  {FS_EXPR, "wink",       0, 0, 1200},
  {FS_EXPR, "left_open",  0, 0, 1000},
  {FS_EXPR, "right_open", 0, 0, 1000},
  {FS_EXPR, "init",       0, 0,  800},
};

static const FlowStep ALERT_STEPS[] = {
  {FS_EXPR, "surprise", 0, 0, 400},
  {FS_EXPR, "blink",    0, 0, 250},
  {FS_EXPR, "surprise", 0, 0, 400},
  {FS_EXPR, "blink",    0, 0, 250},
  {FS_EXPR, "surprise", 0, 0, 500},
};

static const FlowStep SLEEP_STEPS[] = {
  {FS_EXPR, "tired", 0, 0, 1500},
  {FS_EXPR, "close", 0, 0, 1500},
};

static const FlowStep WAKE_STEPS[] = {
  {FS_EXPR, "close", 0, 0, 1200},
  {FS_EXPR, "tired", 0, 0, 1200},
  {FS_EXPR, "init",  0, 0,  800},
  {FS_EXPR, "smile", 0, 0, 1500},
};

static const FlowStep SCAN_STEPS[] = {
  {FS_POSE, "", XARM_LEFT + 12, 90, 900},
  {FS_POSE, "", XARM_RIGHT - 12, 90, 900},
  {FS_POSE, "", (XARM_LEFT + XARM_RIGHT) / 2.0f, 90, 900},
};

static const FlowStep DEMO_STEPS[] = {
  {FS_EXPR, "close",      0, 0, 1000},
  {FS_EXPR, "dis",        0, 0, 1000},
  {FS_EXPR, "surprise",   0, 0, 1200},
  {FS_EXPR, "tired",      0, 0, 1200},
  {FS_EXPR, "smile",      0, 0, 1200},
  {FS_EXPR, "wink",       0, 0, 1200},
  {FS_EXPR, "left_open",  0, 0, 1000},
  {FS_EXPR, "right_open", 0, 0, 1000},
  {FS_EXPR, "blink",      0, 0,  400},
  {FS_EXPR, "init",       0, 0,  800},
};

// scan 的 pose 步骤在表中直接给出坐标（基于 config.h 的眼球行程）

static const FlowDef FLOW_TABLE[] = {
  {"selftest", SELFTEST_STEPS, sizeof(SELFTEST_STEPS) / sizeof(FlowStep), 1, true,  true,  true },
  {"greet",    GREET_STEPS,    sizeof(GREET_STEPS)    / sizeof(FlowStep), 1, true,  true,  true },
  {"alert",    ALERT_STEPS,    sizeof(ALERT_STEPS)    / sizeof(FlowStep), 1, true,  true,  false},
  {"sleep",    SLEEP_STEPS,    sizeof(SLEEP_STEPS)    / sizeof(FlowStep), 1, true,  false, false},
  {"wake",     WAKE_STEPS,     sizeof(WAKE_STEPS)     / sizeof(FlowStep), 1, true,  true,  true },
  {"scan",     SCAN_STEPS,     sizeof(SCAN_STEPS)     / sizeof(FlowStep), 1, true,  true,  true },
  {"demo",     DEMO_STEPS,     sizeof(DEMO_STEPS)     / sizeof(FlowStep), 1, true,  true,  true },
};

static const uint8_t FLOW_TABLE_COUNT = sizeof(FLOW_TABLE) / sizeof(FLOW_TABLE[0]);

#endif // FLOWS_H
