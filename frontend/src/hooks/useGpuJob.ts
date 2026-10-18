import { useEffect, useRef, useState } from 'react'
import { submitGpuJob, getGpuStatus, getGpuResults } from '../api/compiler'
import { errorMessage } from '../lib/workspace'

export type GpuStatus = 'idle' | 'submitting' | 'pending' | 'running' | 'completed' | 'failed' | 'unavailable'

// Submit the program as a GPU job and poll it every 5 s until it completes or fails.
export function useGpuJob(token: string | null, onCompleted: () => void) {
  const [status, setStatus] = useState<GpuStatus>('idle')
  const [output, setOutput] = useState('')
  const [error, setError] = useState('')
  const [scenario, setScenario] = useState('')
  const [benefit, setBenefit] = useState('')
  const poll = useRef<ReturnType<typeof setInterval> | null>(null)

  useEffect(
    () => () => {
      if (poll.current) clearInterval(poll.current)
    },
    [],
  )

  const submit = async (code: string) => {
    if (!token) return
    if (poll.current) clearInterval(poll.current)
    setStatus('submitting')
    setError('')
    setOutput('')
    try {
      const res = await submitGpuJob(token, code)
      setStatus(res.data.status ?? 'pending')
      setScenario(res.data.scenario ?? '')
      setBenefit(res.data.gpuBenefit ?? '')
      poll.current = setInterval(async () => {
        try {
          const response = await getGpuStatus(token, res.data.jobId)
          setStatus(response.data.status)
          if (response.data.status === 'completed' || response.data.status === 'failed') {
            if (poll.current) clearInterval(poll.current)
            poll.current = null
            if (response.data.status === 'completed') {
              const result = await getGpuResults(token, res.data.jobId)
              setOutput(result.data.output ?? '')
              onCompleted()
            } else setError(response.data.error ?? 'GPU job failed')
          }
        } catch (err) {
          if (poll.current) clearInterval(poll.current)
          poll.current = null
          setStatus('failed')
          setError(errorMessage(err, 'Could not check job status'))
        }
      }, 5000)
    } catch (err) {
      setStatus('failed')
      setError(errorMessage(err, 'GPU submission failed'))
    }
  }

  const busy = ['submitting', 'pending', 'running'].includes(status)
  return { status, output, error, scenario, benefit, busy, submit }
}
