#!/usr/bin/env bash
#
# smoke.sh —— 无硬件冒烟测试
#
# 在开发机/CI 上验证：程序能启动、HTTP 服务可用、状态 JSON 正常、
# 快照列表与监控页可访问。无摄像头/传感器时各驱动会优雅降级，
# 不影响本测试。
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${1:-$ROOT/build-smoke}"
PORT="${SMOKE_PORT:-18080}"

echo "[smoke] configuring in $BUILD_DIR"
cmake -S "$ROOT" -B "$BUILD_DIR" >/dev/null
cmake --build "$BUILD_DIR" -j >/dev/null

BIN="$BUILD_DIR/smart_granary_detect_system"
echo "[smoke] starting $BIN (port=$PORT)"
# 无实体按键时自动进入巡检；快照写到临时目录
GRANARY_AUTO_START=1 \
GRANARY_HTTP_PORT="$PORT" \
GRANARY_SNAPSHOT_DIR="$(mktemp -d)" \
"$BIN" &
PID=$!
trap 'kill $PID 2>/dev/null || true; wait $PID 2>/dev/null || true' EXIT

# 等待 HTTP 服务就绪（最多 10s）
for _ in $(seq 1 50); do
    if curl -fsS "http://127.0.0.1:$PORT/status.json" >/dev/null 2>&1; then
        break
    fi
    sleep 0.2
done

echo "[smoke] GET /status.json"
STATUS="$(curl -fsS "http://127.0.0.1:$PORT/status.json")"
echo "$STATUS" | grep -q '"running"' || { echo "FAIL: status.json missing running"; exit 1; }
echo "$STATUS" | grep -q '"E01"' || { echo "FAIL: status.json missing E01"; exit 1; }
echo "$STATUS" | grep -q '"history"' || { echo "FAIL: status.json missing history"; exit 1; }

echo "[smoke] GET / (monitor page)"
curl -fsS "http://127.0.0.1:$PORT/" | grep -q "Smart Granary" || {
    echo "FAIL: index page"; exit 1;
}

echo "[smoke] GET /snapshots.json"
curl -fsS "http://127.0.0.1:$PORT/snapshots.json" | grep -q '"files"' || {
    echo "FAIL: snapshots.json"; exit 1;
}

echo "[smoke] GET /control?action=toggle"
curl -fsS "http://127.0.0.1:$PORT/control?action=toggle" | grep -q '"ok":true' || {
    echo "FAIL: control"; exit 1;
}

echo "[smoke] PASS"
