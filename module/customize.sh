#!/system/bin/sh

SKIPUNZIP=0

set_perm $MODPATH/bin/power_daemon 0 0 0755
set_perm $MODPATH/service.sh 0 0 0755
set_perm $MODPATH/uninstall.sh 0 0 0755

ui_print "- Server Phone module installed"
ui_print "- Reboot to activate"
