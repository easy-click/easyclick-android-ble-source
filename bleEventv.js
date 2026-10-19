// =============================================================================
// 自研BLE触屏驱动 - EC兼容版 v1.3 (自动查找配对设备)
// 完全兼容EC原生 bleEvent API，原代码无需修改
// =============================================================================

try { importPackage(android.bluetooth); } catch(e) {}
try { importPackage(android.bluetooth.le); } catch(e) {}
try { importPackage(java.util); } catch(e) {}

var bleEvent = (function () {
    // ------- 可配置项 -------
    var SERVICE_UUID_STR = "00001234-0000-1000-8000-00805f9b34fb";
    var CHAR_UUID_STR    = "91721b02-11c1-409d-838c-e69c1187492a";
    var CCCD_UUID_STR    = "00002902-0000-1000-8000-00805f9b34fb";
    var TARGET_MTU       = 100;

    // ------- 内部状态 -------
    var _deviceName   = "";
    var _adapter      = null;
    var _gatt         = null;
    var _charac       = null;
    var _ready        = false;
    var _connectCb    = null;
    var _notifyCb     = null;
    var _scanning     = false;
    var _autoList     = [];

    function _log(msg) {
        try { logd("[bleEvent] " + msg); } catch (e) {}
    }

    function _jsStr(s) {
        try {
            if (s == null) return "";
            return s.toString();
        } catch (e) { return ""; }
    }

    function _lower(s) {
        try { return _jsStr(s).toLowerCase(); } catch (e) { return ""; }
    }

    function _writeJSON(json) {
        if (!_ready || !_gatt || !_charac) {
            _log("未就绪，写入失败");
            return false;
        }
        try {
            var text = JSON.stringify(json);
            var bytes = new java.lang.String(text).getBytes("UTF-8");
            _charac.setValue(bytes);
            _charac.setWriteType(1);
            return _gatt.writeCharacteristic(_charac);
        } catch (e) {
            _log("写入异常: " + e.message);
            return false;
        }
    }

    function _syncScreenSize() {
        try {
            var sw = device.getScreenWidth();
            var sh = device.getScreenHeight();
            if (sw > 0 && sh > 0) {
                _log("同步屏幕分辨率: " + sw + "x" + sh);
                _writeJSON({ a: "sr", w: sw, h: sh });
            }
        } catch (e) {
            _log("获取屏幕分辨率失败: " + e.message);
        }
    }

    function _closeOld() {
        if (_gatt) {
            try { _gatt.disconnect(); } catch(e) {}
            try { _gatt.close(); } catch(e) {}
            _gatt = null;
        }
        _ready = false;
        _charac = null;
    }

    function _tryNext() {
        if (_ready) return;
        if (_autoList.length == 0) {
            _log("所有配对设备都试过了，没找到触屏设备");
            if (_connectCb) {
                try { _connectCb(false); } catch(e) {}
                _connectCb = null;
            }
            return;
        }
        var dev = _autoList.shift();
        _log("尝试: " + dev.getName());
        _connectGatt(dev, true);
    }

    function _connectGatt(device, isAuto) {
        _closeOld();

        try {
            var gattCb = new JavaAdapter(
                android.bluetooth.BluetoothGattCallback,
                {
                    onConnectionStateChange: function (gatt, status, newState) {
                        try {
                            if (newState == 2) {
                                _gatt = gatt;
                                _log("GATT已连接, 发现服务...");
                                gatt.discoverServices();
                            } else if (newState == 0) {
                                _ready = false;
                                _charac = null;
                                _log("已断开连接");
                                if (isAuto && _autoList.length > 0) {
                                    // 自动模式下断开了试下一个
                                    try { gatt.close(); } catch(e) {}
                                    _gatt = null;
                                    _tryNext();
                                } else if (_connectCb) {
                                    try { _connectCb(false); } catch (e2) {}
                                    _connectCb = null;
                                }
                            }
                        } catch (e) {
                            _log("onConnectionStateChange err: " + e.message);
                        }
                    },
                    onServicesDiscovered: function (gatt, status) {
                        try {
                            if (status == 0) {
                                var svc = gatt.getService(
                                    UUID.fromString(SERVICE_UUID_STR));
                                if (!svc) {
                                    if (isAuto) {
                                        _log("无控制服务，不是目标，试下一个");
                                        try { gatt.disconnect(); } catch(e) {}
                                        try { gatt.close(); } catch(e) {}
                                        _gatt = null;
                                        _tryNext();
                                    }
                                    return;
                                }
                                _charac = svc.getCharacteristic(
                                    UUID.fromString(CHAR_UUID_STR));
                                if (!_charac) {
                                    if (isAuto) {
                                        _log("无控制特征值，不是目标，试下一个");
                                        try { gatt.disconnect(); } catch(e) {}
                                        try { gatt.close(); } catch(e) {}
                                        _gatt = null;
                                        _tryNext();
                                    }
                                    return;
                                }
                                _log("找到控制特征值, props=" + _charac.getProperties());

                                gatt.setCharacteristicNotification(_charac, true);
                                var desc = _charac.getDescriptor(
                                    UUID.fromString(CCCD_UUID_STR));
                                desc.setValue(
                                    android.bluetooth.BluetoothGattDescriptor
                                        .ENABLE_NOTIFICATION_VALUE);
                                gatt.writeDescriptor(desc);
                            } else {
                                if (isAuto) {
                                    _log("服务发现失败，试下一个");
                                    try { gatt.disconnect(); } catch(e) {}
                                    try { gatt.close(); } catch(e) {}
                                    _gatt = null;
                                    _tryNext();
                                }
                            }
                        } catch (e) {
                            _log("onServicesDiscovered err: " + e.message);
                        }
                    },
                    onDescriptorWrite: function (gatt, descriptor, status) {
                        try {
                            _log("描述符写入完成 status=" + status);
                            if (status == 0) {
                                _log("请求MTU=" + TARGET_MTU + "...");
                                gatt.requestMtu(TARGET_MTU);
                            }
                        } catch (e) {
                            _log("onDescriptorWrite err: " + e.message);
                        }
                    },
                    onMtuChanged: function (gatt, mtu, status) {
                        try {
                            _log("MTU变化: mtu=" + mtu + " status=" + status);
                            _ready = true;
                            _autoList = [];
                            _log("✅ 连接就绪 (MTU=" + mtu + ")");
                            _syncScreenSize();
                            if (_connectCb) {
                                try { _connectCb(true); } catch (e2) {}
                                _connectCb = null;
                            }
                        } catch (e) {
                            _log("onMtuChanged err: " + e.message);
                        }
                    },
                    onCharacteristicChanged: function (gatt, ch) {
                        try {
                            var bytes = ch.getValue();
                            var s = new java.lang.String(bytes, "UTF-8").toString();
                            if (_notifyCb) {
                                try { _notifyCb(s); } catch (e2) {}
                            }
                        } catch (e) {}
                    },
                    onCharacteristicWrite: function (gatt, ch, status) {
                    }
                }
            );

            device.connectGatt(context, false, gattCb);
        } catch (e) {
            _log("连接GATT失败: " + e.message);
            if (isAuto) {
                _tryNext();
            } else if (_connectCb) {
                try { _connectCb(false); } catch (e2) {}
                _connectCb = null;
            }
        }
    }

    return {
        /**
         * 自动连接配对设备（不用输名称）
         */
        autoInit: function (callback) {
            if (_ready || _gatt || _scanning) {
                if (callback && _ready) {
                    try { callback(true); } catch (e) {}
                }
                return;
            }

            _connectCb = callback || null;
            _ready = false;
            _charac = null;
            _autoList = [];

            try {
                var bm = context.getSystemService("bluetooth");
                _adapter = bm.getAdapter();
            } catch (e) {
                _log("获取蓝牙适配器失败: " + e.message);
                if (_connectCb) { try { _connectCb(false); } catch (e2) {} }
                return;
            }
            if (!_adapter) {
                _log("蓝牙不可用");
                if (_connectCb) { try { _connectCb(false); } catch (e2) {} }
                return;
            }
            try {
                if (!_adapter.isEnabled()) {
                    _log("蓝牙未开启");
                    if (_connectCb) { try { _connectCb(false); } catch (e2) {} }
                    return;
                }
            } catch (e) {
                _log("检查蓝牙状态失败: " + e.message);
                if (_connectCb) { try { _connectCb(false); } catch (e2) {} }
                return;
            }

            try {
                var devices = _adapter.getBondedDevices();
                if (devices) {
                    var it = devices.iterator();
                    while (it.hasNext()) {
                        var dev = it.next();
                        _autoList.push(dev);
                    }
                }
            } catch (e) {
                _log("读取配对列表异常: " + e.message);
            }

            if (_autoList.length == 0) {
                _log("没有已配对设备");
                if (_connectCb) { try { _connectCb(false); } catch (e2) {} }
                return;
            }

            _log("已配对设备 " + _autoList.length + " 个，逐个尝试...");
            _tryNext();
        },

        init: function (deviceName, callback) {
            if (_ready || _gatt || _scanning) {
                _log("已连接/连接中，跳过重复init");
                if (callback && _ready) {
                    try { callback(true); } catch (e) {}
                }
                return;
            }

            _deviceName = _lower(deviceName);
            _connectCb = callback || null;
            _ready = false;
            _charac = null;

            try {
                var bm = context.getSystemService("bluetooth");
                _adapter = bm.getAdapter();
            } catch (e) {
                _log("获取蓝牙适配器失败: " + e.message);
                if (_connectCb) { try { _connectCb(false); } catch (e2) {} }
                return;
            }

            if (!_adapter) {
                _log("蓝牙不可用");
                if (_connectCb) { try { _connectCb(false); } catch (e2) {} }
                return;
            }
            try {
                if (!_adapter.isEnabled()) {
                    _log("蓝牙未开启");
                    if (_connectCb) { try { _connectCb(false); } catch (e2) {} }
                    return;
                }
            } catch (e) {
                _log("检查蓝牙状态失败: " + e.message);
                if (_connectCb) { try { _connectCb(false); } catch (e2) {} }
                return;
            }

            try {
                var devices = _adapter.getBondedDevices();
                if (devices) {
                    var it = devices.iterator();
                    while (it.hasNext()) {
                        var dev = it.next();
                        var dname = _lower(dev.getName());
                        if (dname == _deviceName) {
                            _log("找到配对设备: " + dev.getAddress());
                            _connectGatt(dev, false);
                            return;
                        }
                    }
                }
            } catch (e) {
                _log("读取配对列表异常: " + e.message);
            }

            _log("未找到配对设备，开始扫描...");
            try {
                var scanner = _adapter.getBluetoothLeScanner();
                if (!scanner) {
                    _log("扫描器不可用");
                    if (_connectCb) { try { _connectCb(false); } catch (e2) {} }
                    return;
                }

                var scanCb = new JavaAdapter(
                    android.bluetooth.le.ScanCallback,
                    {
                        onScanResult: function (cbType, result) {
                            try {
                                var d = result.getDevice();
                                if (_lower(d.getName()) == _deviceName) {
                                    _log("找到设备: " + d.getName());
                                    try { scanner.stopScan(this); } catch (e) {}
                                    _scanning = false;
                                    _connectGatt(d, false);
                                }
                            } catch (e) {}
                        },
                        onScanFailed: function (code) {
                            try {
                                _log("扫描失败: " + code);
                                _scanning = false;
                                if (_connectCb) {
                                    try { _connectCb(false); } catch (e2) {}
                                    _connectCb = null;
                                }
                            } catch (e) {}
                        }
                    }
                );

                _scanning = true;
                scanner.startScan(scanCb);
            } catch (e) {
                _log("启动扫描失败: " + e.message);
                _scanning = false;
                if (_connectCb) { try { _connectCb(false); } catch (e2) {} }
            }
        },

        clickPoint: function (x, y) {
            try {
                return _writeJSON({ a: "db", x: Math.round(x), y: Math.round(y) });
            } catch (e) { _log("clickPoint err: " + e.message); return false; }
        },

        swipe: function (startX, startY, endX, endY, durationMs) {
            try {
                return _writeJSON({
                    a: "sw",
                    fx: Math.round(startX),
                    fy: Math.round(startY),
                    tx: Math.round(endX),
                    ty: Math.round(endY),
                    d: durationMs || 300
                });
            } catch (e) { _log("swipe err: " + e.message); return false; }
        },

        touchDown: function (x, y) {
            try {
                return _writeJSON({ a: "td", x: Math.round(x), y: Math.round(y) });
            } catch (e) { _log("touchDown err: " + e.message); return false; }
        },

        touchMove: function (x, y) {
            try {
                return _writeJSON({ a: "tm", x: Math.round(x), y: Math.round(y) });
            } catch (e) { _log("touchMove err: " + e.message); return false; }
        },

        touchUp: function () {
            try { return _writeJSON({ a: "tu" }); }
            catch (e) { _log("touchUp err: " + e.message); return false; }
        },

        home: function () {
            try { return _writeJSON({ a: "hbe" }); }
            catch (e) { _log("home err: " + e.message); return false; }
        },

        systemKey: function (key) {
            try {
                if (key === "home") return this.home();
                if (key === "back") return _writeJSON({ a: "sbe" });
                if (key === "recents") return this.home();
                return false;
            } catch (e) { _log("systemKey err: " + e.message); return false; }
        },

        isConnected: function () {
            return _ready;
        },

        disconnect: function () {
            try {
                if (_scanning && _adapter) {
                    try {
                        var s = _adapter.getBluetoothLeScanner();
                        if (s) s.stopScan();
                    } catch (e) {}
                    _scanning = false;
                }
                _closeOld();
            } catch (e) { _log("disconnect err: " + e.message); }
            _autoList = [];
        },

        setNotifyCallback: function (cb) {
            _notifyCb = cb;
        }
    };
})();

try { logd("[bleEvent] 自研BLE触屏驱动已加载 v1.3 (autoInit版)"); } catch(e) {}
