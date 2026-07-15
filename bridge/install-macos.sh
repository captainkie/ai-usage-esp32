#!/usr/bin/env bash
# Install the AI Usage Bridge as a macOS LaunchAgent so it starts at login and
# stays running. Re-run to update. Usage: ./install-macos.sh [PORT]
set -euo pipefail

PORT="${1:-8787}"
LABEL="com.aiusage.bridge"
DIR="$(cd "$(dirname "$0")" && pwd)"
SCRIPT="$DIR/ai-usage-bridge.mjs"
NODE="$(command -v node || true)"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"

if [ -z "$NODE" ]; then
  echo "✗ node not found in PATH. Install Node.js (you already have it if Claude Code runs)." >&2
  exit 1
fi

mkdir -p "$HOME/Library/LaunchAgents"
cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>            <string>$LABEL</string>
  <key>ProgramArguments</key> <array><string>$NODE</string><string>$SCRIPT</string></array>
  <key>EnvironmentVariables</key><dict><key>PORT</key><string>$PORT</string></dict>
  <key>RunAtLoad</key>        <true/>
  <key>KeepAlive</key>        <true/>
  <key>StandardOutPath</key>  <string>/tmp/ai-usage-bridge.log</string>
  <key>StandardErrorPath</key><string>/tmp/ai-usage-bridge.err</string>
</dict>
</plist>
EOF

launchctl unload "$PLIST" 2>/dev/null || true
launchctl load "$PLIST"

IP="$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || echo '<mac-ip>')"
echo "✓ Bridge installed and running."
echo "  Endpoint:  http://$IP:$PORT/usage"
echo "  Logs:      /tmp/ai-usage-bridge.log"
echo "  Uninstall: launchctl unload \"$PLIST\" && rm \"$PLIST\""
