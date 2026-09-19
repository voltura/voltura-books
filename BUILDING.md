# Building Voltura Books

## Requirements

- Windows x64.
- Visual Studio 2022 or 2026 with **Desktop development with C++** and CMake tools.
- NSIS 3 on PATH for the installer.
- Node.js for private reader bridge tests (no browser or Node runtime is shipped).
- Python 3, Pillow, and OpenSSL for integration tests. Git for Windows includes OpenSSL.
- PowerShell 7 and GitHub CLI for releases.

## Build and package

```powershell
./scripts/build.ps1
./scripts/build-installer.ps1 -SkipBuild
```

The default build compiles and creates `dist/`; it does not run tests. CMake downloads checksum-pinned libcurl and miniz source archives. The app uses C++20 and Win32; TLS uses Windows Schannel. The installer is `dist/VolturaBooks-Setup.exe`. It registers the Explorer action and Start menu shortcuts for the current user, plus removal through Windows Installed apps. Builds do not install automatically. An unpacked package can be installed with `install.ps1`.

Settings and credentials are not packaged. Windows executables are unsigned. CMake generates THIRD-PARTY-NOTICES.txt from the statically linked dependency licenses and includes it in the package.

## Tests

```powershell
python tests/smtp_fixture.py
python tests/cover_fixture.py
# Only the affected non-UI tests:
./scripts/build.ps1 -Test docx_text
# Explicit visible validation, when the desktop is available:
./scripts/test-ui.ps1 -Test reader -ReaderFormat docx
```

Run only checks relevant to the change. `-Test` accepts named non-UI checks, not
an implicit full suite. Available names are `core`, `drop`, `mail_setup`,
`settings_cleanup`, `history`, `rtf_fit`, `docx_text`, `doc_conversion`,
`doc_format`, `reader_loading`, `reader_bridge`, `reader_native_loading`, and `instance_forwarding`; pass only relevant names
(for example, `-Test core,drop`). `-SkipTests` remains accepted for compatibility.
To test an already-built DOCX extractor without packaging, run
`python tests/docx_text_fixture.py`. The manual GitHub build workflow explicitly
runs the non-UI suite and SMTP/cover fixtures; it never enables desktop UI tests.

`instance_forwarding` checks both sending modes through the production broker and
launcher handlers using a hidden launcher and a browser test adapter. It also
checks rejection of conflicting active modes and malformed requests without
opening the browser, sending mail, or changing desktop focus.

Visible tests are labelled `interactive` and skip even
under direct CTest/executable invocation without a process-local opt-in. Use
`test-ui.ps1 -Test browser`, `queue`, `edit_layout`, or `shell_selection` for those
specific checks. Reader tests additionally require `-ReaderFormat doc`, `docx`, or `all`;
use `all` only when validating the entire reader. These tests open windows, change
focus, and can enter fullscreen: do not run them on an occupied desktop.

The C++ suite covers folder browsing, sequential queues, input layout and validation, file drops, mail setup, history, and Explorer COM selection. Run the COM selection test in the signed-in interactive Windows session: registration and activation must use the same user context. The selection test uses a temporary test-only CLSID and 20 Unicode paths.

The SMTP fixture uses loopback servers and temporary certificates. It checks attachment integrity, authenticated and direct sending, TLS rejection, server rejection, and uncertain submission. No email leaves the computer. The cover fixture checks EPUB and DOCX metadata, PDF rendering, images, and malformed-document fallbacks. Queue tests use simulated mail and history.

`python tests/windows_smoke.py --interactive` requires a **clean Windows test profile**. It refuses an existing installation, settings, or credentials and creates and removes test data. Do not run it on a configured profile.

## Website

```powershell
python -m http.server 8080 --directory docs
```

Open http://localhost:8080. The site is plain HTML and CSS without external fonts or JavaScript dependencies. GitHub Pages uses the manual **Deploy website** workflow. The build workflow is also manual; pushing does not run either workflow.

## Commands

```text
VolturaBooks.exe --send "C:\Books\My book.epub" "C:\Books\Another book.epub"
VolturaBooks.exe --drop
VolturaBooks.exe --browse
VolturaBooks.exe --browse --test-sending
VolturaBooks.exe --test-sending
VolturaBooks.exe --settings
VolturaBooks.exe --install
VolturaBooks.exe --uninstall
```

No arguments opens the book window. Explorer passes selections to an out-of-process COM DropTarget through the internal `--shell` entry point, avoiding command-line length limits and a separate process per book.

## Release

```powershell
pwsh ./scripts/release.ps1 -PrepareOnly -InteractiveTests
pwsh ./scripts/release.ps1 -InteractiveTests
```

Release preparation requires `-InteractiveTests` to explicitly opt into its full
release validation on an available desktop. Prepare-only builds, runs all C++ and SMTP/cover checks, packages the installer and ZIP, signs the update manifest, verifies it with the native updater, and writes SHA-256 checksums under `dist/releases/<version>/`. It does not publish or need Git history.

Publication requires a clean committed `main`, origin pointing to public `voltura/voltura-books`, and authenticated GitHub CLI. It pushes the exact commit, creates a draft with `docs/releases/<version>.md`, downloads and verifies every asset, and then publishes as Latest. It never stages or commits automatically, replaces a release, or overwrites a tag. If publication fails after creating the draft, inspect the draft before retrying.

The version defaults to CMakeLists.txt. Update CMakeLists.txt, src/app.rc, src/app.manifest, and src/integration.cpp together for a new version. The installer and About window derive their versions from CMake. Write the versioned release notes before preparation. `-Version` optionally asserts the checked-in version. Preparation refuses an existing output directory; move previous output aside before preparing again.

For first publication, review the source and ignored files, create an initial Git commit on main, and create the public repository with `gh repo create voltura/voltura-books --public --source . --remote origin`. Run release.ps1 when ready to push and publish. Enable Pages with GitHub Actions and run Deploy website after the first push.

## Update signing

Set `BOOKS_KEYPATH` to the encrypted private PEM corresponding to `src/update-signing-public.pem`, copied from Voltura Earner. Set `BOOKS_UPDATE_SIGNING_PASSPHRASE` in the release process to unlock it. If that variable is absent, the signer supports the Windows-protected `.passphrase.xml` sidecar used by the sister apps. Never commit private keys or passphrases.

The app verifies RSA-PSS/SHA-256 signed manifests, version, exact installer name, size, and SHA-256 before offering installation. This update signature is separate from Windows Authenticode signing; the executable remains unsigned.

About's Check for updates action contacts GitHub and downloads a newer stable release. Install update opens the normal setup wizard and closes the book window. Checks are user-initiated; there is no background update service. Downloads are stored in local app data under Voltura Books/Updates. Before the first public release, the check reports that no published release is available.

## Dependencies

libcurl and miniz are statically linked. The app uses Windows Imaging Component, XmlLite, Credential Manager, DNS, WinHTTP and Windows cryptography APIs. Dependabot maintains existing GitHub Action references; checksum-pinned CMake source dependencies are updated manually.

The reader adds checksum-pinned WebView2 SDK 1.0.4191.47 (static loader), EPUB.js
0.3.93, JSZip 3.10.1, and docx-preview 0.4.0 (Apache-2.0). CMake embeds the reader HTML and JavaScript in each native
executable; no Node.js, local web server or loose reader assets are needed at runtime.
The shared Evergreen WebView2 Runtime is required for EPUB, HTML, and DOCX reading. Reader
cache data lives under local app data in `Voltura Books/Reader` and is removed on
uninstall. The visible reader fixture requires this runtime and Pillow and exercises the
production reader controls without sending email. DOCX text checks do not need
WebView2, Pillow, or a visible window.

### Reader resource handling

`VolturaBooksReader.exe` is the private renderer helper and must ship alongside
the main executable. It starts suspended and joins a kill-on-close Windows job
before it can parse input or start child processes. It is a windowless GUI
executable and does not create a console host.

For RTF, it uses Windows Rich Edit at below-normal priority and returns only the
current viewport bitmap through private pipes. Only static pictures are accepted
as embedded objects. RTF pages fit the viewport using the document paper size
and the actual display DC resolution; picture scaling is normalized before loading,
and downscaled images use halftone rendering. PNG/JPEG RTF pictures are converted to DIB inside this helper
because system Rich Edit drops them otherwise; decoded pictures are scaled to at
most 4096 pixels per side. The intermediate stream is disk-backed and delete-on-close.

For HTML, the contained helper starts a private WebView2 browser with a unique
profile. The app connects its visible controller to that same browser. This puts
the browser and its child processes inside the job from startup, including those
whose sandbox prevents assignment to another job once running. The app verifies
containment before navigating to document content.

The job memory budget is one eighth of available physical memory or commit
headroom, whichever is smaller, capped at 1 GiB. This is a renderer allocation
budget, not a source-file size cutoff. Active readers also check system memory
pressure. A failed or over-budget formatted reader offers text-only reading or
the default application; this does not guarantee protection against every
possible system-wide resource exhaustion condition.

TXT uses encoding-aware 32 KiB source windows. Text-only RTF/HTML extraction is
cancellable, uses bounded parser buffers, and writes a temporary UTF-16 file rather
than building a complete in-memory document. Temporary text is removed when its
reader session ends. Existing EPUB archive and image decode safeguards remain.

DOCX uses the same contained WebView2 process and resource budget as HTML. Its
renderer is embedded in the executable; Word and LibreOffice are not required.
DOCX is served through a private endpoint only for the selected document. Rendered
content lives in a script-disabled frame, with external resources, links, objects,
and alternative HTML chunks blocked. Navigation moves by viewport rather than
reproducing Word pagination. DOCX text fallback streams the main document XML
through temporary disk storage with a 128 MiB extracted XML/output limit, a
30-second deadline, bounded XML depth, and cancellation checks. It excludes
headers, footers, and notes. Temporary files are removed when no longer needed.
The reader fixture generates DOCX packages without Office and covers formatting,
images, tables, notes, navigation, fallback, invalid archives, and cancellation.

### DOC conversion and loading feedback

The C++ app ships `VolturaBooksDoc.exe`, a windowless x64 .NET Framework 4.8
helper compiled with the Windows Framework C# compiler. Checksum-pinned NuGet
packages provide DocSharp.Binary.Doc 0.21.0, DocSharp.Binary.Common 0.21.0 and
System.IO.Compression 4.3.0. These three DLLs must accompany the helper in installed
and portable packages; their licenses are included in THIRD-PARTY-NOTICES.txt.
No Office installation or bundled modern .NET runtime is required.

Conversion starts only when reading begins. The helper joins the existing
memory-limited, kill-on-close job before resuming, and emits DOCX bytes over a
private pipe. Both helper and parent enforce a 128 MiB output limit. The parent
cancels on document changes and enforces a 30-second conversion deadline. Only a
successful helper exit makes output available to the DOCX reader. The original
path remains the sending/default-app path. Converted storage is delete-on-close;
response streams and fallback workers retain ownership until they finish.
DocSharp preserves legacy EMF/WMF pictures in its DOCX output, but browsers do
not decode those formats. Before committing the conversion, the contained helper
renders those package parts to PNG with Windows System.Drawing and updates their
existing relationships and content types. Raster image defaults are added for
browser MIME detection. The helper also removes duplicate default-style markers
that would leak character and paragraph formatting across the document, keeps
legacy VML pictures in document flow, supplies visible borders when DocSharp
omits all border information for a converted table, and removes redundant
horizontal table-cell direction markers. A failed normalization rejects the
formatted conversion instead of silently presenting incomplete artwork or
formatting.

The reader owns one layered native status panel above its content and WebView2.
Opening feedback is immediate and continuous across conversion and rendering;
page/view feedback is delayed 150 ms without a minimum display time. Document,
worker, and bridge operation identities reject stale completions. The browser
acknowledges after layout, fonts, images, and paint opportunities. Ordinary
scrolling has no status operation. The progress spinner remains active for the
duration of work; high contrast uses system colors. Fullscreen cover enlargement
does not read.

Targeted non-UI checks:

```powershell
./scripts/build.ps1 -Test doc_conversion,doc_format,reader_loading,reader_bridge,reader_native_loading,docx_text
```

CMake fetches a checksum-pinned MIT-licensed DOC fixture from DocSharp for tests
only; the fixture is not installed. Tests cover formatted conversion and input
integrity, older/encrypted/malformed input, job containment, timeout, output and
memory limits, cancellation, failed helpers, delete-sharing, final-consumer
cleanup, delayed status, continuous opening and stale completion identities.
Hidden native tests exercise TXT/RTF/PDF, repeated clicks, document switching,
fullscreen state and image generations without showing or focusing a window.
They also watch the status popup for show requests throughout loading; a hidden
or minimized reader owner suppresses popup presentation. Visible spinner animation
belongs to the explicitly requested interactive reader checks.
The Node test controls promises in the production JavaScript bridge to verify
EPUB/HTML/DOCX page and layout completion, decode waits and error identities.
Visible DOC acceptance is separate and must be explicitly requested:
`./scripts/test-ui.ps1 -Test reader -ReaderFormat doc`. Review light/dark/high
contrast, reduced motion, DPI, keyboard input, windowed/fullscreen navigation,
slow files, cancellation and fullscreen exit on an available desktop.
