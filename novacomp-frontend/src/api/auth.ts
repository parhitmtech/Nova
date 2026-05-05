import axios from 'axios'

const API = import.meta.env.VITE_API_URL

export const signup = (email: string, password: string) =>
  axios.post(`${API}/auth/signup`, { email, password })

export const login = (email: string, password: string) =>
  axios.post(`${API}/auth/login`, { email, password })

export const getMe = (token: string) =>
  axios.get(`${API}/auth/me`, {
    headers: { Authorization: `Bearer ${token}` },
  })
