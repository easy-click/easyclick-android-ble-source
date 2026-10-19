/**
 * @file config.h
 * @brief 配置文件 v4.1
 */

#pragma once

// 蓝牙名称配置
#define BLE_NAME_PREFIX     ""
#define USE_MAC_AS_BLE_NAME  1    // 1=用MAC生成名称，0=用固定名称
#define BLE_DEVICE_NAME     "ec-touch"
#define BLE_NAME_USE_LAST8   1    // 1=取MAC最后4字节8位
#define BLE_NAME_UPPERCASE   0    // 1=大写，0=小写

// 隐藏蓝牙名（防串信号/防检测）
#define BLE_HIDE_AFTER_CONNECT  1   // 1=连接后停止广播，0=调试用一直广播
#define BLE_RECONNECT_TIMEOUT   30000  // 断开后多久恢复广播(ms)

// 默认屏幕尺寸（坐标自动缩放 0-32767）
#define DEFAULT_SCREEN_W    1080
#define DEFAULT_SCREEN_H    2400

// LED引脚
#define LED_PIN             48
#define LED_ON_LEVEL        0    // 低电平点亮
