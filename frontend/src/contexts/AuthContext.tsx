/* eslint-disable react-refresh/only-export-components -- this module exposes the provider and its context hook */
import { createContext, useCallback, useContext, useEffect, useState, type ReactNode } from 'react'
import { readSession, type Session, type User } from '../lib/session'

interface AuthContextType {
  token: string | null
  user: User | null
  saveToken: (token: string) => void
  logout: () => void
}
const AuthContext = createContext<AuthContextType | null>(null)

// Tokens are tab-scoped (sessionStorage), so a token never outlives its tab's session.
export function AuthProvider({ children }: { children: ReactNode }) {
  // Discard long-lived tokens retained by older versions of the client.
  useEffect(() => localStorage.removeItem('novacomp_token'), [])
  const [session, setSession] = useState<Session | null>(() =>
    readSession(sessionStorage.getItem('novacomp_token')),
  )
  const logout = useCallback(() => {
    sessionStorage.removeItem('novacomp_token')
    setSession(null)
  }, [])
  const saveToken = useCallback((token: string) => {
    const next = readSession(token)
    if (!next) throw new Error('The server returned an invalid or expired session. Please sign in again.')
    sessionStorage.setItem('novacomp_token', token)
    setSession(next)
  }, [])
  useEffect(() => {
    window.addEventListener('novacomp:session-expired', logout)
    return () => window.removeEventListener('novacomp:session-expired', logout)
  }, [logout])
  useEffect(() => {
    if (!session) return
    const timer = setTimeout(logout, Math.min(session.expiresAt - Date.now(), 2_147_483_647))
    return () => clearTimeout(timer)
  }, [session, logout])
  return (
    <AuthContext.Provider
      value={{ token: session?.token ?? null, user: session?.user ?? null, saveToken, logout }}
    >
      {children}
    </AuthContext.Provider>
  )
}

export function useAuth() {
  const context = useContext(AuthContext)
  if (!context) throw new Error('useAuth must be used within AuthProvider')
  return context
}
