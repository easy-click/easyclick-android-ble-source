/**
 * @file ble_hid.cpp
 * @brief BLE HID 触屏固件 v5.1
 *
 * v5.1 微调：
 * - 滑动贝塞尔弧度从 ±8% 收窄到 ±3.5%，长距离不会跑偏
 * - 起终点偏移从 ±3/±4px 收窄到 ±2px
 * - 途中抖动从 1.0/0.5px 收窄到 0.6/0.3px
 * 
 * v5.0 核心修复（滑动后手指抬不起来 / 指针十字架停在终点）：
 * - 定位根因：NimBLE-Arduino 1.4.1 的 notify() 底层
 *   ble_gattc_notify_custom() 返回值被丢弃，缓冲满时帧静默丢失、
 *   不阻塞不重试。滑动notify多 → 最后一帧抬起被丢；点击包少不丢。
 * - 去掉"悬停帧(Tip=0,InRange=1)"：部分厂商内核进入hover状态机后
 *   触点不释放。抬起直接 Tip=0 & InRange=0（与商用参考固件一致）。
 * - 抬起帧保留 ContactID + Confidence，并跨多个BLE连接事件重发，
 *   保证至少一帧送达；click/swipe/touchUp 统一走 _sendLiftReport。
 * - 描述符维持与商用参考固件逐字节一致（102字节，5字节报告），
 *   不引入Contact Count（参考固件同样没有且工作正常）。
 */

#include "ble_hid.h"
#include "config.h"
#include <esp_mac.h>

static char s_deviceName[32] = BLE_DEVICE_NAME;
BleHidDevice gBleHid;

// ============================================================
//  HID 报告描述符 - Touch Screen + Finger + Report ID=3
//  参考商用固件格式，1:1位对齐
//  报告结构（5字节，不含Report ID，NimBLE按特征值区分）:
//    Byte0: bit0=TipSwitch, bit1=InRange, bits2-6=ContactID(5bit), bit7=Confidence
//    Byte1-2: X (16位 LE, 0-4095)
//    Byte3-4: Y (16位 LE, 0-4095)
// ============================================================
static const uint8_t hidReportMap[] = {
    0x05, 0x0D,        // Usage Page (Digitizer)
    0x09, 0x04,        // Usage (Touch Screen)
    0xA1, 0x01,        // Collection (Application)
      0x85, REPORT_ID_DIGITIZER,  //   Report ID (3)
      0x09, 0x22,      //   Usage (Finger)
      0xA1, 0x00,      //   Collection (Physical)
        0x09, 0x42,    //     Usage (Tip Switch)
        0x15, 0x00,    //     Logical Min (0)
        0x25, 0x01,    //     Logical Max (1)
        0x75, 0x01,    //     Report Size (1)
        0x95, 0x01,    //     Report Count (1)
        0x81, 0x02,    //     Input (Data,Var,Abs)
        0x09, 0x32,    //     Usage (In Range)
        0x15, 0x00,    //     Logical Min (0)
        0x25, 0x01,    //     Logical Max (1)
        0x81, 0x02,    //     Input (Data,Var,Abs)
        0x09, 0x51,    //     Usage (Contact Identifier)
        0x75, 0x05,    //     Report Size (5)
        0x95, 0x01,    //     Report Count (1)
        0x16, 0x00, 0x00,  //   Logical Min (0)
        0x26, 0x10, 0x00,  //   Logical Max (16)
        0x81, 0x02,    //     Input (Data,Var,Abs)
        0x09, 0x47,    //     Usage (Confidence)
        0x75, 0x01,    //     Report Size (1)
        0x95, 0x01,    //     Report Count (1)
        0x15, 0x00,    //     Logical Min (0)
        0x25, 0x01,    //     Logical Max (1)
        0x81, 0x02,    //     Input (Data,Var,Abs)
        0x05, 0x01,    //     Usage Page (Generic Desktop)
        0x09, 0x30,    //     Usage (X)
        0x75, 0x10,    //     Report Size (16)
        0x95, 0x01,    //     Report Count (1)
        0x55, 0x0D,    //     Unit Exponent (-3, 即 0.1cm = 1mm)
        0x65, 0x33,    //     Unit (SI Linear, cm)
        0x35, 0x00,    //     Physical Min (0)
        0x46, 0x60, 0x17,  // Physical Max (6000 = 6.0cm, 参考值)
        0x26, 0xFF, 0x0F,  // Logical Max (4095)
        0x81, 0x02,    //     Input (Data,Var,Abs)
        0x09, 0x31,    //     Usage (Y)
        0x75, 0x10,    //     Report Size (16)
        0x95, 0x01,    //     Report Count (1)
        0x55, 0x0D,    //     Unit Exponent (-3)
        0x65, 0x33,    //     Unit (SI Linear, cm)
        0x35, 0x00,    //     Physical Min (0)
        0x46, 0x26, 0x0E,  // Physical Max (3622 = 3.622cm, 参考值)
        0x26, 0xFF, 0x0F,  // Logical Max (4095)
        0x81, 0x02,    //     Input (Data,Var,Abs)
      0xC0,            //   End Collection
    0xC0,              // End Collection
};

// ============================================================
//  服务器连接回调
// ============================================================
class BLEServerCallbacksImpl : public NimBLEServerCallbacks {
    BleHidDevice* _dev;
public:
    BLEServerCallbacksImpl(BleHidDevice* dev) : _dev(dev) {}

    void onConnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) override {
        _dev->_connected = true;
        _dev->_connHandle = desc->conn_handle;
        Serial.printf("[BLE] Client connected, handle=%d\n", desc->conn_handle);

#if BLE_HIDE_AFTER_CONNECT
        _dev->stopAdvertising();
#endif
    }

    void onDisconnect(NimBLEServer* pServer) override {
        _dev->_connected = false;
        _dev->_connHandle = 0xFFFF;
        _dev->_bonded = false;
        Serial.println("[BLE] Client disconnected");

        // 延迟恢复广播（重连窗口）
#if BLE_RECONNECT_TIMEOUT > 0
        delay(BLE_RECONNECT_TIMEOUT);
#endif
        if (!_dev->_connected) {
            _dev->startAdvertising();
        }
    }
};

// ============================================================
//  安全绑定回调
// ============================================================
class BLESecurityCallbacksImpl : public NimBLESecurityCallbacks {
    BleHidDevice* _dev;
public:
    BLESecurityCallbacksImpl(BleHidDevice* dev) : _dev(dev) {}

    uint32_t onPassKeyRequest() override { return 0; }
    void onPassKeyNotify(uint32_t pass_key) override {}
    bool onConfirmPIN(uint32_t pass_key) override { return true; }
    bool onSecurityRequest() override { return true; }

    void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
        if (desc->sec_state.bonded) {
            _dev->_bonded = true;
            Serial.println("[SEC] ✅ Bonded!");
        }
    }
};

// ============================================================
//  控制特征值回调
// ============================================================
class CtrlCharCallbacks : public NimBLECharacteristicCallbacks {
    BleHidDevice* _dev;
public:
    CtrlCharCallbacks(BleHidDevice* dev) : _dev(dev) {}

    void onWrite(NimBLECharacteristic* pCharacteristic) override {
        std::string value = pCharacteristic->getValue();
        if (value.empty()) return;

        int len = value.length();
        char buf[256];
        int copyLen = len < (int)sizeof(buf)-1 ? len : (int)sizeof(buf)-1;
        memcpy(buf, value.c_str(), copyLen);
        buf[copyLen] = 0;
        Serial.printf("[CMD] %s\n", buf);

        _dev->_handleCmd((const uint8_t*)value.c_str(), len);
    }

    void onSubscribe(NimBLECharacteristic* pCharacteristic,
                     ble_gap_conn_desc* desc, uint16_t subValue) override {
        Serial.printf("[SUB] subscribed=%d\n", subValue);
        if (subValue) {
            _dev->_sendConnectAck();
        }
    }
};

// ============================================================
//  构造 / 析构
// ============================================================
BleHidDevice::BleHidDevice()
    : _hid(nullptr), _server(nullptr),
      _ctrlService(nullptr), _ctrlChar(nullptr),
      _inputDigitizer(nullptr),
      _connected(false), _connHandle(0xFFFF), _advertising(false), _bonded(false),
      _screenW(DEFAULT_SCREEN_W), _screenH(DEFAULT_SCREEN_H),
      _touchX(0), _touchY(0), _touchActive(false), _touchId(0)
{
    uint32_t seed = esp_random();
    srand(seed);
}

BleHidDevice::~BleHidDevice() {}

// ============================================================
//  初始化
// ============================================================
void BleHidDevice::begin() {
    // 1. 生成设备名
#if USE_MAC_AS_BLE_NAME
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
#if BLE_NAME_USE_LAST8
    snprintf(s_deviceName, sizeof(s_deviceName),
#if BLE_NAME_UPPERCASE
             "%s%02X%02X%02X%02X",
#else
             "%s%02x%02x%02x%02x",
#endif
             BLE_NAME_PREFIX, mac[2], mac[3], mac[4], mac[5]);
#else
    snprintf(s_deviceName, sizeof(s_deviceName), "%s%02X%02X%02X",
             BLE_NAME_PREFIX, mac[3], mac[4], mac[5]);
#endif
    Serial.printf("[BLE] BT MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
#endif
    Serial.printf("[BLE] Device name: %s\n", s_deviceName);

    // 2. 初始化 NimBLE
    NimBLEDevice::init(s_deviceName);
    Serial.println("[BLE] NimBLE init done");

    // 3. 配置安全（绑定）
    NimBLEDevice::setSecurityAuth(true, true, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
    NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
    NimBLEDevice::setSecurityCallbacks(new BLESecurityCallbacksImpl(this));
    Serial.println("[BLE] Security configured");

    // 4. 创建Server
    _server = NimBLEDevice::createServer();
    _server->setCallbacks(new BLEServerCallbacksImpl(this));
    Serial.println("[BLE] Server created");

    // 5. 创建HID设备
    _hid = new NimBLEHIDDevice(_server);
    Serial.println("[BLE] HID device created");

    _hid->manufacturer()->setValue("EC Touch");
    _hid->pnp(0x02, 0x1234, 0x5678, 0x0100);
    _hid->hidInfo(0x00, 0x00);

    _hid->reportMap((uint8_t*)hidReportMap, sizeof(hidReportMap));

    // 创建inputReport，ID=3
    // NimBLEHIDDevice的inputReport每个Report ID是独立特征值，
    // setValue不需要包含Report ID前缀
    _inputDigitizer = _hid->inputReport(REPORT_ID_DIGITIZER);

    _hid->startServices();
    Serial.println("[BLE] HID services started");

    // 6. 创建控制服务 + 特征值
    _ctrlService = _server->createService(EC_SERVICE_UUID);

    _ctrlChar = _ctrlService->createCharacteristic(
        EC_CHAR_UUID,
        NIMBLE_PROPERTY::READ |
        NIMBLE_PROPERTY::WRITE |
        NIMBLE_PROPERTY::WRITE_NR |
        NIMBLE_PROPERTY::NOTIFY |
        NIMBLE_PROPERTY::INDICATE
    );
    _ctrlChar->setCallbacks(new CtrlCharCallbacks(this));
    _ctrlChar->setValue("ready");

    _ctrlService->start();
    Serial.println("[BLE] Control service started");

    // 7. 配置广播
    NimBLEAdvertising* adv = _server->getAdvertising();
    adv->setName(s_deviceName);
    adv->addServiceUUID(EC_SERVICE_UUID);
    adv->addServiceUUID("1812");
    adv->setAppearance(0x03C0);  // Digitizer Touch Screen

    Serial.println("[BLE] Advertising configured");

    // 8. 开始广播
    adv->start();
    _advertising = true;

    Serial.printf("[BLE] ✅ Ready! Name: %s (v5.0)\n", s_deviceName);
}

// ============================================================
//  状态控制
// ============================================================
bool BleHidDevice::isConnected() {
    return _connected;
}

void BleHidDevice::stopAdvertising() {
    if (_advertising) {
        NimBLEDevice::getAdvertising()->stop();
        _advertising = false;
        Serial.println("[BLE] Advertising stopped");
    }
}

void BleHidDevice::startAdvertising() {
    if (!_advertising && !_connected) {
        NimBLEDevice::getAdvertising()->start();
        _advertising = true;
        Serial.println("[BLE] Advertising started");
    }
}

void BleHidDevice::disconnect() {
    if (_server && _connected && _connHandle != 0xFFFF) {
        _server->disconnect(_connHandle, 0x13);
    }
}

void BleHidDevice::setScreenSize(int w, int h) {
    _screenW = w;
    _screenH = h;
    Serial.printf("[CFG] Screen: %dx%d\n", w, h);
}

// ============================================================
//  狂暴拟人化工具函数
// ============================================================
int BleHidDevice::_randInt(int min, int max) {
    if (min >= max) return min;
    return min + rand() % (max - min + 1);
}

float BleHidDevice::_randFloat(float min, float max) {
    float r = (float)rand() / (float)RAND_MAX;
    return min + r * (max - min);
}

int BleHidDevice::_randGauss(int mean, int stdDev) {
    float u1 = _randFloat(0.001f, 1.0f);
    float u2 = _randFloat(0.0f, 1.0f);
    float z = sqrtf(-2.0f * logf(u1)) * cosf(2.0f * 3.14159265f * u2);
    return (int)(mean + z * stdDev);
}

uint32_t BleHidDevice::_randDuration(uint32_t baseMs, float variance) {
    float r = _randFloat(1.0f - variance, 1.0f + variance);
    int v = (int)((float)baseMs * r);
    if (v < 20) v = 20;
    return (uint32_t)v;
}

// ============================================================
//  内部：构造5字节HID报告数据（不含Report ID）
//  参考商用固件位布局：
//    Byte0: bit0=TipSwitch, bit1=InRange, bits2-6=ContactID(5bit), bit7=Confidence
//    Byte1-2: X (16位 LE)
//    Byte3-4: Y (16位 LE)
// ============================================================
static void _buildReport(uint8_t* buf, uint8_t contactId, uint8_t tip, uint8_t inRange,
                         uint16_t scaledX, uint16_t scaledY) {
    // Byte0: bit0=Tip, bit1=InRange, bits2-6=ContactID, bit7=Confidence
    // Confidence: 只要contactId>0就为1——表示"这个接触点ID的数据是可信的"
    // 包括抬起状态（Tip=0, InRange=0），否则安卓可能因为Confidence=0而忽略抬起报告
    uint8_t byte0 = 0;
    if (tip) byte0 |= 0x01;
    if (inRange) byte0 |= 0x02;
    byte0 |= ((contactId & 0x1F) << 2);  // Contact ID占5位
    if (contactId > 0) byte0 |= 0x80;   // 有有效Contact ID时Confidence=1
    buf[0] = byte0;

    // X (16位 LE)
    buf[1] = scaledX & 0xFF;
    buf[2] = (scaledX >> 8) & 0xFF;

    // Y (16位 LE)
    buf[3] = scaledY & 0xFF;
    buf[4] = (scaledY >> 8) & 0xFF;
}

// ============================================================
//  内部：可靠发送Notify（通过节奏控制+重发确保送达）
// ============================================================
static void _notifySafe(NimBLECharacteristic* chr, const uint8_t* data, size_t len) {
    chr->setValue(data, len);
    chr->notify();
    // 短暂等待让BLE堆栈处理，防止缓冲区溢出
    vTaskDelay(2 / portTICK_PERIOD_MS);
}

void BleHidDevice::_sendReport(uint8_t contactId, uint8_t tip, uint8_t inRange, int x, int y) {
    if (!_inputDigitizer || !_connected) return;

    uint8_t report[5];
    _buildReport(report, contactId, tip, inRange, _scaleX(x), _scaleY(y));
    _notifySafe(_inputDigitizer, report, sizeof(report));
}

// 抬起专用（v5.0）：
// 关键修复依据：
//  1) NimBLE-Arduino 1.4.1 的 notify() 底层 ble_gattc_notify_custom()
//     返回值被丢弃，缓冲满时该帧静默丢失、不阻塞、不重试。
//     滑动产生大量notify，最后一帧最容易被丢；点击包少所以不丢。
//  2) 不再发"悬停帧(Tip=0,InRange=1)"。参考商用固件/标准HID设备抬起时
//     直接 Tip=0 & InRange=0。悬停态会让部分厂商内核进入hover状态机，
//     反而造成触点不释放。
//  3) ContactID保持不变(=1)、Confidence=1，让内核Type-B正确关闭同一slot。
//  4) 跨多个BLE连接事件重发(每次间隔≥一个CI)，保证至少一帧送达。
void BleHidDevice::_sendLiftReport(uint8_t contactId, int x, int y, int repeats, int gapMs) {
    if (!_inputDigitizer || !_connected) return;

    uint8_t report[5];
    // 直接完全抬起：Tip=0, InRange=0（保留contactId，Confidence=1）
    _buildReport(report, contactId, 0, 0, _scaleX(x), _scaleY(y));

    for (int i = 0; i < repeats; i++) {
        _inputDigitizer->setValue(report, sizeof(report));
        _inputDigitizer->notify();
        vTaskDelay(gapMs / portTICK_PERIOD_MS);
    }
}

// ============================================================
//  触屏控制（原始接口）
// ============================================================
void BleHidDevice::touchDown(int x, int y, int pointer) {
    _touchX = x;
    _touchY = y;
    _touchActive = true;
    _touchId = pointer + 1;
    _sendReport(_touchId, 1, 1, x, y);
    Serial.printf("[TOUCH] down (%d,%d) id=%d\n", x, y, _touchId);
}

void BleHidDevice::touchMove(int x, int y, int pointer) {
    _touchX = x;
    _touchY = y;
    if (_touchActive && _touchId > 0) {
        _sendReport(_touchId, 1, 1, x, y);
    }
}

void BleHidDevice::touchUp(int pointer) {
    if (!_touchActive || _touchId == 0) return;
    uint8_t id = _touchId;
    int lastX = _touchX;
    int lastY = _touchY;

    _touchActive = false;
    _touchId = 0;
    _touchX = 0;
    _touchY = 0;

    // v5.0：不经过悬停，直接 Tip=0 & InRange=0，跨多个连接事件重发
    _sendLiftReport(id, lastX, lastY, 4, 20);
    Serial.println("[TOUCH] up");
}

// ============================================================
//  狂暴拟人化点击
// ============================================================
void BleHidDevice::click(int x, int y, uint16_t durationMs) {
    // 落点正态分布偏移
    int px = _randGauss(x, 5);
    int py = _randGauss(y, 5);

    // 抬起点略有不同
    int rx = px + _randGauss(0, 2);
    int ry = py + _randGauss(0, 2);

    // 按压时长
    uint16_t baseDur = durationMs > 0 ? durationMs : 120;
    int dur = _randGauss(baseDur, 25);
    if (dur < 40) dur = 40;
    if (dur > 250) dur = 250;

    // 按下
    _sendReport(1, 1, 1, px, py);

    // 按下中期微小位移
    int midSteps = _randInt(1, 2);
    int midDelay = dur / (midSteps + 1);
    for (int i = 0; i < midSteps; i++) {
        vTaskDelay(midDelay / portTICK_PERIOD_MS);
        int mx = px + _randGauss(0, 1);
        int my = py + _randGauss(0, 1);
        _sendReport(1, 1, 1, mx, my);
    }
    vTaskDelay(midDelay / portTICK_PERIOD_MS);

    // v5.0：直接抬起（不悬停），跨多个连接事件重发
    _sendLiftReport(1, rx, ry, 3, 20);

    Serial.printf("[TOUCH] click (%d,%d) dur=%dms actual=(%d,%d)->(%d,%d)\n",
                  x, y, dur, px, py, rx, ry);
}

// ============================================================
//  狂暴拟人化滑动
//  - 二次贝塞尔曲线轨迹
//  - ease-in-out 速度曲线
//  - 每步随机抖动
//  - v5.0：终点直接Tip/InRange归零抬起（不悬停），跨连接事件重发防丢帧
// ============================================================
void BleHidDevice::swipe(int x1, int y1, int x2, int y2, uint32_t durationMs) {
    // 总时长随机 ±20%
    uint32_t totalDur = _randDuration(durationMs, 0.20f);

    // 步数：最少5步，最多15步（减少BLE压力，诊断用）
    int dx = x2 - x1;
    int dy = y2 - y1;
    float dist = sqrtf((float)(dx*dx + dy*dy));
    int baseSteps = (int)(dist / 60.0f) + 3;
    if (baseSteps < 5) baseSteps = 5;
    if (baseSteps > 15) baseSteps = 15;  // 最多15步

    uint32_t stepDelay = totalDur / baseSteps;
    if (stepDelay < 15) stepDelay = 15;  // 最少15ms一步

    // 二次贝塞尔曲线控制点（弧度收窄，长距离不会跑偏太多）
    float angle = atan2f((float)dy, (float)dx);
    float perpAngle = angle + 3.14159265f / 2.0f;
    float curveAmount = _randFloat(-0.035f, 0.035f);
    float curveDist = curveAmount * dist;
    int midX = (x1 + x2) / 2;
    int midY = (y1 + y2) / 2;
    int cpx = midX + (int)(cosf(perpAngle) * curveDist);
    int cpy = midY + (int)(sinf(perpAngle) * curveDist);

    // 起点随机偏移
    int sx = _randGauss(x1, 2);
    int sy = _randGauss(y1, 2);

    // 终点随机偏移（收窄，避免停的位置比目标远太多）
    int ex = _randGauss(x2, 2);
    int ey = _randGauss(y2, 2);

    // ====== 按下起点 ======
    _sendReport(1, 1, 1, sx, sy);
    vTaskDelay(_randDuration(40, 0.3f) / portTICK_PERIOD_MS);

    // ====== 移动 - 二次贝塞尔 + ease-out + 末端额外减速 ======
    for (int i = 1; i <= baseSteps - 1; i++) {  // 最后一步单独处理
        float t = (float)i / baseSteps;

        // ease-out cubic（末端特别慢，模拟手指减速停下）
        // eased = 1 - (1-t)^3
        float inv = 1.0f - t;
        float eased = 1.0f - inv * inv * inv;

        // 二次贝塞尔曲线: B(t) = (1-t)²*P0 + 2(1-t)t*P1 + t²*P2
        float mt = eased;
        float inv_t = 1.0f - mt;
        float b0 = inv_t * inv_t;
        float b1 = 2.0f * inv_t * mt;
        float b2 = mt * mt;

        float bx = b0 * sx + b1 * cpx + b2 * ex;
        float by = b0 * sy + b1 * cpy + b2 * ey;

        // 每步随机抖动（末端抖动更小）
        float jitterSigma = (i < baseSteps - 3) ? 0.6f : 0.3f;
        int jx = _randGauss(0, (int)jitterSigma);
        int jy = _randGauss(0, (int)jitterSigma);

        int px = (int)bx + jx;
        int py = (int)by + jy;

        _sendReport(1, 1, 1, px, py);

        // 最后3步额外放慢（模拟手指即将停下的减速感）
        if (i >= baseSteps - 3) {
            vTaskDelay((stepDelay * 2) / portTICK_PERIOD_MS);
        } else {
            vTaskDelay(stepDelay / portTICK_PERIOD_MS);
        }
    }

    // ====== 终点最后一步 ======
    _sendReport(1, 1, 1, ex, ey);
    vTaskDelay(80 / portTICK_PERIOD_MS);  // 长等待，彻底排空缓冲区

    // ====== 抬起（v5.0：解决十字架停在终点） ======
    // 不经过悬停(Tip=0,InRange=1)，直接 Tip=0 & InRange=0；
    // 跨多个BLE连接事件重发，保证至少一帧送达（NimBLE缓冲满会静默丢帧）
    _sendLiftReport(1, ex, ey, 6, 25);

    // 安全静默期
    vTaskDelay(30 / portTICK_PERIOD_MS);

    Serial.printf("[TOUCH] swipe (%d,%d)->(%d,%d) steps=%d curve=%.0fpx\n",
                  x1, y1, x2, y2, baseSteps, curveDist);
}

// ============================================================
//  JSON响应
// ============================================================
void BleHidDevice::sendJsonResponse(const char* status, const char* msg) {
    if (!_connected || !_ctrlChar) return;

    char buf[128];
    if (msg && *msg) {
        snprintf(buf, sizeof(buf), "{\"s\":\"%s\",\"m\":\"%s\"}", status, msg);
    } else {
        snprintf(buf, sizeof(buf), "{\"s\":\"%s\"}", status);
    }
    _ctrlChar->setValue(buf);
    _ctrlChar->notify();
}

void BleHidDevice::_sendConnectAck() {
    if (!_ctrlChar) return;
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"a\":\"cn\",\"v\":1,\"mac\":\"%s\",\"name\":\"%s\"}",
             s_deviceName, s_deviceName);
    _ctrlChar->setValue(buf);
    _ctrlChar->notify();
}

// ============================================================
//  坐标缩放
// ============================================================
uint16_t BleHidDevice::_scaleX(int x) {
    if (x < 0) x = 0;
    if (x > _screenW) x = _screenW;
    return (uint16_t)((int32_t)x * 4095 / _screenW);
}

uint16_t BleHidDevice::_scaleY(int y) {
    if (y < 0) y = 0;
    if (y > _screenH) y = _screenH;
    return (uint16_t)((int32_t)y * 4095 / _screenH);
}

// ============================================================
//  指令处理
// ============================================================
void BleHidDevice::_handleCmd(const uint8_t* data, size_t len) {
    if (len == 0) return;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, data, len);
    if (err) {
        Serial.println("[CMD] Not valid JSON");
        return;
    }

    _processJsonCmd(doc.as<JsonObject>());
}

void BleHidDevice::_processJsonCmd(JsonObject root) {
    const char* action = root["a"] | "";
    Serial.printf("[CMD] action=%s\n", action);

    if (strcmp(action, "td") == 0) {
        int x = root["x"] | 0;
        int y = root["y"] | 0;
        int p = root["p"] | 0;
        touchDown(x, y, p);
    }
    else if (strcmp(action, "tm") == 0) {
        int x = root["x"] | 0;
        int y = root["y"] | 0;
        int p = root["p"] | 0;
        touchMove(x, y, p);
    }
    else if (strcmp(action, "tu") == 0) {
        int p = root["p"] | 0;
        touchUp(p);
    }
    else if (strcmp(action, "db") == 0) {
        int x = root["x"] | 0;
        int y = root["y"] | 0;
        uint16_t d = root["d"] | 80;
        click(x, y, d);
    }
    else if (strcmp(action, "dbl") == 0) {
        int x = root["x"] | 0;
        int y = root["y"] | 0;
        click(x, y, 60);
        vTaskDelay(120 / portTICK_PERIOD_MS);
        click(x, y, 60);
    }
    else if (strcmp(action, "sw") == 0) {
        int fx = root["fx"] | 0;
        int fy = root["fy"] | 0;
        int tx = root["tx"] | 0;
        int ty = root["ty"] | 0;
        uint32_t d = root["d"] | 300;
        swipe(fx, fy, tx, ty, d);
    }
    else if (strcmp(action, "hbe") == 0) {
        int midX = _screenW / 2;
        int startY = _screenH - 30;
        int endY = _screenH * 2 / 3;
        swipe(midX, startY, midX, endY, 250);
        Serial.println("[CMD] HOME (gesture)");
    }
    else if (strcmp(action, "sbe") == 0) {
        int midY = _screenH / 2;
        swipe(10, midY, _screenW / 4, midY, 200);
        Serial.println("[CMD] BACK (gesture)");
    }
    else if (strcmp(action, "sr") == 0) {
        int w = root["w"] | DEFAULT_SCREEN_W;
        int h = root["h"] | DEFAULT_SCREEN_H;
        setScreenSize(w, h);
        sendJsonResponse("ok");
    }
    else if (strcmp(action, "rst") == 0) {
        sendJsonResponse("ok", "restarting");
        vTaskDelay(200 / portTICK_PERIOD_MS);
        ESP.restart();
    }
    else if (strcmp(action, "cn") == 0) {
        _sendConnectAck();
    }
    else {
        Serial.printf("[CMD] unknown action: %s\n", action);
        sendJsonResponse("error", "unknown action");
    }
}
