import axios from 'axios'

// Base URL is injected by Vite at build time from the .env file (VITE_API_URL).
// Changing .env requires a rebuild — it is not a runtime variable.
const API = import.meta.env.VITE_API_URL

// Register a new account. Server hashes the password with bcrypt before storing.
export const signup = (email: string, password: string) =>
  axios.post(`${API}/auth/signup`, { email, password })

// Authenticate with existing credentials. Server returns a signed JWT on success.
export const login = (email: string, password: string) =>
  axios.post(`${API}/auth/login`, { email, password })

// Fetch the current user's profile. Useful for validating a stored token is still active.
export const getMe = (token: string) =>
  axios.get(`${API}/auth/me`, {
    headers: { Authorization: `Bearer ${token}` },
  })
