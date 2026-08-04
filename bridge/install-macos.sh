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

# Scaffold the remote's shortcut allowlist on first install. open_url (e.g. YouTube)
# works without it; open_app / shortcut actions need matching entries here.
CONFIG_DIR="$HOME/.config/ai-usage-bridge"
mkdir -p "$CONFIG_DIR"
if [ ! -f "$CONFIG_DIR/actions.json" ] && [ -f "$DIR/actions.example.json" ]; then
  cp "$DIR/actions.example.json" "$CONFIG_DIR/actions.json"
  echo "✓ Created $CONFIG_DIR/actions.json (edit it to add your own apps/shortcuts)."
fi

# Dashboard only (no command endpoint)?  run:  REMOTE=0 ./install-macos.sh
REMOTE_ENV=""
if [ -n "${REMOTE:-}" ]; then
  REMOTE_ENV="<key>REMOTE</key><string>${REMOTE}</string>"
fi

LOG="$HOME/Library/Logs/ai-usage-bridge.log"
mkdir -p "$HOME/Library/LaunchAgents" "$HOME/Library/Logs"

# A hand-started copy (nohup) still holding the port makes the agent die with
# EADDRINUSE on every relaunch, and KeepAlive turns that into a silent respawn
# loop — the agent looks like it "hangs and never binds". Clear the port first.
if command -v lsof >/dev/null && lsof -nP -iTCP:"$PORT" -sTCP:LISTEN >/dev/null 2>&1; then
  echo "! Port $PORT is already in use — stopping the old bridge first:"
  lsof -nP -iTCP:"$PORT" -sTCP:LISTEN | sed 's/^/    /'
  for pid in $(lsof -nP -tiTCP:"$PORT" -sTCP:LISTEN); do kill "$pid" 2>/dev/null || true; done
  sleep 2
fi

cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>            <string>$LABEL</string>
  <key>ProgramArguments</key> <array><string>$NODE</string><string>$SCRIPT</string></array>
  <key>EnvironmentVariables</key><dict><key>PORT</key><string>$PORT</string>$REMOTE_ENV</dict>
  <key>RunAtLoad</key>        <true/>
  <key>KeepAlive</key>        <true/>
  <key>StandardOutPath</key>  <string>$LOG</string>
  <key>StandardErrorPath</key><string>$LOG</string>
</dict>
</plist>
EOF

# bootout/bootstrap is the supported path; load/unload is legacy and can fail
# quietly on current macOS. Fall back so older systems still work.
UID_NUM="$(id -u)"
if launchctl print "gui/$UID_NUM/$LABEL" >/dev/null 2>&1; then
  launchctl bootout "gui/$UID_NUM/$LABEL" 2>/dev/null || true
fi
if ! launchctl bootstrap "gui/$UID_NUM" "$PLIST" 2>/dev/null; then
  launchctl unload "$PLIST" 2>/dev/null || true
  launchctl load "$PLIST"
fi
launchctl kickstart -k "gui/$UID_NUM/$LABEL" >/dev/null 2>&1 || true

# Prove it actually bound instead of reporting success blindly.
BOUND=""
for _ in 1 2 3 4 5 6 7 8 9 10; do
  if lsof -nP -iTCP:"$PORT" -sTCP:LISTEN >/dev/null 2>&1; then BOUND=1; break; fi
  sleep 1
done
if [ -z "$BOUND" ]; then
  echo "✗ Agent loaded but nothing is listening on :$PORT. Last log lines:" >&2
  tail -20 "$LOG" >&2 2>/dev/null || true
  exit 1
fi

IP="$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || echo '<mac-ip>')"

echo "✓ Bridge installed and listening on :$PORT."
echo "  Endpoint:  http://$IP:$PORT/usage"
echo "  Logs:      $LOG"
if [ "${REMOTE:-}" = "0" ]; then
  echo "  Remote:    disabled (dashboard only)"
else
  # The bridge writes its pairing token on first run; read it back to show the user.
  TOKEN=""
  for _ in 1 2 3 4 5 6; do
    if [ -f "$CONFIG_DIR/pairing.json" ]; then
      TOKEN="$("$NODE" -e 'process.stdout.write(JSON.parse(require("fs").readFileSync(process.argv[1],"utf8")).token||"")' "$CONFIG_DIR/pairing.json" 2>/dev/null || true)"
    fi
    [ -n "$TOKEN" ] && break
    sleep 0.5
  done
  if [ -n "$TOKEN" ]; then
    echo "  Remote:    ENABLED — pairing token:  $TOKEN"
    echo "             Enter it on the device's Wi-Fi setup screen. Shortcuts: $CONFIG_DIR/actions.json"
  else
    echo "  Remote:    ENABLED — token will appear in $CONFIG_DIR/pairing.json (see the log)"
  fi
  echo "             Dashboard only? re-run:  REMOTE=0 ./install-macos.sh $PORT"
fi
echo "  Uninstall: launchctl bootout \"gui/$UID_NUM/$LABEL\" && rm \"$PLIST\""
