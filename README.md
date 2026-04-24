# Server Phone Module

Zygisk 模块，将 Android 手机改装为可长期挂机的服务器。基于小米 11 (M2011K2C) + Android 14 + APatch + Zygisk-Next 环境开发。

## 功能

### 1. 后台进程保护

- 禁用 phantom process killer
- 禁用 Doze（深度 + 轻度）
- 禁用 App Standby、Adaptive Battery
- 最大化 cached process 上限
- 禁用 MIUI/HyperOS 特有的后台限制

### 2. 屏幕常亮

- 屏幕超时设为最大值（~24.8 天）
- 充电时保持唤醒
- 禁用自动亮度
- 禁用锁屏

### 3. 电源键亮度控制（power_daemon）

一个 native daemon 通过 `EVIOCGRAB` 独占电源键输入设备，拦截并重新定义按键行为：

| 操作 | 效果 |
|------|------|
| 双击电源键 | 切换屏幕亮度（0 ↔ 正常）+ 切换触控启用/禁用 |
| 长按电源键 (≥400ms) | 弹出关机菜单 |
| 单击电源键 | 无操作（被消耗，不会锁屏） |
| 连按5次音量上键 (3秒内) | 紧急恢复：最高亮度 + 恢复触控 |

亮度为 0 时屏幕完全黑屏但系统保持 Awake 状态，不会触发休眠或省电策略。

## 项目结构

```
├── native/
│   └── power_daemon.c      # 电源键拦截 daemon 源码
├── module/
│   ├── module.prop          # 模块元信息
│   ├── customize.sh         # 安装脚本
│   ├── service.sh           # 开机启动（系统设置 + 启动 daemon）
│   ├── uninstall.sh         # 卸载清理
│   └── bin/power_daemon     # 编译产物（gitignore）
├── build.sh                 # 编译 + 打包
└── server_phone_module.zip  # 可安装模块包（gitignore）
```

## 构建

需要 Android NDK（已测试 r27）。

```bash
./build.sh
```

产出 `server_phone_module.zip`，通过 APatch 应用安装后重启生效。

## 设备适配

默认硬编码了以下设备路径（针对小米 11）：

| 用途 | 默认路径 | 说明 |
|------|---------|------|
| 电源键 | `/dev/input/event0` | qpnp_pon (KEY_POWER + KEY_VOLUMEDOWN) |
| 触摸屏 | `/dev/input/event4` | fts |
| 音量上键 | `/dev/input/event2` | gpio-keys (KEY_VOLUMEUP) |
| 背光 | `/sys/class/backlight/panel0-backlight/brightness` | 范围 0-2047 |

其他设备需要通过 `getevent -pl` 确认路径，并修改源码中的 `DEFAULT_*_DEV` 和 `BRIGHTNESS_SYSFS` 宏，或通过命令行参数覆盖：

```bash
power_daemon [电源键设备] [触摸屏设备] [音量上键设备]
```

## 日志

```bash
# daemon 日志
cat /data/local/tmp/power_daemon.log

# 模块启动日志
cat /data/local/tmp/server_phone.log
```
