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

export function AuthProvider({ children }: { children: ReactNode }) {
  const [session, setSession] = useState<Session | null>(() =>
    readSession(localStorage.getItem('novacomp_token')),
  )
  const logout = useCallback(() => {
    localStorage.removeItem('novacomp_token')
    setSession(null)
  }, [])
  const saveToken = useCallback((token: string) => {
    const next = readSession(token)
    if (!next) throw new Error('The server returned an invalid or expired session. Please sign in again.')
    localStorage.setItem('novacomp_token', token)
    setSession(next)
  }, [])
  useEffect(() => {
    const sync = (event: StorageEvent) => {
      if (event.key === 'novacomp_token') setSession(readSession(event.newValue))
    }
    window.addEventListener('novacomp:session-expired', logout)
    window.addEventListener('storage', sync)
    return () => {
      window.removeEventListener('novacomp:session-expired', logout)
      window.removeEventListener('storage', sync)
    }
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
