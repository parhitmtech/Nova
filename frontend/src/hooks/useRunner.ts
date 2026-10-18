import { useEffect, useRef, useState } from 'react'
import { runCodeStream, compareCode } from '../api/compiler'
import { errorMessage } from '../lib/workspace'
import type { OutputState } from '../components/BottomPanel'
import type { DocumentState } from './useWorkspace'

// Running the current program (streamed output, stoppable) and the optimization comparison.
export function useRunner({
  token,
  inputs,
  document,
  docRef,
  onFinished,
  notify,
}: {
  token: string | null
  inputs: string
  document: DocumentState
  docRef: { current: DocumentState }
  onFinished: () => void
  notify: (text: string, error?: boolean) => void
}) {
  const [output, setOutput] = useState<OutputState>({ mode: 'idle', content: '' })
  const [loading, setLoading] = useState(false)
  const [running, setRunning] = useState(false)
  const runAbort = useRef<AbortController | null>(null)

  useEffect(() => () => runAbort.current?.abort(), [])

  const run = async () => {
    if (loading || docRef.current.binary) return
    const controller = new AbortController()
    runAbort.current = controller
    setLoading(true)
    setRunning(true)
    setOutput({ mode: 'run', content: '' })
    let text = ''
    try {
      await runCodeStream(
        docRef.current.code,
        inputs,
        (type, chunk) => {
          if (controller.signal.aborted) return
          if (type === 'stdout' || type === 'stderr') {
            text += chunk
            setOutput((prev) => ({ ...prev, mode: prev.mode === 'error' ? 'error' : 'run', content: text }))
          } else if (type === 'error') {
            text += chunk
            setOutput({ mode: 'error', content: text })
          } else if (type === 'done') {
            setOutput((prev) => ({ ...prev, ms: Number(chunk) }))
            onFinished()
          }
        },
        controller.signal,
        token ?? undefined,
      )
    } catch (error) {
      if (!controller.signal.aborted)
        setOutput({ mode: 'error', content: errorMessage(error, 'Execution failed') })
    } finally {
      if (runAbort.current === controller) {
        setLoading(false)
        setRunning(false)
        runAbort.current = null
      }
    }
  }

  const stop = () => {
    runAbort.current?.abort()
    notify('Execution stopped')
    setOutput((prev) => ({ ...prev, content: `${prev.content}\n[Execution stopped]` }))
  }

  const compare = async () => {
    if (loading || document.binary) return
    setLoading(true)
    setOutput({ mode: 'idle', content: '' })
    try {
      const res = await compareCode(document.code, inputs, token ?? undefined)
      setOutput(
        res.data.error
          ? { mode: 'error', content: res.data.error }
          : { mode: 'compare', content: '', compareData: res.data },
      )
    } catch (error) {
      setOutput({ mode: 'error', content: errorMessage(error, 'Comparison failed') })
    } finally {
      setLoading(false)
    }
  }

  return {
    output,
    clearOutput: () => setOutput({ mode: 'idle', content: '' }),
    loading,
    running,
    run,
    stop,
    compare,
  }
}
