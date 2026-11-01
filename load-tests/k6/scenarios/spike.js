// k6/scenarios/spike.js — sudden spike: 0 → 200 VUs in 10s
import http from 'k6/http';
import { check, sleep } from 'k6';
import { GATEWAY_URL, THRESHOLDS, ENVIRONMENT_ID } from '../config.js';
import { superAdminLogin, authHeaders } from '../helpers.js';

export const options = {
  stages: [
    { duration: '10s', target: 200 },
    { duration: '1m', target: 200 },
    { duration: '10s', target: 0 },
  ],
  thresholds: THRESHOLDS,
};

export default function () {
  const sa = superAdminLogin();
  if (!sa) { sleep(1); return; }
  const headers = authHeaders(sa.token);

  const res = http.get(`${GATEWAY_URL}/api/v1/dashboard/summary`, headers);
  check(res, { 'summary 200': (r) => r.status === 200 });

  sleep(Math.random() * 1 + 0.5);
}
