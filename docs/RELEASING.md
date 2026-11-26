# Releasing the VS Code extensions

Nova ships in VS Code as two extensions from one publisher:

| Extension | ID | Repository | Folder |
|---|---|---|---|
| **Nova Programming Language** | `parhitmathur.nova` | [Nova](https://github.com/parhitmtech/Nova) | `editors/vscode/nova` |
| **Nova Quantum** | `parhitmathur.nova-quantum` | [NovaQuantum](https://github.com/parhitmtech/NovaQuantum) | `editors/vscode/nova-quantum` |

Nova Quantum declares Nova as a dependency, so installing it installs both. Each is published to the
[VS Code Marketplace](https://marketplace.visualstudio.com/vscode) and to [Open VSX](https://open-vsx.org) (used by
Cursor, VSCodium and Gitpod). Version 1 targets **Linux (x64) and Windows (x64)**.

## One-time setup (the publisher)

These need the account owner; they can't be done from CI.

**VS Code Marketplace**

1. Sign in to [Azure DevOps](https://dev.azure.com) with a Microsoft account and create an organization (any name).
2. Create a Personal Access Token: *User settings → Personal access tokens → New token*. Organization: **All
   accessible organizations**; scopes: *Custom defined → Marketplace → **Manage***. Copy it.
3. Create the publisher at [marketplace.visualstudio.com/manage](https://marketplace.visualstudio.com/manage/createpublisher)
   with **ID `parhitmathur`** and **name `Parhit Mathur`**, signed in with the same account.
4. Check it: `npx @vscode/vsce login parhitmathur` (paste the token).

**Open VSX**

1. Sign in at [open-vsx.org](https://open-vsx.org) with GitHub, and sign the Eclipse Foundation Open VSX Publisher
   Agreement under *Settings*.
2. Create an access token under *Settings → Access Tokens*.
3. Create the namespace: `npx ovsx create-namespace parhitmathur -p <token>`.

**CI secrets** (in both repositories: *Settings → Secrets and variables → Actions*)

| Secret | Value |
|---|---|
| `VSCE_PAT` | the Azure DevOps token |
| `OVSX_PAT` | the Open VSX token |

Tokens expire (Azure DevOps allows at most a year): put the renewal date in a calendar.

## Building a package locally

```bash
cd editors/vscode/nova
npm ci
npm run package                                   # writes nova-<version>.vsix
code --install-extension nova-<version>.vsix      # try it in your own VS Code
```

## Versions

- Semantic versioning. Until 1.0 the extensions are **pre-release** (`vsce publish --pre-release`), and the
  Marketplace offers them only to people who opt in.
- The Marketplace has no pre-release tags in versions (`1.0.0-beta` is rejected): pre-releases use **odd minor
  versions** (0.1.x, 0.3.x) and releases even ones, as Microsoft recommends.
- Every release updates `CHANGELOG.md` in the extension's folder; the Marketplace shows it.

## Publishing (until the release workflow exists)

```bash
cd editors/vscode/nova
npx vsce publish --pre-release            # uses the token from `vsce login`
npx ovsx publish nova-<version>.vsix -p <OVSX token>
```

Phase 6 of the roadmap replaces this with a workflow: a `nova-v*` tag builds the per-platform packages (with the
compiler and language server inside), tests them, and publishes to both marketplaces.
