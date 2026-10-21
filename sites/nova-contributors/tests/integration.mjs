import assert from 'node:assert/strict';
const base = 'http://127.0.0.1:4178'; // Local preview only; never create fixtures in production.
const sample = { requestId: crypto.randomUUID(), name: 'Local portal test', email: `fixture-${crypto.randomUUID()}@example.test`, level: 'Just starting', interests: ['compiler'], motivation: 'I would love to learn how a parser becomes a real compiler.', experience: '', availability: '3–5 hours / week', timezone: 'UTC', github: '', resume: '', consent: true, company: '' };
const post = (body, options = {}) => fetch(base + '/api/applications', { method: 'POST', headers: { 'content-type': 'application/json', origin: base, ...options }, body: JSON.stringify(body) });
let response = await post({ ...sample, consent: false }); assert.equal(response.status, 400);
response = await post(sample, { origin: 'https://outside.example.test' }); assert.equal(response.status, 403);
response = await post({ ...sample, motivation: 'x'.repeat(25000) }); assert.equal(response.status, 413);
response = await post(sample); const first = await response.json(); assert.equal(response.status, 201, JSON.stringify(first)); assert.match(first.reference, /^NOVA-\d{4}-[A-F0-9]{16}$/);
response = await post(sample); assert.equal(response.status, 200); assert.equal((await response.json()).reference, first.reference);
response = await post({ ...sample, name: 'Changed submission' }); assert.equal(response.status, 409);
for (let i = 0; i < 2; i++) { response = await post({ ...sample, requestId: crypto.randomUUID() }); assert.equal(response.status, 201); assert.notEqual((await response.json()).reference, first.reference); }
response = await post({ ...sample, requestId: crypto.randomUUID() }); assert.equal(response.status, 429);
assert.equal((await fetch(base + '/api/applications')).status, 405);
assert.equal((await fetch(base + '/api/applications/' + sample.requestId)).status, 404);
console.log('Real local D1 submission, idempotency, conflict protection, validation, size/origin/rate limits, and private-record access checks passed.');
console.log('Fixture email:', sample.email);
