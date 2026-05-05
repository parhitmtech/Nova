import axios from 'axios'

const API = import.meta.env.VITE_API_URL

const authHeaders = (token: string) => ({
  headers: { Authorization: `Bearer ${token}` },
})

export const listFiles = (token: string) =>
  axios.get(`${API}/files`, authHeaders(token))

export const saveFile = (token: string, fileName: string, content: string) =>
  axios.post(`${API}/files/save`, { fileName, content }, authHeaders(token))

export const loadFile = (token: string, fileName: string) =>
  axios.get(`${API}/files/load?fileName=${encodeURIComponent(fileName)}`, authHeaders(token))

export const deleteFile = (token: string, fileName: string) =>
  axios.delete(`${API}/files/delete?fileName=${encodeURIComponent(fileName)}`, authHeaders(token))
