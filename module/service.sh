#!/system/bin/sh
MODDIR=${0%/*}

while [ "$(getprop sys.boot_completed)" != "1" ]; do
    sleep 1
done
sleep 8

LOG="/data/local/tmp/server_phone.log"
echo "$(date): service.sh starting" >> "$LOG"

# =====================================================
# 1. Background process protection
# =====================================================

# Disable phantom process killer (Android 12+)
settings put global settings_enable_monitor_phantom_procs false
device_config put activity_manager max_phantom_processes 2147483647 2>/dev/null

# Maximize cached / background process slots
settings put global activity_manager_constants max_cached_processes=2147483647,background_settle_time=0

# Disable Doze
dumpsys deviceidle disable
settings put global device_idle_constants inactive_to=2147483647,sensing_to=2147483647,locating_to=2147483647,motion_inactive_to=2147483647,idle_after_inactive_to=2147483647,idle_pending_to=2147483647,max_idle_pending_to=2147483647,idle_to=2147483647,max_idle_to=2147483647

# Disable App Standby
settings put global app_standby_enabled 0

# Disable battery saver
settings put global low_power 0
settings put global low_power_sticky 0

# Disable adaptive battery
settings put secure adaptive_battery_management_enabled 0

# Disable background FGS restrictions (Android 14)
device_config put activity_manager default_fgs_starts_restriction_enabled false 2>/dev/null

# --- MIUI / HyperOS specific ---
setprop persist.miui.extm.enable 0
setprop persist.sys.millet.cgroup1 ""
settings put secure background_limit_enabled 0 2>/dev/null

echo "$(date): background protection applied" >> "$LOG"

# =====================================================
# 2. Screen always on
# =====================================================

# Manual brightness mode
settings put system screen_brightness_mode 0

# Never auto-off (max timeout ~24.8 days)
settings put system screen_off_timeout 2147483647

# Keep screen on while plugged in (USB + AC + wireless)
svc power stayon true

# Disable lock screen
settings put secure lockscreen.disabled 1

echo "$(date): screen settings applied" >> "$LOG"

# =====================================================
# 3. Power button config
# =====================================================

# Long press = global actions (power menu)
settings put global power_button_long_press 1

# =====================================================
# 4. Start power button daemon
# =====================================================

killall power_daemon 2>/dev/null
sleep 1

$MODDIR/bin/power_daemon >> /data/local/tmp/power_daemon.log 2>&1 &
DAEMON_PID=$!
echo "$(date): power_daemon started (PID=$DAEMON_PID)" >> "$LOG"
