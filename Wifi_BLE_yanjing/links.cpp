#include "app.h"

// ==================== 蓝牙 BLE（Nordic UART Service） ====================
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

  for (int i = 0; i < (int)len; i++) {
    cmd += (char)payload[i];
  }

  cmd.trim();
  dispatchCommand(cmd);
}
