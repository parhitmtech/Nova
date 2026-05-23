import { useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { login, signup } from '../api/auth'
import { useAuth } from '../contexts/AuthContext'

export default function LoginPage() {
  const navigate = useNavigate()
  const { saveToken } = useAuth()

  // Single page handles both login and sign-up — isLogin toggles which API call is made.
  const [isLogin, setIsLogin] = useState(true)
  const [email,    setEmail]    = useState('')
  const [password, setPassword] = useState('')
  const [error,    setError]    = useState('')
  const [loading,  setLoading]  = useState(false)

  const handleSubmit = async (e: { preventDefault(): void }) => {
    e.preventDefault()
    setError('')
    setLoading(true)
    try {
      // Reuse the same form for both flows — only the API function differs.
      const fn = isLogin ? login : signup
      const res = await fn(email, password)
      // saveToken persists the JWT to localStorage and updates AuthContext,
      // which causes App.tsx to redirect to /ide via the auth guard.
      saveToken(res.data.token)
      navigate('/ide')
    } catch (err: unknown) {
      // Surface the server's error message (e.g. "Invalid password") if available,
      // otherwise fall back to the generic network error message.
      const msg =
        err instanceof Error
          ? (err as { response?: { data?: { error?: string } } }).response?.data?.error ?? err.message
          : 'Something went wrong'
      setError(msg)
    } finally {
      setLoading(false)
    }
  }

  // Switching modes clears the error so stale messages don't confuse the user.
  const toggle = () => {
    setIsLogin(v => !v)
    setError('')
  }

  return (
    <div
      className="min-h-screen flex items-center justify-center"
      style={{ background: '#0d1117' }}
    >
      {/* Back to landing page */}
      <button
        onClick={() => { window.location.href = '/' }}
        className="absolute top-4 left-4 flex items-center gap-1.5 px-3 py-1.5 rounded-md text-xs font-medium"
        style={{ background: '#21262d', color: '#8b949e', border: '1px solid #30363d' }}
      >
        ← Home
      </button>

      <div
        className="w-full max-w-md p-8 rounded-xl"
        style={{ background: '#161b22', border: '1px solid #30363d' }}
      >
        {/* Branding */}
        <div className="text-center mb-8">
          <div
            className="inline-flex items-center justify-center w-14 h-14 rounded-xl mb-4"
            style={{ background: '#21262d', border: '1px solid #30363d' }}
          >
            <svg
              width="28" height="28" viewBox="0 0 24 24"
              fill="none" stroke="#58a6ff" strokeWidth="2"
              strokeLinecap="round" strokeLinejoin="round"
            >
              <polyline points="16 18 22 12 16 6" />
              <polyline points="8 6 2 12 8 18" />
            </svg>
          </div>
          <h1 className="text-2xl font-bold" style={{ color: '#c9d1d9' }}>NovaComp</h1>
          <p className="text-sm mt-1" style={{ color: '#8b949e' }}>
            {isLogin ? 'Sign in to your account' : 'Create a new account'}
          </p>
        </div>

        {/* Login / Sign Up tab toggle */}
        <div className="flex rounded-lg p-1 mb-6" style={{ background: '#21262d' }}>
          {(['Login', 'Sign Up'] as const).map((label, i) => {
            const active = isLogin ? i === 0 : i === 1
            return (
              <button
                key={label}
                type="button"
                onClick={toggle}
                className="flex-1 py-2 text-sm font-medium rounded-md transition-colors"
                style={
                  active
                    ? { background: '#30363d', color: '#c9d1d9' }
                    : { background: 'transparent', color: '#8b949e' }
                }
              >
                {label}
              </button>
            )
          })}
        </div>

        {/* Shared email + password form for both login and sign-up */}
        <form onSubmit={handleSubmit} className="space-y-4">
          <div>
            <label className="block text-sm font-medium mb-1.5" style={{ color: '#8b949e' }}>
              Email
            </label>
            <input
              type="email"
              value={email}
              onChange={e => setEmail(e.target.value)}
              required
              placeholder="you@example.com"
              className="w-full px-3 py-2.5 rounded-lg text-sm outline-none"
              style={{ background: '#0d1117', border: '1px solid #30363d', color: '#c9d1d9' }}
              onFocus={e => (e.target.style.borderColor = '#58a6ff')}
              onBlur={e  => (e.target.style.borderColor = '#30363d')}
            />
          </div>

          <div>
            <label className="block text-sm font-medium mb-1.5" style={{ color: '#8b949e' }}>
              Password
            </label>
            <input
              type="password"
              value={password}
              onChange={e => setPassword(e.target.value)}
              required
              placeholder="••••••••"
              className="w-full px-3 py-2.5 rounded-lg text-sm outline-none"
              style={{ background: '#0d1117', border: '1px solid #30363d', color: '#c9d1d9' }}
              onFocus={e => (e.target.style.borderColor = '#58a6ff')}
              onBlur={e  => (e.target.style.borderColor = '#30363d')}
            />
          </div>

          {/* Server-side error message (e.g. wrong password, duplicate email) */}
          {error && (
            <div
              className="px-3 py-2.5 rounded-lg text-sm"
              style={{
                background: 'rgba(248,81,73,0.1)',
                border: '1px solid rgba(248,81,73,0.4)',
                color: '#f85149',
              }}
            >
              {error}
            </div>
          )}

          <button
            type="submit"
            disabled={loading}
            className="w-full py-2.5 rounded-lg text-sm font-semibold transition-opacity disabled:opacity-60"
            style={{ background: '#1f6feb', color: '#ffffff' }}
            onMouseEnter={e => !loading && (e.currentTarget.style.background = '#388bfd')}
            onMouseLeave={e => (e.currentTarget.style.background = '#1f6feb')}
          >
            {loading ? 'Please wait…' : isLogin ? 'Sign in' : 'Create account'}
          </button>
        </form>

        {/* Secondary toggle link below the form */}
        <p className="text-center text-xs mt-6" style={{ color: '#8b949e' }}>
          {isLogin ? "Don't have an account? " : 'Already have an account? '}
          <button type="button" onClick={toggle} className="underline" style={{ color: '#58a6ff' }}>
            {isLogin ? 'Sign up' : 'Sign in'}
          </button>
        </p>
      </div>
    </div>
  )
}
