#!/bin/bash
# Orbit Vermittler - Installation auf einem Linux-Server (Ubuntu/Debian, auch Oracle Cloud Free).
# Als Cloud-Init-Skript einfuegen ODER auf dem Server ausfuehren:  sudo bash install.sh
set -e
mkdir -p /opt/orbit-relay
cat > /opt/orbit-relay/orbit_relay.py <<'ORBIT_RELAY_PY'
@@RELAY@@
ORBIT_RELAY_PY
id orbit >/dev/null 2>&1 || useradd --system --home /opt/orbit-relay --shell /usr/sbin/nologin orbit
chown -R orbit /opt/orbit-relay
command -v python3 >/dev/null || (apt-get update && apt-get install -y python3)
cat > /etc/systemd/system/orbit-relay.service <<'ORBIT_UNIT'
[Unit]
Description=Orbit PS3 Vermittler
After=network-online.target
Wants=network-online.target

[Service]
User=orbit
WorkingDirectory=/opt/orbit-relay
ExecStart=/usr/bin/python3 /opt/orbit-relay/orbit_relay.py --port 31400 --state /opt/orbit-relay/owners.json
Restart=always
RestartSec=3
NoNewPrivileges=true

[Install]
WantedBy=multi-user.target
ORBIT_UNIT
# Server-Firewall oeffnen (Oracle-Ubuntu-Images blockieren sonst alles ausser SSH)
if command -v iptables >/dev/null; then
    iptables -C INPUT -p tcp --dport 31400 -j ACCEPT 2>/dev/null || iptables -I INPUT 1 -p tcp --dport 31400 -j ACCEPT
    if command -v netfilter-persistent >/dev/null; then netfilter-persistent save || true; fi
fi
if command -v ufw >/dev/null && ufw status | grep -q active; then ufw allow 31400/tcp; fi
systemctl daemon-reload
systemctl enable --now orbit-relay
sleep 2
systemctl is-active orbit-relay && echo "Orbit Vermittler laeuft auf Port 31400."
