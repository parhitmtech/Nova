import { describe, expect, it } from 'vitest'
import { applyCarriageReturns } from './terminalText'

describe('applyCarriageReturns', () => {
  it('leaves text without carriage returns untouched', () => {
    expect(applyCarriageReturns('a\nb\n')).toBe('a\nb\n')
  })

  it('keeps only the last redraw of a progress bar line', () => {
    const bar = '\r[█░░] 33% 1/3\r[██░] 66% 2/3\r[███] 100% 3/3\n'
    expect(applyCarriageReturns('Training\n' + bar + 'done\n')).toBe('Training\n[███] 100% 3/3\ndone\n')
  })

  it('shows the latest frame of a line that is still being redrawn', () => {
    expect(applyCarriageReturns('epoch 1\n\r[█░] 50%\r[██] 75%')).toBe('epoch 1\n[██] 75%')
  })

  it('treats \\r\\n as a plain newline', () => {
    expect(applyCarriageReturns('42\r\n7\r\n')).toBe('42\n7\n')
  })

  it('keeps a line whose \\r\\n has only half arrived', () => {
    expect(applyCarriageReturns('42\r')).toBe('42')
  })
})
