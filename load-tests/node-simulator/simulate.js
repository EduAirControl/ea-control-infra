// node-simulator/simulate.js — simulates N ESP32 nodes sending measurements
// Usage: node simulate.js --nodes 10 --interval 10000 --duration 60000
// Requires: node 18+, npm install (none needed, uses fetch)

const args = process.argv.slice(2);
const getArg = (name, def) => {
  const idx = args.indexOf('--' + name);
  return idx >= 0 ? args[idx + 1] : def;
};

const GATEWAY = process.env.GATEWAY_URL || 'http://localhost:8080';
const NODES = parseInt(getArg('nodes', '10'));
const INTERVAL = parseInt(getArg('interval', '10000'));
const DURATION = parseInt(getArg('duration', '60000'));
const TOKEN = process.env.DEVICE_TOKEN || '';
const INSTALLATION_ID = process.env.INSTALLATION_ID || '00000000-0000-4000-8000-000000000001';
const VARIABLE_ID = process.env.VARIABLE_ID || '00000000-0000-4000-8000-000000000071';

console.log(`Simulating ${NODES} nodes, interval ${INTERVAL}ms, duration ${DURATION}ms`);
console.log(`Gateway: ${GATEWAY}`);

function randomValue() {
  return 18 + Math.random() * 12;
}

function nowIso() {
  return new Date().toISOString();
}

async function sendMeasurement(nodeId) {
  const body = JSON.stringify({
    sensorInstallationId: INSTALLATION_ID,
    variableId: VARIABLE_ID,
    value: randomValue(),
    measuredAt: nowIso(),
  });
  try {
    const res = await fetch(`${GATEWAY}/api/v1/measurements`, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'Authorization': `Bearer ${TOKEN}`,
      },
      body,
    });
    if (!res.ok) {
      console.error(`Node ${nodeId}: HTTP ${res.status}`);
    }
  } catch (e) {
    console.error(`Node ${nodeId}: ${e.message}`);
  }
}

const nodes = Array.from({ length: NODES }, (_, i) => i + 1);
const startTime = Date.now();
let sent = 0;

const interval = setInterval(() => {
  if (Date.now() - startTime >= DURATION) {
    clearInterval(interval);
    console.log(`Done. Sent ${sent} measurements.`);
    process.exit(0);
  }
  for (const nodeId of nodes) {
    sendMeasurement(nodeId).then(() => sent++);
  }
}, INTERVAL);

console.log('Simulation started. Press Ctrl+C to stop.');
