# Building Voltura Books

## Requirements

- Windows x64.
- Visual Studio 2022 or 2026 with **Desktop development with C++** and CMake tools.
- NSIS 3 on PATH for the installer.
- Python 3, Pillow, and OpenSSL for integration tests. Git for Windows includes OpenSSL.
- PowerShell 7 and GitHub CLI for releases.

## Build and package

```powershell
./scripts/build.ps1
./scripts/build-installer.ps1 -SkipBuild
```

The build runs the C++ tests and creates `dist/`. CMake downloads checksum-pinned libcurl and miniz source archives. The app uses C++20 and Win32; TLS uses Windows Schannel. The installer is `dist/VolturaBooks-Setup.exe`. It registers the Explorer action and Start menu shortcuts for the current user, plus removal through Windows Installed apps. Builds do not install automatically. An unpacked package can be installed with `install.ps1`.

Settings and credentials are not packaged. Windows executables are unsigned. CMake generates THIRD-PARTY-NOTICES.txt from the statically linked dependency licenses and includes it in the package.

## Tests

```powershell
python tests/smtp_fixture.py
python tests/cover_fixture.py
```

The C++ suite covers folder browsing, sequential queues, input layout and validation, file drops, mail setup, history, and Explorer COM selection. Run in an interactive Windows user session: COM registration and activation must use the same user context. The selection test uses a temporary test-only CLSID and 20 Unicode paths.

The SMTP fixture uses loopback servers and temporary certificates. It checks attachment integrity, authenticated and direct sending, TLS rejection, server rejection, and uncertain submission. No email leaves the computer. The cover fixture checks EPUB and DOCX metadata, PDF rendering, images, and malformed-document fallbacks. Queue tests use simulated mail and history.

`python tests/windows_smoke.py` requires a **clean Windows test profile**. It refuses an existing installation, settings, or credentials and creates and removes test data. Do not run it on a configured profile.

## Website

```powershell
python -m http.server 8080 --directory docs
```

Open http://localhost:8080. The site is plain HTML and CSS without external fonts or JavaScript dependencies. GitHub Pages uses the manual **Deploy website** workflow. The build workflow is also manual; pushing does not run either workflow.

## Commands

```text
VolturaBooks.exe --send "C:\Books\My book.epub" "C:\Books\Another book.epub"
VolturaBooks.exe --drop
VolturaBooks.exe --settings
VolturaBooks.exe --install
VolturaBooks.exe --uninstall
```

No arguments opens the book window. Explorer passes selections to an out-of-process COM DropTarget through the internal `--shell` entry point, avoiding command-line length limits and a separate process per book.

## Release

```powershell
pwsh ./scripts/release.ps1 -PrepareOnly
pwsh ./scripts/release.ps1
```

Prepare-only builds, runs all C++ and SMTP/cover checks, packages the installer and ZIP, signs the update manifest, verifies it with the native updater, and writes SHA-256 checksums under `dist/releases/<version>/`. It does not publish or need Git history.

Publication requires a clean committed `main`, origin pointing to public `voltura/voltura-books`, and authenticated GitHub CLI. It pushes the exact commit, creates a draft with `docs/releases/<version>.md`, downloads and verifies every asset, and then publishes as Latest. It never stages or commits automatically, replaces a release, or overwrites a tag. If publication fails after creating the draft, inspect the draft before retrying.

The version defaults to CMakeLists.txt. Update CMakeLists.txt, src/app.rc, src/app.manifest, and src/integration.cpp together for a new version. The installer and About window derive their versions from CMake. Write the versioned release notes before preparation. `-Version` optionally asserts the checked-in version. Preparation refuses an existing output directory; move previous output aside before preparing again.

For first publication, review the source and ignored files, create an initial Git commit on main, and create the public repository with `gh repo create voltura/voltura-books --public --source . --remote origin`. Run release.ps1 when ready to push and publish. Enable Pages with GitHub Actions and run Deploy website after the first push.

## Update signing

Set `BOOKS_KEYPATH` to the encrypted private PEM corresponding to `src/update-signing-public.pem`, copied from Voltura Earner. Set `BOOKS_UPDATE_SIGNING_PASSPHRASE` in the release process to unlock it. If that variable is absent, the signer supports the Windows-protected `.passphrase.xml` sidecar used by the sister apps. Never commit private keys or passphrases.

The app verifies RSA-PSS/SHA-256 signed manifests, version, exact installer name, size, and SHA-256 before offering installation. This update signature is separate from Windows Authenticode signing; the executable remains unsigned.

About's Check for updates action contacts GitHub and downloads a newer stable release. Install update opens the normal setup wizard and closes the book window. Checks are user-initiated; there is no background update service. Downloads are stored in local app data under Voltura Books/Updates. Before the first public release, the check reports that no published release is available.

## Dependencies

libcurl and miniz are statically linked. The app uses Windows Imaging Component, XmlLite, Credential Manager, DNS, WinHTTP and Windows cryptography APIs. Dependabot maintains existing GitHub Action references; checksum-pinned CMake source dependencies are updated manually.
