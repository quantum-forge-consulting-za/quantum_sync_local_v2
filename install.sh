#!/bin/bash
# QuantumSync Local V2 — Installation Script
# Installs standalone music player on Raspberry Pi
# V2 adds: Samba network share for the music folder + folder selection GUI
# Also cleans up old QuantumSync client/server files

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

echo -e "${GREEN}╔══════════════════════════════════════╗${NC}"
echo -e "${GREEN}║   QuantumSync Local V2 — Installer   ║${NC}"
echo -e "${GREEN}╚══════════════════════════════════════╝${NC}"
echo ""

# Must run as root
if [ "$EUID" -ne 0 ]; then
    echo -e "${RED}Please run as root: sudo ./install.sh${NC}"
    exit 1
fi

# ──────────────────────────────────────────────
# Phase 1: Clean up old QuantumSync
# ──────────────────────────────────────────────
echo -e "${YELLOW}[Phase 1] Cleaning up old QuantumSync...${NC}"

# Stop and disable old services
for svc in quantumsync-client quantumsync-server quantumsync-mpd-watchdog; do
    if systemctl is-active --quiet "$svc" 2>/dev/null; then
        echo "  Stopping $svc..."
        systemctl stop "$svc" 2>/dev/null || true
    fi
    if systemctl is-enabled --quiet "$svc" 2>/dev/null; then
        echo "  Disabling $svc..."
        systemctl disable "$svc" 2>/dev/null || true
    fi
done

# Remove old systemd service files
for f in /etc/systemd/system/quantumsync-client.service \
         /etc/systemd/system/quantumsync-server.service \
         /etc/systemd/system/quantumsync-mpd-watchdog.service; do
    if [ -f "$f" ]; then
        echo "  Removing $f"
        rm -f "$f"
    fi
done

# Remove old binaries
for f in /usr/local/bin/quantumsync-client \
         /usr/local/bin/quantumsync-server \
         /usr/local/bin/quantumsync-music-player \
         /usr/local/bin/quantumsync-mpd-watchdog; do
    if [ -f "$f" ]; then
        echo "  Removing $f"
        rm -f "$f"
    fi
done

# Remove old config (but keep music if present)
if [ -d "/etc/quantumsync" ]; then
    echo "  Removing /etc/quantumsync/"
    rm -rf /etc/quantumsync
fi

# Remove old web GUI files
if [ -d "/usr/share/quantumsync" ]; then
    echo "  Removing /usr/share/quantumsync/"
    rm -rf /usr/share/quantumsync
fi

# Remove old build artifacts if repo exists
if [ -d "/home/pi/quantum_sync_V3" ]; then
    echo "  Removing /home/pi/quantum_sync_V3/"
    rm -rf /home/pi/quantum_sync_V3
fi
if [ -d "/home/pi/quantum_sync_V2" ]; then
    echo "  Removing /home/pi/quantum_sync_V2/"
    rm -rf /home/pi/quantum_sync_V2
fi

# Reload systemd after removing service files
systemctl daemon-reload

echo -e "${GREEN}  Old QuantumSync cleaned up.${NC}"
echo ""

# ──────────────────────────────────────────────
# Phase 2: Install dependencies
# ──────────────────────────────────────────────
echo -e "${YELLOW}[Phase 2] Installing dependencies...${NC}"

apt-get update -qq
apt-get install -y -qq build-essential cmake libboost-all-dev mpd mpc samba

# wsdd2 makes the Pi show up under "Network" in Windows Explorer (optional)
apt-get install -y -qq wsdd2 2>/dev/null || true

echo -e "${GREEN}  Dependencies installed.${NC}"
echo ""

# ──────────────────────────────────────────────
# Phase 3: Create user and directories
# ──────────────────────────────────────────────
echo -e "${YELLOW}[Phase 3] Setting up user and directories...${NC}"

# Create quantumsync user if it doesn't exist
if ! id -u quantumsync &>/dev/null; then
    useradd -r -s /sbin/nologin -d /var/lib/quantumsync -m quantumsync
    echo "  Created user: quantumsync"
fi

# Add to audio and systemd-journal groups
usermod -a -G audio quantumsync 2>/dev/null || true
usermod -a -G systemd-journal quantumsync 2>/dev/null || true

# Create directories
mkdir -p /opt/quantumsync-local/music
mkdir -p /var/lib/quantumsync-local/mpd/playlists
mkdir -p /etc/quantumsync-local
mkdir -p /run/quantumsync-local

# Set ownership
chown -R quantumsync:quantumsync /opt/quantumsync-local
chown -R quantumsync:quantumsync /var/lib/quantumsync-local
chown -R quantumsync:quantumsync /etc/quantumsync-local
chown -R quantumsync:quantumsync /run/quantumsync-local

echo -e "${GREEN}  User and directories ready.${NC}"
echo ""

# ──────────────────────────────────────────────
# Phase 4: Build C++ program
# ──────────────────────────────────────────────
echo -e "${YELLOW}[Phase 4] Building quantumsync-local...${NC}"

BUILD_DIR="$SCRIPT_DIR/build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake "$SCRIPT_DIR" -DCMAKE_BUILD_TYPE=Release
make -j"$(nproc)"

# Install binary
cp quantumsync-local /usr/local/bin/quantumsync-local
chmod 755 /usr/local/bin/quantumsync-local

# Install boot-time queue restore script (remembers selected folder)
cp "$SCRIPT_DIR/scripts/quantumsync-restore-queue" /usr/local/bin/quantumsync-restore-queue
chmod 755 /usr/local/bin/quantumsync-restore-queue

cd "$SCRIPT_DIR"
echo -e "${GREEN}  Build complete. Binary: /usr/local/bin/quantumsync-local${NC}"
echo ""

# ──────────────────────────────────────────────
# Phase 5: Configure MPD
# ──────────────────────────────────────────────
echo -e "${YELLOW}[Phase 5] Configuring MPD...${NC}"

# Stop system MPD if running (we use our own instance)
systemctl stop mpd 2>/dev/null || true
systemctl disable mpd 2>/dev/null || true

# Install our MPD config
cp "$SCRIPT_DIR/config/mpd.conf" /etc/quantumsync-local/mpd.conf
chown quantumsync:quantumsync /etc/quantumsync-local/mpd.conf

echo -e "${GREEN}  MPD configured.${NC}"
echo ""

# ──────────────────────────────────────────────
# Phase 6: Device name configuration
# ──────────────────────────────────────────────
echo -e "${YELLOW}[Phase 6] Device configuration...${NC}"

# Check if config already exists (re-install)
if [ -f /etc/quantumsync-local/config.conf ]; then
    EXISTING_NAME=$(grep "^DEVICE_NAME=" /etc/quantumsync-local/config.conf | cut -d'=' -f2)
    echo "  Existing config found: DEVICE_NAME=$EXISTING_NAME"
    read -p "  Keep existing name? [Y/n]: " KEEP_NAME
    if [ "$KEEP_NAME" = "n" ] || [ "$KEEP_NAME" = "N" ]; then
        EXISTING_NAME=""
    fi
fi

if [ -z "$EXISTING_NAME" ]; then
    read -p "  Enter device/room name (e.g. Kitchen, Lounge): " DEVICE_NAME
    if [ -z "$DEVICE_NAME" ]; then
        DEVICE_NAME="Local Player"
    fi
else
    DEVICE_NAME="$EXISTING_NAME"
fi

cat > /etc/quantumsync-local/config.conf << EOF
# QuantumSync Local Configuration
DEVICE_NAME=$DEVICE_NAME
HTTP_PORT=1706
EOF

chown quantumsync:quantumsync /etc/quantumsync-local/config.conf

echo -e "${GREEN}  Device name: $DEVICE_NAME${NC}"
echo ""

# ──────────────────────────────────────────────
# Phase 7: Samba network share for the music folder
# ──────────────────────────────────────────────
echo -e "${YELLOW}[Phase 7] Setting up network share (\\\\pi\\music)...${NC}"

SMB_CONF="/etc/samba/smb.conf"

# Pick the login user for the share (must be an existing system user).
# Files are force-owned by quantumsync regardless, so any user works.
DEFAULT_SHARE_USER="quantumsync"
if id -u pi &>/dev/null; then
    DEFAULT_SHARE_USER="pi"
fi
while true; do
    read -p "  Username for connecting to the share [$DEFAULT_SHARE_USER]: " SHARE_USER
    SHARE_USER="${SHARE_USER:-$DEFAULT_SHARE_USER}"
    if id -u "$SHARE_USER" &>/dev/null; then
        break
    fi
    echo -e "${RED}  User '$SHARE_USER' does not exist on this system.${NC}"
done

# (Re)write the share definition between markers so re-installs (or a
# changed username) replace the old block instead of duplicating it.
touch "$SMB_CONF"
sed -i '/^# >>> quantumsync-local music share >>>$/,/^# <<< quantumsync-local music share <<<$/d' "$SMB_CONF"
# Also remove the marker-less block from older V2 installs
sed -i '/^# ── QuantumSync Local music share ──$/,/^   directory mask = 0775$/d' "$SMB_CONF"
cat >> "$SMB_CONF" << EOF
# >>> quantumsync-local music share >>>
[music]
   comment = QuantumSync Music
   path = /opt/quantumsync-local/music
   browseable = yes
   read only = no
   valid users = $SHARE_USER
   force user = quantumsync
   force group = quantumsync
   create mask = 0664
   directory mask = 0775
# <<< quantumsync-local music share <<<
EOF
echo "  Share [music] configured for user '$SHARE_USER'"

# Set (or update) the share password
SET_PW="y"
if pdbedit -L 2>/dev/null | grep -q "^$SHARE_USER:"; then
    read -p "  Samba user '$SHARE_USER' already exists. Reset share password? [y/N]: " SET_PW
fi
if [ "$SET_PW" = "y" ] || [ "$SET_PW" = "Y" ]; then
    while true; do
        read -s -p "  Enter share password (for connecting from Windows): " SHARE_PW
        echo ""
        read -s -p "  Confirm password: " SHARE_PW2
        echo ""
        if [ -z "$SHARE_PW" ]; then
            echo -e "${RED}  Password cannot be empty.${NC}"
        elif [ "$SHARE_PW" != "$SHARE_PW2" ]; then
            echo -e "${RED}  Passwords do not match, try again.${NC}"
        else
            break
        fi
    done
    printf '%s\n%s\n' "$SHARE_PW" "$SHARE_PW" | smbpasswd -s -a "$SHARE_USER"
    smbpasswd -e "$SHARE_USER" >/dev/null 2>&1 || true
    unset SHARE_PW SHARE_PW2
    echo "  Share password set for user '$SHARE_USER'"
fi

# Validate config and (re)start services
testparm -s >/dev/null 2>&1 || echo -e "${YELLOW}  Warning: testparm reported issues with smb.conf${NC}"
systemctl enable smbd >/dev/null 2>&1 || true
systemctl restart smbd
systemctl enable nmbd >/dev/null 2>&1 || true
systemctl restart nmbd 2>/dev/null || true
systemctl enable wsdd2 >/dev/null 2>&1 || true
systemctl restart wsdd2 2>/dev/null || true

echo -e "${GREEN}  Network share ready.${NC}"
echo ""

# ──────────────────────────────────────────────
# Phase 8: Install and enable systemd services
# ──────────────────────────────────────────────
echo -e "${YELLOW}[Phase 8] Installing systemd services...${NC}"

# Install service files
cp "$SCRIPT_DIR/systemd/quantumsync-local-mpd.service" /etc/systemd/system/
cp "$SCRIPT_DIR/systemd/quantumsync-local.service" /etc/systemd/system/

# Create tmpfiles.d entry for /run directory (survives reboots)
cat > /etc/tmpfiles.d/quantumsync-local.conf << EOF
d /run/quantumsync-local 0755 quantumsync quantumsync -
EOF

systemctl daemon-reload

# Enable and start services
systemctl enable quantumsync-local-mpd.service
systemctl enable quantumsync-local.service

systemctl start quantumsync-local-mpd.service
sleep 3
systemctl start quantumsync-local.service

echo -e "${GREEN}  Services installed and started.${NC}"
echo ""

# ──────────────────────────────────────────────
# Phase 9: Initialize MPD database
# ──────────────────────────────────────────────
echo -e "${YELLOW}[Phase 9] Initializing MPD...${NC}"

# Wait for MPD to be ready
sleep 2
mpc update --wait 2>/dev/null || true

# Count tracks
TRACK_COUNT=$(mpc listall 2>/dev/null | wc -l)
echo "  Found $TRACK_COUNT tracks in music directory"

if [ "$TRACK_COUNT" -gt 0 ]; then
    mpc repeat on 2>/dev/null || true
    mpc random on 2>/dev/null || true
    /usr/local/bin/quantumsync-restore-queue || true
    echo -e "${GREEN}  Music playing!${NC}"
else
    echo -e "${YELLOW}  No music files found. Copy music to the network share \\\\<pi-ip>\\music${NC}"
    echo "  (or to /opt/quantumsync-local/music/), then click Rescan in the web GUI."
fi

echo ""

# ──────────────────────────────────────────────
# Done!
# ──────────────────────────────────────────────
IP_ADDR=$(hostname -I | awk '{print $1}')

echo -e "${GREEN}╔══════════════════════════════════════╗${NC}"
echo -e "${GREEN}║   Installation Complete!             ║${NC}"
echo -e "${GREEN}╚══════════════════════════════════════╝${NC}"
echo ""
echo "  Device:      $DEVICE_NAME"
echo "  Web GUI:     http://$IP_ADDR:1706/"
echo "  Music share: \\\\$IP_ADDR\\music   (user: $SHARE_USER + your share password)"
echo "  Music dir:   /opt/quantumsync-local/music/"
echo ""
echo "  To add music from a Windows PC:"
echo "    Open \\\\$IP_ADDR\\music in File Explorer and copy files in."
echo "    Make subfolders (e.g. 'Day to Day', 'Christmas') to group music —"
echo "    they show up as selectable folders in the web GUI."
echo "    New files are picked up automatically (or click Rescan in the GUI)."
echo ""
echo "  Service commands:"
echo "    systemctl status quantumsync-local"
echo "    systemctl status quantumsync-local-mpd"
echo "    journalctl -u quantumsync-local -f"
echo ""
