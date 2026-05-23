import { createContext, useContext, useState, useEffect, type ReactNode } from 'react'

interface User {
  email: string
  userId: string
}

interface AuthContextType {
  token: string | null
  user: User | null
  saveToken: (t: string) => void
  logout: () => void
}

const AuthContext = createContext<AuthContextType | null>(null)

export function AuthProvider({ children }: { children: ReactNode }) {
  // Initialise from localStorage so the user stays logged in across page refreshes.
  // If the stored token is invalid or expired, the API calls will 401 and the
  // user will be effectively logged out when they next try an action.
  const [token, setToken] = useState<string | null>(
    localStorage.getItem('novacomp_token')
  )
  const [user, setUser] = useState<User | null>(null)

  // Decode the JWT payload (middle segment, base64url) to extract email and
  // userId without an extra /me round-trip. We don't verify the signature here —
  // that happens server-side on every protected API call.
  useEffect(() => {
    if (token) {
      try {
        const payload = JSON.parse(atob(token.split('.')[1]))
        setUser({ email: payload.email, userId: payload.userId })
      } catch {
        // Malformed token — treat as logged out
        setUser(null)
      }
    } else {
      setUser(null)
    }
  }, [token])

  // Persist the token to localStorage so it survives hard refreshes,
  // then update in-memory state so components re-render immediately.
  const saveToken = (t: string) => {
    localStorage.setItem('novacomp_token', t)
    setToken(t)
  }

  // Remove all auth state from both memory and localStorage.
  const logout = () => {
    localStorage.removeItem('novacomp_token')
    setToken(null)
    setUser(null)
  }

  return (
    <AuthContext.Provider value={{ token, user, saveToken, logout }}>
      {children}
    </AuthContext.Provider>
  )
}

// Convenience hook — throws if called outside the provider so misuse
// surfaces at development time rather than silently producing null.
export function useAuth() {
  const ctx = useContext(AuthContext)
  if (!ctx) throw new Error('useAuth must be used within AuthProvider')
  return ctx
}
