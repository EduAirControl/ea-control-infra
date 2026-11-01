// k6/scenarios/stress.js — stress: ramp up until failure (p95 > 1s or errors > 1%)
import http from 'k6/http';
import { check, sleep } from 'k6';
import { GATEWAY_URL, THRESHOLDS, ENVIRONMENT_ID, TEMPERATURE_VARIABLE_ID } from '../config.js';
import { superAdminLogin, authHeaders, randomMeasurement } from '../helpers.js';

export const options = {
  stages: [
    { duration: '2m', target: 50 },
    { duration: '2m', target: 100 },
    { duration: '2m', target: 200 },
    { duration: '2m', target: 300 },
    { duration: '2m', target: 400 },
    { duration: '1m', target: 0 },
  ],
  thresholds: THRESHOLDS,
};

export default function () {
  const sa = superAdminLogin();
  if (!sa) { sleep(1); return; }
  const headers = authHeaders(sa.token);

  const m = randomMeasurement('00000000-0000-4000-8000-000000000001', TEMPERATURE_VARIABLE_ID);
  const res = http.post(`${GATEWAY_URL}/api/v1/measurements`, JSON.stringify(m), headers);
  check(res, { 'ingest ok': (r) => r.status === 201 || r.status === 409 });

  sleep(Math.random() * 0.5 + 0.2);
}
