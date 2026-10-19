# easyclick-android-ble-source
EasyClick 蓝牙HID源码，实现自动化点击，下载自己编译

EasyClick 主打安卓脚本、苹果脚本、鸿蒙脚本三大手机系统自动化，还有苹果群控、安卓群控、iEasyRun智能体服务。
苹果群控可以打字、上传视频、同步操作、批量操作手机登的。苹果USBHID技术可以过任何APP风控，对于跨境电商、TikTok群控绝对是好帮手！

官网 https://ieasyclick.com
AI群控手机产品: https://ieasyrun.com


# ESP32-S3 BLE HID 触屏固件 v4.1

## 功能
- BLE HID 触屏（数字化仪 Touch Screen + Finger，Report ID=3）
- JSON 控制协议（服务 1234，特征值 9172）
- 坐标自动缩放（传像素坐标，固件转 0-32767）
- 绑定持久化（BLE Bonding + NVS）
- MAC 唯一命名（最后4字节小写8位）
- 可选：连接后隐藏蓝牙名（防串信号）

## 编译
```bash
pio run
```

## 烧录
```bash
pio run -t upload
```

## 串口监控
```bash
pio device monitor -b 115200
```

## 控制协议（JSON）

所有指令通过特征值 `91721b02-...` 写入，支持 WRITE 和 WRITE_NR。

### 触屏指令
```json
{"a":"td","x":500,"y":800}     // 按下
{"a":"tm","x":500,"y":800}     // 移动
{"a":"tu"}                      // 抬起
{"a":"db","x":500,"y":800}     // 双击
{"a":"sw","fx":100,"fy":300,"tx":200,"ty":800,"d":300}  // 滑动
```

### 系统键（手势模拟）
```json
{"a":"hbe"}  // Home（底部上滑）
{"a":"sbe"}  // Back（左边缘右滑）
```

### 其他
```json
{"a":"rst"}  // 重启
{"a":"cn","v":1}  // 握手（响应同格式）
```

## 设备命名
MAC 最后4字节小写十六进制，共8位。如 MAC=1C:DB:D4:5B:CD:29，则名称为 `d45bcd29`。

## 绑定
- Just Works 配对，无需 PIN 码
- 绑定信息存 NVS，重启后自动重连

## 防串信号
- 每台设备蓝牙名唯一（基于 MAC）
- 配置 `BLE_HIDE_AFTER_CONNECT=1` 可连接后隐藏广播
