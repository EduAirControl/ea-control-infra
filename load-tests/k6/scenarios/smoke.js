// k6/scenarios/smoke.js — smoke test: 1 VU, 30s
import http from 'k6/http';
import { check, sleep } from 'k6';
import { GATEWAY_URL, THRESHOLDS, ENVIRONMENT_ID, TEMPERATURE_VARIABLE_ID } from '../config.js';
import { superAdminLogin, authHeaders } from '../helpers.js';

export const options = {
  vus: 1,
  duration: '30s',
  thresholds: THRESHOLDS,
};

export default function () {
  const sa = superAdminLogin();
  if (!sa) { sleep(1); return; }

  // Dashboard summary
  const summaryRes = http.get(`${GATEWAY_URL}/api/v1/dashboard/summary`, authHeaders(sa.token));
  check(summaryRes, { 'summary 200': (r) => r.status === 200 });

  // Dashboard series
  const seriesRes = http.get(`${GATEWAY_URL}/api/v1/dashboard/series?period=day&variable=temperature`, authHeaders(sa.token));
  check(seriesRes, { 'series 200': (r) => r.status === 200 });

  // Measurements history
  const histRes = http.get(`${GATEWAY_URL}/api/v1/environments/${ENVIRONMENT_ID}/measurements?limit=20`, authHeaders(sa.token));
  check(histRes, { 'history 200': (r) => r.status === 200 });

  sleep(1);
}
