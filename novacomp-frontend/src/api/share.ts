import axios from 'axios'

const API = import.meta.env.VITE_API_URL

export const shareCode = (code: string) =>
  axios.post(`${API}/api/share`, { code })

export const getShare = (id: string) =>
  axios.get(`${API}/api/share/${id}`)
