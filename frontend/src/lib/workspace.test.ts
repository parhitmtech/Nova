import { describe, expect, it } from 'vitest'
import { errorMessage, isBinaryFile, validFileName } from './workspace'

describe('validFileName', () => {
  it.each(['main.nova', 'my_file-2.nova', 'a', 'data.csv'])('accepts %s', (name) => {
    expect(validFileName(name)).toBe(true)
  })

  it.each(['', '.hidden', '../x.nova', 'a/b.nova', 'a b.nova', 'x..nova', 'a'.repeat(129)])(
    'rejects %j',
    (name) => {
      expect(validFileName(name)).toBe(false)
    },
  )
})

describe('isBinaryFile', () => {
  it.each(['model.pt', 'weights.pth', 'program.nbc', 'out.tar.gz', 'X.ZIP'])(
    'treats %s as binary',
    (name) => {
      expect(isBinaryFile(name)).toBe(true)
    },
  )

  it.each(['main.nova', 'notes.txt', 'nbc.nova'])('treats %s as source', (name) => {
    expect(isBinaryFile(name)).toBe(false)
  })
})

describe('errorMessage', () => {
  it('prefers the server error from an axios-style response', () => {
    expect(errorMessage({ response: { data: { error: 'Too many requests' } } }, 'fallback')).toBe(
      'Too many requests',
    )
  })

  it('uses an Error message next', () => {
    expect(errorMessage(new Error('Network down'), 'fallback')).toBe('Network down')
  })

  it('falls back for anything else', () => {
    expect(errorMessage('boom', 'fallback')).toBe('fallback')
    expect(errorMessage(undefined, 'fallback')).toBe('fallback')
  })
})
