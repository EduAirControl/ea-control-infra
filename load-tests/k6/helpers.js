// k6/helpers.js â€” authentication and data helpers
import { GATEWAY_URL, SUPERADMIN_EMAIL, SUPERADMIN_PASSWORD } from './config.js';
import http from 'k6/http';
import { check } from 'k6';

export function login(email, password) {
  const res = http.post(`${GATEWAY_URL}/api/v1/auth/login`, JSON.stringify({
    email,
    password,
  }), { headers: { 'Content-Type': 'application/json' } });
  check(res, { 'login 200': (r) => r.status === 200 });
  if (res.status !== 200) return null;
  const body = JSON.parse(res.body);
  return { token: body.accessToken, refreshToken: body.refreshToken, user: body.user };
}

export function superAdminLogin() {
  return login(SUPERADMIN_EMAIL, SUPERADMIN_PASSWORD);
}

export function authHeaders(token) {
  return { headers: { 'Authorization': `Bearer ${token}`, 'Content-Type': 'application/json' } };
}

export function nowIso() {
  return new Date().toISOString();
}

export function randomMeasurement(installationId, variableId) {
  return {
    sensorInstallationId: installationId,
    variableId: variableId,
    value: 20 + Math.random() * 10, // NOSONAR
    measuredAt: nowIso(),
  };
}
