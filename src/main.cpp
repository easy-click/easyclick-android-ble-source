/**
 * @file main.cpp
 * @brief 主程序入口 v4.1
 */

#include <Arduino.h>
#include "ble_hid.h"
#include "config.h"

static unsigned long s_lastDisconnectTime = 0;
static bool s_wantReconnect = false;

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n========================================");
    Serial.println("  EC Touch - BLE HID Firmware v5.1");
    Serial.println("  ESP32-S3 + NimBLE + JSON Protocol");
    Serial.println("========================================");

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, !LED_ON_LEVEL);

    gBleHid.begin();

    Serial.println("[SYS] Setup done");
}

void loop() {
    static unsigned long s_ledBlink = 0;
    static bool s_ledState = false;
    unsigned long now = millis();

    if (gBleHid.isConnected()) {
        digitalWrite(LED_PIN, LED_ON_LEVEL);
    } else {
        if (now - s_ledBlink >= 500) {
            s_ledBlink = now;
            s_ledState = !s_ledState;
            digitalWrite(LED_PIN, s_ledState ? LED_ON_LEVEL : !LED_ON_LEVEL);
        }
    }

    // 断开后延迟恢复广播（如果启用了隐藏）
    static bool s_wasConnected = false;
    if (s_wasConnected && !gBleHid.isConnected()) {
        s_lastDisconnectTime = now;
        s_wantReconnect = true;
        Serial.println("[SYS] Disconnected, will resume advertising");
    }
    s_wasConnected = gBleHid.isConnected();

    if (s_wantReconnect && !gBleHid.isConnected()) {
#if BLE_RECONNECT_TIMEOUT > 0
        if (now - s_lastDisconnectTime >= BLE_RECONNECT_TIMEOUT) {
            gBleHid.startAdvertising();
            s_wantReconnect = false;
            Serial.println("[SYS] Advertising resumed");
        }
#else
        gBleHid.startAdvertising();
        s_wantReconnect = false;
#endif
    }

    delay(100);
}
