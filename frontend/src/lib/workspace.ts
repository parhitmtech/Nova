export const EXAMPLES = [
  {
    name: 'hello.nova',
    title: 'Hello, Nova',
    description: 'Your first program',
    code: 'ignite:\n    print("Hello, Nova!")\n',
  },
  {
    name: 'functions.nova',
    title: 'Functions',
    description: 'Types, parameters, and return values',
    code: 'def add(int a, int b) -> int:\n    return a + b\n\nignite:\n    print(add(3, 4))\n',
  },
  {
    name: 'loops.nova',
    title: 'Loops',
    description: 'Add the numbers from 1 to 5',
    code: 'ignite:\n    int sum = 0\n    int i = 0\n    for (i = 1 ; i < 6 ; i += 1):\n        sum += i\n    print(sum)\n',
  },
  {
    name: 'input.nova',
    title: 'Standard input',
    description: 'Try input values 3 and 4',
    code: 'ignite:\n    int a = input()\n    int b = input()\n    print(a + b)\n    print(a * b)\n',
  },
]
export const isBinaryFile = (name: string) => /\.(tar\.gz|gz|bin|pt|pth|nbc|zip)$/i.test(name)
export const validFileName = (name: string) =>
  /^[A-Za-z0-9][A-Za-z0-9_.-]{0,127}$/.test(name) && !name.includes('..')
export function errorMessage(error: unknown, fallback: string) {
  return (
    (error as { response?: { data?: { error?: string } } })?.response?.data?.error ??
    (error instanceof Error ? error.message : fallback)
  )
}
export function downloadBlob(blob: Blob, name: string) {
  const url = URL.createObjectURL(blob)
  const link = document.createElement('a')
  link.href = url
  link.download = name
  link.click()
  setTimeout(() => URL.revokeObjectURL(url), 1000)
}
