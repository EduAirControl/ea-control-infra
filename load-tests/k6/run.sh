#!/usr/bin/env bash
# run.sh — orquesta todos los escenarios de carga y genera reportes
# Uso: GATEWAY_URL=http://localhost:8080 SUPERADMIN_PASSWORD=xxx bash run.sh
set -euo pipefail

GATEWAY="${GATEWAY_URL:-http://localhost:8080}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RESULTS_DIR="$SCRIPT_DIR/../results"
mkdir -p "$RESULTS_DIR"

K6="${K6_BIN:-k6}"
if ! command -v "$K6" >/dev/null 2>&1; then
    K6="docker run --rm -i --network host -v $SCRIPT_DIR:/scripts grafana/k6 run"
fi

run_scenario() {
    local name="$1"
    local file="$2"
    echo "=== Running scenario: $name ==="
    $K6 run --out json="$RESULTS_DIR/${name}.json" "$SCRIPT_DIR/scenarios/$file" \
        -e GATEWAY_URL="$GATEWAY" \
        -e SUPERADMIN_EMAIL="${SUPERADMIN_EMAIL:-admin@eduaircontrol.com}" \
        -e SUPERADMIN_PASSWORD="${SUPERADMIN_PASSWORD:?Set SUPERADMIN_PASSWORD}" \
        2>&1 | tee "$RESULTS_DIR/${name}.log"
    echo "=== $name done ==="
    echo
}

run_scenario "smoke" "smoke.js"
run_scenario "load" "load.js"
run_scenario "spike" "spike.js"
run_scenario "stress" "stress.js"

echo "=== All scenarios completed ==="
echo "Results in: $RESULTS_DIR"
ls -la "$RESULTS_DIR"