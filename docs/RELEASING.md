# Releasing the VS Code extensions

Nova ships in VS Code as two extensions from one publisher:

| Extension | ID | Repository | Folder |
|---|---|---|---|
| **Nova Programming Language** | `parhitmathur.nova-programming-language` | [Nova](https://github.com/parhitmtech/Nova) | `editors/vscode/nova` |
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

Each platform has its own package, with Nova's tools inside it (`bin/nova`, `bin/nova-lsp`, `stdlib/`):

```bash
make -C compiler CXXFLAGS="-O2 -static-libstdc++ -static-libgcc"        # Windows (MSYS2 UCRT64): CXXFLAGS="-O2 -static"
make -C compiler lsp CXXFLAGS="-O2 -static-libstdc++ -static-libgcc"
cd editors/vscode/nova
npm ci
node scripts/bundle.js linux-x64                                       # or win32-x64
npx vsce package --target linux-x64 --no-dependencies -o nova-linux-x64.vsix
code --install-extension nova-linux-x64.vsix                           # try it in your own VS Code
```

CI does the same on every pull request (the "VS Code package" jobs): it builds the tools on Ubuntu 22.04 (an
older glibc) and on Windows with MinGW-w64 linked statically, checks that they need no C++ runtime at run time,
runs the golden and language server tests with them, packages, unpacks the package, and runs the extension's tests
in VS Code against it. The packages are kept as the run's artifacts.

## Versions

- Semantic versioning. Until 1.0 the extensions are **pre-release** (`vsce publish --pre-release`), and the
  Marketplace offers them only to people who opt in.
- The Marketplace has no pre-release tags in versions (`1.0.0-beta` is rejected): pre-releases use **odd minor
  versions** (0.1.x, 0.3.x) and releases even ones, as Microsoft recommends.
- Every release updates `CHANGELOG.md` in the extension's folder; the Marketplace shows it.

## Releasing

A release is a tag. To release the Nova extension:

1. Set the version in `editors/vscode/nova/package.json`, and add its entry to `CHANGELOG.md` (a heading
   `## [0.3.0] ...`). An odd minor version (0.3, 0.5) is a pre-release.
2. Merge that, then tag the commit and push the tag:

   ```bash
   git tag nova-v0.3.0
   git push origin nova-v0.3.0
   ```

The workflow **Release the Nova extension** (`.github/workflows/release.yml`) then:

1. checks that the tag matches the version, and that the changelog has the version's entry;
2. builds and tests the package for each platform, exactly as CI does (`.github/workflows/package.yml`);
3. makes a GitHub release with the packages attached, its notes the version's changelog entry (the packages install
   from there too: `code --install-extension nova-linux-x64.vsix`);
4. publishes them to the VS Code Marketplace and to Open VSX (as a pre-release for an odd minor version).

Step 4 needs the secrets `VSCE_PAT` and `OVSX_PAT` (above); without them it stops and says which one to add, after
the GitHub release is made. To try a release without publishing, run the workflow by hand (Actions > Release the
Nova extension > Run workflow): it checks the version and builds and tests the packages. Ticking **publish**
publishes them as well.

Nova Quantum is released the same way from its repository, with a tag `nova-quantum-v<version>`. It depends on the
Nova extension, so release Nova first: the Marketplace refuses an extension whose dependency it does not have.
