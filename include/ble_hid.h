/**
 * @file ble_hid.h
 * @brief BLE HID 触屏固件 v4.12
 *
 * 协议：
 * - HID Service (0x1812): 数字化仪触屏（Report ID=3，Touch Screen + Finger）
 * - 控制服务: 00001234-0000-1000-8000-00805f9b34fb
 * - 控制特征值: 91721b02-11c1-409d-838c-e69c1187492a (WRITE+WRITE_NR+NOTIFY+INDICATE+READ)
 * - 协议: JSON, "a"字段为指令类型
 *
 * v4.12 核心修复：
 * - 修复swipe函数三次贝塞尔变量残留bug（cp1x/cp1y→cpx/cpy）
 * - 抬起报告使用Indicate（有ACK确认，不会因BLE缓冲区拥塞丢失）
 * - 滑动收尾强化：终点减速停留 + indicate连发抬起 + 安全静默期
 */

#pragma once

#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEHIDDevice.h>
#include <ArduinoJson.h>

#define REPORT_ID_DIGITIZER   3

#define EC_SERVICE_UUID        "00001234-0000-1000-8000-00805f9b34fb"
#define EC_CHAR_UUID           "91721b02-11c1-409d-838c-e69c1187492a"

class BleHidDevice {
public:
    BleHidDevice();
    ~BleHidDevice();

    void begin();
    bool isConnected();
    void stopAdvertising();
    void startAdvertising();
    void disconnect();
    void setScreenSize(int w, int h);

    // 触屏
    void touchDown(int x, int y, int pointer = 0);
    void touchMove(int x, int y, int pointer = 0);
    void touchUp(int pointer = 0);

    // 点击 / 滑动（拟人化）
    void click(int x, int y, uint16_t durationMs = 80);
    void swipe(int x1, int y1, int x2, int y2, uint32_t durationMs = 300);

    // 发送JSON响应
    void sendJsonResponse(const char* status, const char* msg = "");
    void _sendConnectAck();

private:
    NimBLEHIDDevice*   _hid;
    NimBLEServer*      _server;
    NimBLEService*     _ctrlService;
    NimBLECharacteristic* _ctrlChar;
    NimBLECharacteristic* _inputDigitizer;

    bool  _connected;
    uint16_t _connHandle;
    bool  _advertising;
    bool  _bonded;
    int   _screenW;
    int   _screenH;

    int   _touchX;
    int   _touchY;
    bool  _touchActive;
    uint8_t _touchId;

    // 内部发送报告
    void _sendReport(uint8_t contactId, uint8_t tip, uint8_t inRange, int x, int y);
    void _sendLiftReport(uint8_t contactId, int x, int y, int repeats = 4, int gapMs = 20);
    uint16_t _scaleX(int x);
    uint16_t _scaleY(int y);
    void _handleCmd(const uint8_t* data, size_t len);
    void _processJsonCmd(JsonObject root);

    // 狂暴拟人化工具
    int _randGauss(int mean, int stdDev);  // 正态分布随机
    int _randInt(int min, int max);
    float _randFloat(float min, float max);
    uint32_t _randDuration(uint32_t baseMs, float variance = 0.3f);

    friend class BLEServerCallbacksImpl;
    friend class BLESecurityCallbacksImpl;
    friend class CtrlCharCallbacks;
};

extern BleHidDevice gBleHid;
