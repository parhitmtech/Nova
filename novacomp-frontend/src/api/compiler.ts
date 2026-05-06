import axios from 'axios'

const API = import.meta.env.VITE_API_URL

export const runCode = (source: string, inputs: string = '') =>
  axios.post(`${API}/run`, { source, inputs })

export const compareCode = (source: string, inputs: string = '') =>
  axios.post(`${API}/compare`, { source, inputs })

export const submitGpuJob = (token: string, source: string) =>
  axios.post(`${API}/gpu/submit`, { source }, {
    headers: { Authorization: `Bearer ${token}` },
  })

export const getGpuStatus = (token: string, jobId: string) =>
  axios.get(`${API}/gpu/status/${jobId}`, {
    headers: { Authorization: `Bearer ${token}` },
  })

export const getGpuResults = (token: string, jobId: string) =>
  axios.get(`${API}/gpu/results/${jobId}`, {
    headers: { Authorization: `Bearer ${token}` },
  })
