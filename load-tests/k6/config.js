// k6/config.js â€” shared configuration for EduAirControl load tests
export const GATEWAY_URL = __ENV.GATEWAY_URL || 'http://localhost:8080';
export const SUPERADMIN_EMAIL = __ENV.SUPERADMIN_EMAIL || 'admin@eduaircontrol.com';
export const SUPERADMIN_PASSWORD = __ENV.SUPERADMIN_PASSWORD || '';

// Thresholds (stop criteria from PLAN-001 D7)
export const THRESHOLDS = {
  'http_req_duration': ['p(95)<1000', 'p(99)<2000'],
  'http_req_failed': ['rate<0.01'],
  'checks': ['rate>0.99'],
};

// Environment IDs from seed data (classroom service)
export const ENVIRONMENT_ID = '00000000-0000-4000-8000-000000000401'; // Aula 101
export const TEMPERATURE_VARIABLE_ID = '00000000-0000-4000-8000-000000000071';
export const CO2_VARIABLE_ID = '00000000-0000-4000-8000-000000000073';

// Measurement value ranges per variable
export const VALUE_RANGES = {
  temperature: { min: 18, max: 30 },
  humidity: { min: 30, max: 70 },
  co2: { min: 400, max: 1200 },
  noise: { min: 30, max: 60 },
};

export function randomValue(variable) {
  const r = VALUE_RANGES[variable] || VALUE_RANGES.temperature;
  return r.min + Math.random() * (r.max - r.min); // NOSONAR
}
