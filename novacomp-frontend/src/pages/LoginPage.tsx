import { useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { Code2, ArrowRight, Eye, EyeOff, Loader2 } from 'lucide-react'
import { login, signup } from '../api/auth'
import { useAuth } from '../contexts/AuthContext'
import { errorMessage } from '../lib/workspace'
export default function LoginPage() {
  const navigate = useNavigate()
  const { saveToken } = useAuth()
  const [isLogin, setIsLogin] = useState(true)
  const [email, setEmail] = useState('')
  const [password, setPassword] = useState('')
  const [visible, setVisible] = useState(false)
  const [error, setError] = useState('')
  const [loading, setLoading] = useState(false)
  const submit = async (e: React.FormEvent) => {
    e.preventDefault(); setError(''); setLoading(true)
    try { const res = await (isLogin ? login : signup)(email.trim(), password); saveToken(res.data.token); navigate('/ide') }
    catch (err) { setError(errorMessage(err, 'Unable to connect. Please try again.')) }
    finally { setLoading(false) }
  }
  const mode = (loginMode: boolean) => { setIsLogin(loginMode); setError('') }
  return <main className="auth-page"><section className="auth-story"><a className="brand" href="/"><span className="brand-symbol"><Code2 size={23}/></span><span>Nova<span className="brand-light">Comp</span><small>AN IDEA. A FEW LINES. A NEW POSSIBILITY.</small></span></a><div className="eyebrow">BUILT FOR YOUR NEXT IDEA</div><h1>Small language.<br/><span>Big possibilities.</span></h1><p>Write, run, and explore Nova in one focused workspace. From your first function to your next experiment.</p><div className="auth-code"><div className="code-label"><span className="status-dot"/> hello.nova</div><pre>{'main() {\n    print("Hello, possibility.") ;\n}'}</pre></div></section><section className="auth-form-side"><div className="auth-card"><h2>{isLogin ? 'Welcome back.' : 'Start something new.'}</h2><p>{isLogin ? 'Your ideas are right where you left them.' : 'Create your account and make room for your ideas.'}</p><div className="auth-tabs" aria-label="Account access"><button className={isLogin ? 'active' : ''} aria-pressed={isLogin} disabled={loading} onClick={() => mode(true)}>Sign in</button><button className={!isLogin ? 'active' : ''} aria-pressed={!isLogin} disabled={loading} onClick={() => mode(false)}>Create account</button></div><form onSubmit={submit}><div><label className="form-label" htmlFor="email">Email address</label><input className="form-input" id="email" name="email" type="email" autoComplete="email" placeholder="you@example.com" value={email} onChange={e => setEmail(e.target.value)} required disabled={loading}/></div><div><label className="form-label" htmlFor="password">Password</label><div className="password-field"><input className="form-input" id="password" name="password" type={visible ? 'text' : 'password'} autoComplete={isLogin ? 'current-password' : 'new-password'} value={password} onChange={e => setPassword(e.target.value)} required placeholder={isLogin ? 'Enter your password' : 'Choose a strong password'} disabled={loading}/><button type="button" className="icon-button" aria-label={visible ? 'Hide password' : 'Show password'} aria-pressed={visible} onClick={() => setVisible(v => !v)}>{visible ? <EyeOff size={16}/> : <Eye size={16}/>}</button></div></div>{error && <div className="form-error" role="alert">{error}</div>}<button type="submit" className="button primary auth-submit" disabled={loading}>{loading ? <><Loader2 size={16} className="animate-spin"/> Please wait…</> : <>{isLogin ? 'Open my workspace' : 'Create my account'}<ArrowRight size={16}/></>}</button></form><p className="auth-note">A focused space to write code, try ideas,<br/>and learn something along the way.</p></div></section></main>
}
