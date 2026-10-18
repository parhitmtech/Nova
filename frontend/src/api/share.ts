import { http as axios, API, authHeaders } from './client'

// Save a code snippet to S3 under a server-generated UUID.
// Returns the UUID so the caller can construct a shareable /share/:id URL.
// Creating shares requires authentication; viewing a shared snippet is public.
export const shareCode = (code: string, token?: string) =>
  axios.post(`${API}/api/share`, { code }, { headers: authHeaders(token) })

// Fetch a previously shared snippet by its UUID.
// Used by SharePage to populate the read-only editor.
export const getShare = (id: string) => axios.get(`${API}/api/share/${id}`)
