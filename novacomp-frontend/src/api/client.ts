import axios from 'axios'

export const API = (import.meta.env.VITE_API_URL || '').replace(/\/$/, '')
export const http = axios.create()
export const authHeaders = (token?: string): Record<string, string> => {
  const current = token ?? localStorage.getItem('novacomp_token')
  return current ? { Authorization: `Bearer ${current}` } : {}
}

export function sessionExpired() {
  window.dispatchEvent(new Event('novacomp:session-expired'))
}

http.interceptors.request.use(config => {
  const token = localStorage.getItem('novacomp_token')
  if (token && !config.headers.Authorization) config.headers.Authorization = `Bearer ${token}`
  return config
})
http.interceptors.response.use(response => response, error => {
  if (error.response?.status === 401 && localStorage.getItem('novacomp_token')) sessionExpired()
  return Promise.reject(error)
})
