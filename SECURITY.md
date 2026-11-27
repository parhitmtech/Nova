# Security policy

## Supported versions

| Component | Supported |
|---|---|
| Nova Compiler extension for VS Code | the latest release |
| The Nova compiler and interpreter (`main`) | yes |
| The NovaComp cloud IDE (`main`) | yes |

## Reporting a vulnerability

Please report security issues **privately**, through GitHub's
[private vulnerability reporting](https://github.com/parhitmtech/Nova/security/advisories/new). Do not open a public
issue.

Include what you can of:

- the component and version (extension version, or commit);
- the steps or the Nova program that reproduces it;
- what an attacker gains (code execution, escaping the IDE's sandbox, reading another user's files, …).

You will get an acknowledgement within 7 days and an assessment within 30. Fixes are released as soon as they are
ready, and the advisory credits the reporter unless you ask otherwise.

## In scope

- **The VS Code extension** runs Nova's compiler on the files you open. Opening a file or a folder must never run
  code you did not ask to run; in an untrusted workspace (VS Code's Workspace Trust), the extension runs nothing.
- **The compiler** must not crash or corrupt memory on any input, however malformed: a crash on bad input is a bug,
  and one that can be exploited is a security issue.
- **The cloud IDE** runs each program in a sandboxed container with no network and a read-only file system;
  escaping it, or reaching another user's data, is in scope.

## Out of scope

- A Nova program that you run yourself doing what it was written to do (reading your files, using the CPU).
