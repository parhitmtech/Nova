export interface User {
  email: string
  userId: string
}

export interface Session {
  token: string
  user: User
  expiresAt: number
}

// Decode the session from a JWT without verifying it (the server verifies every request).
// Returns null for anything that is not an unexpired token carrying a userId and email.
export function readSession(token: string | null, now = Date.now()): Session | null {
  if (!token) return null
  try {
    const part = token.split('.')[1]
    if (!part) return null // not a JWT
    const payload = part.replace(/-/g, '+').replace(/_/g, '/')
    const decoded = JSON.parse(
      new TextDecoder().decode(Uint8Array.from(atob(payload), (char) => char.charCodeAt(0))),
    )
    if (
      typeof decoded.exp !== 'number' ||
      decoded.exp * 1000 <= now ||
      typeof decoded.userId !== 'string' ||
      typeof decoded.email !== 'string'
    )
      return null
    return { token, user: { email: decoded.email, userId: decoded.userId }, expiresAt: decoded.exp * 1000 }
  } catch {
    return null
  }
}
