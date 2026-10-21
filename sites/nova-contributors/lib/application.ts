export type ApplicationData = { requestId: string; name: string; email: string; level: string; interests: string[]; motivation: string; experience: string; availability: string; timezone: string; github: string; resume: string; consent: boolean; company: string };
export function validateApplication(input: unknown): { ok: true; data: ApplicationData } | { ok: false; error: string } {
  if (!input || typeof input !== 'object') return { ok: false, error: 'Please complete the application.' };
  const raw = input as Record<string, unknown>;
  const bounded = (key: string, limit: number) => typeof raw[key] === 'string' && (raw[key] as string).length <= limit ? (raw[key] as string).trim() : null;
  const name = bounded('name', 100), email = bounded('email', 254)?.toLowerCase();
  const motivation = bounded('motivation', 2000), experience = bounded('experience', 2000);
  const timezone = bounded('timezone', 80), github = bounded('github', 2048), resume = bounded('resume', 2048);
  if (!name || !email || !/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(email)) return { ok: false, error: 'Please enter your name and a valid email address.' };
  if (motivation === null || motivation.length < 20 || experience === null || timezone === null || github === null || resume === null) return { ok: false, error: 'Tell us a little about your interests (at least 20 characters), and keep your answers within the field limits.' };
  if (!['Just starting', 'Some experience', 'Experienced'].includes(String(raw.level)) || !['Under 3 hours / week', '3–5 hours / week', '5–10 hours / week', '10+ hours / week', 'Let’s discuss'].includes(String(raw.availability))) return { ok: false, error: 'Choose your experience level and availability.' };
  const interests = raw.interests;
  if (!Array.isArray(interests) || !interests.length || interests.length > 4 || interests.some(id => !['compiler', 'tooling', 'developer', 'runtime'].includes(id))) return { ok: false, error: 'Choose at least one contribution track.' };
  for (const link of [github, resume]) {
    if (!link) continue;
    try { const url = new URL(link); if (url.protocol !== 'https:' || url.username || url.password) throw new Error(); } catch { return { ok: false, error: 'Portfolio and resume links must be valid HTTPS links.' }; }
  }
  if (raw.consent !== true) return { ok: false, error: 'Please acknowledge how your application information will be used.' };
  if (typeof raw.requestId !== 'string' || !/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i.test(raw.requestId) || raw.company !== '') return { ok: false, error: 'Please refresh the application and try again.' };
  return { ok: true, data: { requestId: raw.requestId, name, email, motivation, experience, timezone, github, resume, interests: [...new Set(interests)].sort(), level: String(raw.level), availability: String(raw.availability), consent: true, company: '' } };
}
