import { lazy, Suspense, useEffect, useState } from 'react'
import { useParams, useNavigate } from 'react-router-dom'
import { Loader2, Copy, Check, ExternalLink, Code2, Download, FileCode, AlertCircle } from 'lucide-react'
import { getShare } from '../api/share'
import { registerNovaLanguage } from '../lib/novaLanguage'
import { downloadBlob } from '../lib/workspace'
const MonacoEditor = lazy(() => import('../components/NovaEditor'))
export default function SharePage() {
  const { id } = useParams<{ id: string }>()
  const navigate = useNavigate()
  const [code, setCode] = useState('')
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState('')
  const [copied, setCopied] = useState(false)
  const [copyError, setCopyError] = useState('')
  useEffect(() => {
    let active = true
    if (!id) return
    getShare(id).then(res => { if (active) { if (typeof res.data.code !== 'string') throw new Error('Invalid snippet'); setCode(res.data.code) } }).catch(() => { if (active) setError('This snippet could not be found. The link may have expired.') }).finally(() => { if (active) setLoading(false) })
    return () => { active = false }
  }, [id])
  const copy = async () => {
    try { await navigator.clipboard.writeText(code); setCopied(true); setCopyError(''); setTimeout(() => setCopied(false), 2000) }
    catch { setCopyError('Clipboard access is unavailable. Download the source or select it in the editor.') }
  }
  const fork = () => { sessionStorage.setItem('novacomp_fork', code); navigate('/ide') }
  return <main className="share-shell"><header className="share-toolbar"><a className="brand" href="/"><span className="brand-symbol"><Code2 size={20}/></span><span>Nova<span className="brand-light">Comp</span><small>SHARED IDEAS START HERE</small></span></a><div><button className="button" onClick={() => void copy()} disabled={loading || !!error}>{copied ? <Check size={14}/> : <Copy size={14}/>} {copied ? 'Copied' : 'Copy'}</button><button className="button" aria-label="Download shared source" onClick={() => downloadBlob(new Blob([code], { type: 'text/plain' }), 'shared.nova')} disabled={loading || !!error}><Download size={14}/></button><button className="button primary" onClick={fork} disabled={loading || !!error}><ExternalLink size={14}/>Open in IDE</button></div></header><div className="share-meta"><FileCode size={15}/>shared.nova<span className="share-badge">READ ONLY</span><span style={{ marginLeft: 'auto' }}>{code.split('\n').length} lines · Nova</span></div>{copyError && <div role="alert" className="form-error" style={{ padding: '0 20px' }}>{copyError}</div>}<section className="share-content" aria-label="Shared source code">{loading ? <div className="editor-loading"><Loader2 className="animate-spin" size={20}/>Loading shared code…</div> : error ? <div className="share-error"><AlertCircle size={32}/><h1>Snippet unavailable</h1><p>{error}</p><button className="button primary" onClick={() => navigate('/ide')}>Go to workspace</button></div> : <Suspense fallback={<div className="editor-loading">Preparing editor…</div>}><MonacoEditor height="100%" language="nova" theme="nova-dark" value={code} beforeMount={registerNovaLanguage} options={{ readOnly: true, domReadOnly: true, fontSize: 14, minimap: { enabled: false }, scrollBeyondLastLine: false, padding: { top: 22 }, wordWrap: 'on', automaticLayout: true }}/></Suspense>}</section></main>
}
