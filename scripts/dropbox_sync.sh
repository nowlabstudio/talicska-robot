#!/bin/bash

#Install: sudo -v ; curl https://rclone.org/install.sh | sudo bash

# Változók beállítása (így hordozhatóbb és biztosabb)
REMOTE="synapse:"
MOUNT_POINT="/home/eduard/Dropbox"
FILTER_FILE="/home/eduard/dropbox_sync.conf"

# 1. Takarítás
echo "Előző csatolások felszabadítása..."
killall rclone 2>/dev/null
# Leválasztjuk a mappát akkor is, ha be van ragadva
fusermount -u $MOUNT_POINT 2>/dev/null

# Mappa biztosítása
mkdir -p $MOUNT_POINT

# 2. Indítás
echo "Dropbox felcsatolása rclone-nal..."

# A 'nohup' és a végi '&' biztosítja, hogy a terminál bezárása után is fusson
nohup rclone mount $REMOTE/ $MOUNT_POINT \
  --filter-from $FILTER_FILE \
  --vfs-cache-mode full \
  --vfs-cache-max-size 5G \
  --dir-cache-time 10s \
  --attr-timeout 10s \
  --allow-other \
  --async-read=false \
  --buffer-size 16M > /home/eduard/rclone_debug.log 2>&1 &

# Várunk egy kicsit, hogy az rclone inicializáljon
sleep 2

if mountpoint -q "$MOUNT_POINT"; then
    echo "A Dropbox elérhető: $MOUNT_POINT"
    echo "Aktuális tartalom (szűrő alapján):"
    ls $MOUNT_POINT
else
    echo "HIBA: A csatolás nem sikerült. Ellenőrizd a szűrőfájlt vagy a hálózatot!"
fi
