import { env } from 'cloudflare:workers';
import { validateApplication } from '../../../lib/application';
const reply = (data: object, status = 200) => Response.json(data, { status, headers: { 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff' } });
async function hash(value: string) { return [...new Uint8Array(await crypto.subtle.digest('SHA-256', new TextEncoder().encode(value)))].map(b => b.toString(16).padStart(2, '0')).join(''); }
export async function POST(request: Request) {
  if (request.headers.get('origin') && request.headers.get('origin') !== new URL(request.url).origin) return reply({ error: 'Please submit from the Nova portal.' }, 403);
  if (!request.headers.get('content-type')?.startsWith('application/json')) return reply({ error: 'Expected an application form.' }, 415);
  let input: unknown;
  try {
    const reader = request.body?.getReader(); if (!reader) return reply({ error: 'The application is empty.' }, 400);
    let size = 0; const chunks: Uint8Array[] = [];
    while (true) { const { done, value } = await reader.read(); if (done) break; size += value.length; if (size > 24000) { await reader.cancel(); return reply({ error: 'This application is too large.' }, 413); } chunks.push(value); }
    const buffer = new Uint8Array(size); let offset = 0; for (const chunk of chunks) { buffer.set(chunk, offset); offset += chunk.length; }
    input = JSON.parse(new TextDecoder().decode(buffer));
  } catch { return reply({ error: 'The application could not be read. Please try again.' }, 400); }
  const validated = validateApplication(input); if (!validated.ok) return reply({ error: validated.error }, 400);
  const a = validated.data;
  if (!env.DB) return reply({ error: 'Applications are temporarily unavailable. Your form has not been submitted; please try again later.' }, 503);
  try {
    const fingerprint = await hash(JSON.stringify({ ...a, requestId: undefined }));
    const existing = await env.DB.prepare('SELECT reference, fingerprint FROM applications WHERE id = ?').bind(a.requestId).first<{ reference: string; fingerprint: string }>();
    if (existing) return existing.fingerprint === fingerprint ? reply({ reference: existing.reference }) : reply({ error: 'A previous version may already have been saved. Use a new request ID to submit updated answers.' }, 409);
    const now = Date.now();
    // Only hash the proxy-supplied address. Never store the raw address.
    const ipHash = await hash('nova-contributors:' + (request.headers.get('cf-connecting-ip') || 'local-preview') + ':' + new Date(now).toISOString().slice(0, 10));
    const reference = 'NOVA-' + new Date(now).getUTCFullYear() + '-' + crypto.randomUUID().replaceAll('-', '').slice(0, 16).toUpperCase();
    const result = await env.DB.prepare(`INSERT INTO applications (id, reference, fingerprint, name, email, experience_level, interests, motivation, experience, availability, timezone, github_url, resume_url, created_at, consent_at, ip_hash)
      SELECT ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?
      WHERE (SELECT COUNT(*) FROM applications WHERE email = ? AND created_at > ?) < 3
      AND (SELECT COUNT(*) FROM applications WHERE ip_hash = ? AND created_at > ?) < 20
      ON CONFLICT(id) DO NOTHING`).bind(a.requestId, reference, fingerprint, a.name, a.email, a.level, JSON.stringify(a.interests), a.motivation, a.experience, a.availability, a.timezone, a.github, a.resume, now, now, ipHash, a.email, now - 86400000, ipHash, now - 3600000).run();
    if (!result.meta.changes) {
      const duplicate = await env.DB.prepare('SELECT reference, fingerprint FROM applications WHERE id = ?').bind(a.requestId).first<{ reference: string; fingerprint: string }>();
      if (duplicate?.fingerprint === fingerprint) return reply({ reference: duplicate.reference });
      return reply({ error: 'You’ve reached the application limit for now. Please try again later.' }, 429);
    }
    return reply({ reference }, 201);
  } catch { return reply({ error: 'We could not save your application. Your answers are still here; please try again.' }, 503); }
}
export async function GET() { return reply({ error: 'Application records are private.' }, 405); }
