import axios from 'axios'

const API = import.meta.env.VITE_API_URL

// Save a code snippet to S3 under a server-generated UUID.
// Returns the UUID so the caller can construct a shareable /share/:id URL.
// No auth required — anyone can share a snippet.
export const shareCode = (code: string) =>
  axios.post(`${API}/api/share`, { code })

// Fetch a previously shared snippet by its UUID.
// Used by SharePage to populate the read-only editor.
export const getShare = (id: string) =>
  axios.get(`${API}/api/share/${id}`)
