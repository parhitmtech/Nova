import { lazy, Suspense, useEffect, useState } from 'react'
import { useParams, useNavigate } from 'react-router-dom'
import { Loader2, Copy, Check, ExternalLink } from 'lucide-react'
import { getShare } from '../api/share'
import { registerNovaLanguage } from '../lib/novaLanguage'

const MonacoEditor = lazy(() => import('@monaco-editor/react'))

export default function SharePage() {
  const { id } = useParams<{ id: string }>()
  const navigate = useNavigate()
  const [code, setCode] = useState('')
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState('')
  const [copied, setCopied] = useState(false)

  useEffect(() => {
    if (!id) return
    getShare(id)
      .then(res => setCode(res.data.code))
      .catch(() => setError('Share not found or has expired.'))
      .finally(() => setLoading(false))
  }, [id])

  const handleCopy = async () => {
    await navigator.clipboard.writeText(code)
    setCopied(true)
    setTimeout(() => setCopied(false), 2000)
  }

  const handleOpenIDE = () => {
    sessionStorage.setItem('novacomp_fork', code)
    navigate('/ide')
  }

  return (
    <div className="flex flex-col" style={{ height: '100vh', background: '#0d1117', color: '#c9d1d9' }}>
      {/* Header */}
      <div
        className="flex items-center justify-between px-4 py-2 shrink-0"
        style={{ background: '#161b22', borderBottom: '1px solid #30363d' }}
      >
        <div className="flex items-center gap-2">
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="#00d4ff" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
            <polyline points="16 18 22 12 16 6" /><polyline points="8 6 2 12 8 18" />
          </svg>
          <span className="font-semibold text-sm" style={{ color: '#c9d1d9' }}>NovaComp</span>
          <span style={{ color: '#30363d' }}>/</span>
          <span className="text-sm font-mono" style={{ color: '#8b949e' }}>shared snippet</span>
        </div>
        <div className="flex items-center gap-2">
          <button
            onClick={handleCopy}
            disabled={!code}
            className="flex items-center gap-1.5 px-3 py-1.5 rounded-md text-xs font-medium disabled:opacity-50"
            style={{ background: '#21262d', color: '#c9d1d9', border: '1px solid #30363d' }}
          >
            {copied ? <Check size={12} /> : <Copy size={12} />}
            {copied ? 'Copied!' : 'Copy code'}
          </button>
          <button
            onClick={handleOpenIDE}
            disabled={!code}
            className="flex items-center gap-1.5 px-3 py-1.5 rounded-md text-xs font-medium disabled:opacity-50"
            style={{ background: '#1f6feb', color: '#ffffff', border: '1px solid #1f6feb' }}
          >
            <ExternalLink size={12} /> Fork in IDE
          </button>
        </div>
      </div>

      {/* Content */}
      {loading && (
        <div className="flex items-center justify-center flex-1 gap-2" style={{ color: '#8b949e' }}>
          <Loader2 size={18} className="animate-spin" />
          Loading snippet…
        </div>
      )}

      {error && (
        <div className="flex items-center justify-center flex-1">
          <p className="text-sm" style={{ color: '#f85149' }}>{error}</p>
        </div>
      )}

      {!loading && !error && (
        <div className="flex-1 min-h-0">
          <Suspense fallback={
            <div className="flex items-center justify-center h-full gap-2" style={{ color: '#8b949e' }}>
              <Loader2 size={18} className="animate-spin" /> Loading editor…
            </div>
          }>
            <MonacoEditor
              height="100%"
              language="nova"
              theme="nova-dark"
              value={code}
              beforeMount={registerNovaLanguage}
              options={{
                readOnly: true,
                fontSize: 14,
                fontFamily: 'ui-monospace, Consolas, monospace',
                minimap: { enabled: false },
                scrollBeyondLastLine: false,
                padding: { top: 16 },
                wordWrap: 'on',
                domReadOnly: true,
                renderValidationDecorations: 'off',
              }}
            />
          </Suspense>
        </div>
      )}
    </div>
  )
}
