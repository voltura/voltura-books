# Security Policy

## Supported versions

Security reports should target the latest public Voltura Books release or the current default branch (`master`). Older versions may receive fixes when the issue is severe and a safe patch is practical.

## Reporting a vulnerability

Do not publish exploit details in public issues.

1. Use [GitHub private vulnerability reporting](https://github.com/voltura/voltura-Books/security/advisories/new) if enabled.
2. Otherwise, contact Voltura AB through an available private channel.
3. If no private channel is available, open a minimal issue asking for maintainer contact, without exploit details or sensitive attachments.

When reporting privately, include the affected version or commit, Windows version, installation type, reproduction steps, expected impact, and whether local access or a modified file is required. Remove unrelated personal information from logs and screenshots.

## Security boundaries

Voltura Books runs with the signed-in Windows user's permissions. Calendar calculations happen locally. Settings imports are bounded and validated before being saved. Settings and diagnostic files are not a security boundary against other software running as the same user.

Installed builds fetch update information from GitHub over HTTPS. The updater checks an RSA-PSS signed manifest against an embedded public key, verifies the installer's signed metadata and SHA-256, and checks it again before launch. Downloads and redirects are restricted to allowed origins. The user chooses when to install a ready update. Portable copies use manual updates.

Installation is per user. The standard installer verifies Microsoft's runtime installer before requesting elevation for a missing runtime. Maintenance validates payload files and installation ownership and rejects reparse points. Update manifest signing is separate from Windows Authenticode publisher signing.

Private release signing keys, passphrases, and credentials must not be committed or included in reports. For data storage and network behavior, see [Privacy](PRIVACY.md).
