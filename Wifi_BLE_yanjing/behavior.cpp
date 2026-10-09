#include "app.h"

// ==================== 行为层（动作流） ====================
// 让眼睛在静止时也在"活着"：自发眨眼 + 微扫视 + 缓慢漂移。
// 这些层永远运行，与表情指令叠加，互不冲突。

#if BEHAVIOR_ENABLED

struct BlinkRuntime {
  bool active;
  unsigned long startMs;
  unsigned long closeMs;
  unsigned long holdMs;
  unsigned long openMs;
  unsigned long totalMs;
};

static BlinkRuntime blink = {false, 0, 0, 0, 0, 0};
static unsigned long blinkNextAt = 0;

static float microOffX = 0.0f, microOffY = 0.0f;
static float microTargetX = 0.0f, microTargetY = 0.0f;
static unsigned long microNextAt = 0;

static float driftX = 0.0f, driftY = 0.0f;
static float driftVelX = 0.0f, driftVelY = 0.0f;

// 轻量 xorshift32 伪随机，避免依赖 esp_random 头文件
static uint32_t rngState = 0x12345678u;

static uint32_t rngNext() {
  uint32_t x = rngState;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  rngState = x;
  return x;
}

static float randUnit() {
  return (float)(rngNext() & 0x00FFFFFF) / 16777215.0f;
}

static unsigned long randRange(unsigned long lo, unsigned long hi) {
  if (hi <= lo) return lo;
  return lo + (rngNext() % (hi - lo + 1));
}

// 眨眼间隔：对数正态抖动，避免机械的周期感
static unsigned long nextBlinkInterval() {
  float jitter = expf((randUnit() - 0.5f) * BLINK_JITTER_SIGMA);
  float v = BLINK_BASE_MS * jitter;
  return (unsigned long)constrain(v, (float)BLINK_MIN_MS, (float)BLINK_MAX_MS);
}

// 眨眼包络：0 = 全睁，1 = 全闭（闭合快、保持、张开慢）
static float blinkFactor(unsigned long now) {
  if (!blink.active) return 0.0f;
  unsigned long e = now - blink.startMs;
  if (e < blink.closeMs) return (float)e / (float)blink.closeMs;
  e -= blink.closeMs;
  if (e < blink.holdMs) return 1.0f;
  e -= blink.holdMs;
  if (e < blink.openMs) return 1.0f - (float)e / (float)blink.openMs;
  return 0.0f;
}

static void startBlink(unsigned long now) {
  blink.closeMs = randRange(BLINK_CLOSE_MIN, BLINK_CLOSE_MAX);
  blink.holdMs  = randRange(BLINK_HOLD_MIN,  BLINK_HOLD_MAX);
  blink.openMs  = randRange(BLINK_OPEN_MIN,  BLINK_OPEN_MAX);
  blink.totalMs = blink.closeMs + blink.holdMs + blink.openMs;
  blink.startMs = now;
  blink.active  = true;
}

void behaviorInit() {
  unsigned long now = millis();

  rngState = ((uint32_t)now * 2654435761u) ^ 0x9E3779B9u;
  if (rngState == 0) rngState = 0x12345678u;

  blink.active  = false;
  blinkNextAt   = now + randRange(800, 2500);   // 开机后稍等再眨眼

  microOffX = microOffY = microTargetX = microTargetY = 0.0f;
  microNextAt = now + randRange(MICRO_MIN_MS, MICRO_MAX_MS);

  driftX = driftY = driftVelX = driftVelY = 0.0f;

  behaviorEyeOffX = 0.0f;
  behaviorEyeOffY = 0.0f;
  behaviorBlink   = 0.0f;
}

void behaviorUpdate(unsigned long now) {
  // ---- 眨眼：自动调度仅在动作流开启时；手动触发的眨眼始终播放 ----
  if (!blink.active && behaviorEnabled && (long)(now - blinkNextAt) >= 0) {
    startBlink(now);
  }
  if (blink.active && (now - blink.startMs) >= blink.totalMs) {
    blink.active = false;
    blinkNextAt  = now + nextBlinkInterval();
  }
  behaviorBlink = blinkFactor(now);

  if (!behaviorEnabled) {
    behaviorEyeOffX = 0.0f;
    behaviorEyeOffY = 0.0f;
    return;
  }

  // ---- 微扫视：注视时偶发的小幅跳视 ----
  if ((long)(now - microNextAt) >= 0) {
    float amp = randUnit() * MICRO_AMP_MAX;
    float ang = randUnit() * 2.0f * (float)M_PI;
    microTargetX = cosf(ang) * amp;
    microTargetY = sinf(ang) * amp;
    microNextAt  = now + randRange(MICRO_MIN_MS, MICRO_MAX_MS);
  }
  // 用时间常数换算成每 tick 系数，与控制节拍解耦
  const float microAlpha = 1.0f - expf(-(float)SERVO_TICK_MS / MICRO_TAU_MS);
  microOffX += (microTargetX - microOffX) * microAlpha;
  microOffY += (microTargetY - microOffY) * microAlpha;

  // ---- 漂移：阻尼弹簧 + 噪声 → 低频慢漂移（取代原先的高频振荡） ----
  driftVelX += (randUnit() - 0.5f) * DRIFT_ACC;
  driftVelX -= driftX * DRIFT_REVERT;
  driftVelX -= driftVelX * DRIFT_DAMP;
  driftX += driftVelX;
  driftX = constrain(driftX, -DRIFT_MAX, DRIFT_MAX);

  driftVelY += (randUnit() - 0.5f) * DRIFT_ACC;
  driftVelY -= driftY * DRIFT_REVERT;
  driftVelY -= driftVelY * DRIFT_DAMP;
  driftY += driftVelY;
  driftY = constrain(driftY, -DRIFT_MAX, DRIFT_MAX);

  // ---- 合成眼球偏移 ----
  behaviorEyeOffX = microOffX + driftX;
  behaviorEyeOffY = microOffY + driftY;
}

// 手动触发一次眨眼（无论动作流是否开启都生效）
void triggerBlink() {
  startBlink(millis());
}

void setBehaviorEnabled(bool on) {
  behaviorEnabled = on;
  if (!on) {
    blink.active    = false;
    behaviorBlink   = 0.0f;
    behaviorEyeOffX = 0.0f;
    behaviorEyeOffY = 0.0f;
  }
}

#else  // BEHAVIOR_ENABLED == 0：退回静态帧行为

void behaviorInit() {}
void behaviorUpdate(unsigned long now) { (void)now; }
void setBehaviorEnabled(bool on) { behaviorEnabled = on; }
void triggerBlink() {}

#endif // BEHAVIOR_ENABLED
