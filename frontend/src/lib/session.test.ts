import { describe, expect, it } from 'vitest'
import { readSession } from './session'

const NOW = 1_700_000_000_000
const b64url = (value: unknown) =>
  btoa(JSON.stringify(value)).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '')
const jwt = (payload: unknown) => `eyJhbGciOiJIUzI1NiJ9.${b64url(payload)}.signature`

describe('readSession', () => {
  const valid = { userId: 'u-1', email: 'ada@example.com', exp: NOW / 1000 + 3600 }

  it('reads the user and expiry from a valid token', () => {
    const token = jwt(valid)
    expect(readSession(token, NOW)).toEqual({
      token,
      user: { userId: 'u-1', email: 'ada@example.com' },
      expiresAt: valid.exp * 1000,
    })
  })

  it('handles non-ASCII payloads (UTF-8 + base64url)', () => {
    const token = `x.${btoa(String.fromCharCode(...new TextEncoder().encode(JSON.stringify({ ...valid, email: 'jürgen@example.com' })))).replace(/=+$/, '')}.y`
    expect(readSession(token, NOW)?.user.email).toBe('jürgen@example.com')
  })

  it('rejects an expired token', () => {
    expect(readSession(jwt({ ...valid, exp: NOW / 1000 - 1 }), NOW)).toBeNull()
  })

  it('rejects tokens missing required claims', () => {
    expect(readSession(jwt({ email: 'a@b.co', exp: valid.exp }), NOW)).toBeNull()
    expect(readSession(jwt({ userId: 'u-1', exp: valid.exp }), NOW)).toBeNull()
    expect(readSession(jwt({ userId: 'u-1', email: 'a@b.co' }), NOW)).toBeNull()
  })

  it('rejects empty and malformed tokens', () => {
    expect(readSession(null, NOW)).toBeNull()
    expect(readSession('', NOW)).toBeNull()
    expect(readSession('not-a-jwt', NOW)).toBeNull()
    expect(readSession('a.%%%.c', NOW)).toBeNull()
  })
})
