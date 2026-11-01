# Load Tests — EduAirControl

## Prerequisites
- k6 installed (https://k6.io/docs/get-started/installation/) or Docker.
- Stack running (`docker compose up -d`).
- `SUPERADMIN_PASSWORD` set (from `infra/.env`).

## Usage

```bash
# Run all scenarios
cd load-tests/k6
GATEWAY_URL=http://localhost:8080 \
SUPERADMIN_EMAIL=admin@eduaircontrol.com \
SUPERADMIN_PASSWORD=your_password \
bash run.sh

# Run a single scenario
k6 run -e GATEWAY_URL=http://localhost:8080 \
       -e SUPERADMIN_EMAIL=admin@eduaircontrol.com \
       -e SUPERADMIN_PASSWORD=your_password \
       scenarios/load.js

# Node simulator (offline buffer testing)
cd load-tests/node-simulator
GATEWAY_URL=http://localhost:8080 \
DEVICE_TOKEN=your_device_token \
INSTALLATION_ID=your_installation_uuid \
node simulate.js --nodes 10 --interval 10000 --duration 60000
```

## Scenarios
| Scenario | VUs | Duration | Purpose |
|---|---|---|---|
| smoke | 1 | 30s | Basic health check under load |
| load | 10→200 | ~8 min | Progressive capacity test |
| spike | 0→200 | ~1m20s | Sudden traffic spike |
| stress | 50→400 | ~10 min | Find breaking point |

## Stop criteria (PLAN-001 D7)
- p95 > 1 second → stop increasing load
- Error rate > 1% → stop
- Record the last stable VU count as system capacity

## Results
Results are saved in `load-tests/results/` as JSON logs. Summarize in a capacity report (ambientes/RPS/p95/errores/CPU/RAM/disco).
