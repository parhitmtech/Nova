import axios from 'axios'

const API = import.meta.env.VITE_API_URL

// Attach the JWT on every file request so the server can scope S3 operations
// to the correct user prefix ({userId}/{fileName}).
const authHeaders = (token: string) => ({
  headers: { Authorization: `Bearer ${token}` },
})

// Returns the list of files registered in DynamoDB for this user.
// DynamoDB holds metadata only (fileName, updatedAt); actual content lives in S3.
export const listFiles = (token: string) =>
  axios.get(`${API}/files`, authHeaders(token))

// Upload file content to S3 and register it in DynamoDB.
// Overwrites silently if a file with the same name already exists.
export const saveFile = (token: string, fileName: string, content: string) =>
  axios.post(`${API}/files/save`, { fileName, content }, authHeaders(token))

// Fetch file content from S3. Returns null content for binary files
// (e.g. .tar.gz model archives) — callers must guard against null before
// setting editor state to avoid wiping the editor with empty content.
export const loadFile = (token: string, fileName: string) =>
  axios.get(`${API}/files/load?fileName=${encodeURIComponent(fileName)}`, authHeaders(token))

// Remove the file from both S3 and DynamoDB.
export const deleteFile = (token: string, fileName: string) =>
  axios.delete(`${API}/files/delete?fileName=${encodeURIComponent(fileName)}`, authHeaders(token))
