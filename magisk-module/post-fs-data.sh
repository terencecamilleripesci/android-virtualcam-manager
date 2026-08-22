#!/system/bin/sh
MODDIR=${0%/*}

mkdir -p /data/adb/virtualcam
chmod 755 /data/adb/virtualcam

if [ ! -f /data/adb/virtualcam/enabled ]; then
  echo -n 0 > /data/adb/virtualcam/enabled
fi
chmod 644 /data/adb/virtualcam/enabled 2>/dev/null

mkdir -p /data/local/tmp/virtualcam
chmod 755 /data/local/tmp/virtualcam

mkdir -p /storage/emulated/0/DCIM/Camera1 2>/dev/null
chmod 775 /storage/emulated/0/DCIM/Camera1 2>/dev/null
