import axios from 'axios'

const API = import.meta.env.VITE_API_URL

export const runCode = (source: string, inputs: string = '') =>
  axios.post(`${API}/run`, { source, inputs })

export const compareCode = (source: string, inputs: string = '') =>
  axios.post(`${API}/compare`, { source, inputs })
