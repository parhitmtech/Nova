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
  const [token, setToken] = useState<string | null>(
    localStorage.getItem('novacomp_token')
  )
  const [user, setUser] = useState<User | null>(null)

  useEffect(() => {
    if (token) {
      try {
        const payload = JSON.parse(atob(token.split('.')[1]))
        setUser({ email: payload.email, userId: payload.userId })
      } catch {
        setUser(null)
      }
    } else {
      setUser(null)
    }
  }, [token])

  const saveToken = (t: string) => {
    localStorage.setItem('novacomp_token', t)
    setToken(t)
  }

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

export function useAuth() {
  const ctx = useContext(AuthContext)
  if (!ctx) throw new Error('useAuth must be used within AuthProvider')
  return ctx
}
