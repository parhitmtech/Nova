import { test } from 'node:test';
import assert from 'node:assert/strict';
import { validateApplication } from '../lib/application.ts';
const valid = () => ({ requestId: crypto.randomUUID(), name: 'New contributor', email: 'person@example.test', level: 'Just starting', interests: ['compiler'], motivation: 'I would love to learn how parsing and compilation work.', availability: '3–5 hours / week', timezone: 'UTC', github: '', resume: '', experience: '', consent: true, company: '' });
test('a newcomer can submit without experience, a degree or a resume', () => {
  const result = validateApplication(valid()); assert.equal(result.ok, true);
});
test('reject invalid contact, links, consent, tracks and oversized fields', () => {
  for (const change of [{ email: 'bad' }, { github: 'javascript:alert(1)' }, { resume: 'http://unsafe.test/file' }, { consent: false }, { interests: ['nonexistent'] }, { name: 'x'.repeat(101) }, { requestId: 'guessable' }]) assert.equal(validateApplication({ ...valid(), ...change }).ok, false);
});
test('normalize email and whitespace and refuse spam honeypot', () => {
  const result = validateApplication({ ...valid(), name: '  New contributor ', email: 'Person@Example.Test' });
  assert.equal(result.ok, true); assert.equal(result.data.email, 'person@example.test'); assert.equal(result.data.name, 'New contributor');
  assert.equal(validateApplication({ ...valid(), company: 'spam' }).ok, false);
});
