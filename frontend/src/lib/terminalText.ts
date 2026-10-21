// Render program output the way a terminal shows carriage returns: on each line, text after the
// last '\r' replaces what came before it. Programs use this to redraw a line in place, e.g. a
// training progress bar printing "\r[███░░] 40%" repeatedly.
//
// "\r\n" is an ordinary newline (Windows). A line that currently ends in '\r' keeps its text: the
// '\n' completing "\r\n" may simply not have arrived yet in a stream.
export function applyCarriageReturns(text: string): string {
  if (!text.includes('\r')) return text
  return text
    .replace(/\r\n/g, '\n')
    .split('\n')
    .map((line) => {
      const body = line.endsWith('\r') ? line.slice(0, -1) : line
      return body.slice(body.lastIndexOf('\r') + 1)
    })
    .join('\n')
}
