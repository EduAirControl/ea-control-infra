// k6/scenarios/load.js — progressive load: 10 → 25 → 50 → 100 → 200 VUs
import http from 'k6/http';
import { check, sleep } from 'k6';
import { GATEWAY_URL, THRESHOLDS, ENVIRONMENT_ID, TEMPERATURE_VARIABLE_ID, CO2_VARIABLE_ID } from '../config.js';
import { superAdminLogin, authHeaders, randomMeasurement } from '../helpers.js';

export const options = {
  stages: [
    { duration: '30s', target: 10 },
    { duration: '1m', target: 10 },
    { duration: '30s', target: 25 },
    { duration: '1m', target: 25 },
    { duration: '30s', target: 50 },
    { duration: '1m', target: 50 },
    { duration: '30s', target: 100 },
    { duration: '1m', target: 100 },
    { duration: '30s', target: 200 },
    { duration: '1m', target: 200 },
    { duration: '30s', target: 0 },
  ],
  thresholds: THRESHOLDS,
};

export default function () {
  const sa = superAdminLogin();
  if (!sa) { sleep(1); return; }

  const headers = authHeaders(sa.token);

  // Weighted mix of operations
  const r = Math.random();
  if (r < 0.2) {
    // 20%: ingest single measurement
    const m = randomMeasurement('00000000-0000-4000-8000-000000000001', TEMPERATURE_VARIABLE_ID);
    const res = http.post(`${GATEWAY_URL}/api/v1/measurements`, JSON.stringify(m), headers);
    check(res, { 'ingest 201/409': (r) => r.status === 201 || r.status === 409 });
  } else if (r < 0.35) {
    // 15%: ingest batch
    const batch = {
      measurements: Array.from({ length: 10 }, () => randomMeasurement('00000000-0000-4000-8000-000000000001', TEMPERATURE_VARIABLE_ID)),
    };
    const res = http.post(`${GATEWAY_URL}/api/v1/measurements/batch`, JSON.stringify(batch), headers);
    check(res, { 'batch 200': (r) => r.status === 200 });
  } else if (r < 0.55) {
    // 20%: history query
    const res = http.get(`${GATEWAY_URL}/api/v1/environments/${ENVIRONMENT_ID}/measurements?limit=20`, headers);
    check(res, { 'history 200': (r) => r.status === 200 });
  } else if (r < 0.75) {
    // 20%: dashboard summary
    const res = http.get(`${GATEWAY_URL}/api/v1/dashboard/summary`, headers);
    check(res, { 'summary 200': (r) => r.status === 200 });
  } else {
    // 25%: dashboard series
    const res = http.get(`${GATEWAY_URL}/api/v1/dashboard/series?period=day&variable=temperature`, headers);
    check(res, { 'series 200': (r) => r.status === 200 });
  }

  sleep(Math.random() * 2 + 0.5);
}
