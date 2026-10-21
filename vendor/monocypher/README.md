# Monocypher

Pinned Monocypher 4.0.3 provides Linux SecureString idle authenticated encryption
(XChaCha20-Poly1305). Source and header are unmodified official release files.
The release changes the Git source version comment from `__git__` to `4.0.3`;
the cryptographic implementation is identical to the recorded Git commit.

`manifest.json` records the immutable release archive and file hashes. CMake checks
these bytes without network access. Only Linux native builds compile this source;
Windows uses its system CryptProtectMemory backend and WASM has no native backend.
Private per-source compiler definitions prefix upstream external function symbols
with `portals_monocypher_`; installed applications acquire no separate crypto library
or headers. The source bundle includes these files for offline builds.

The upstream BSD-2-Clause/CC0 dual license is retained in `LICENCE.md`; Portals
redistributes it under BSD-2-Clause, including notices in installed/bundled licenses.
Upstream integration: https://monocypher.org/manual/aead
Upstream release: https://github.com/LoupVaillant/Monocypher/releases/tag/4.0.3

The published 2020 Cure53 audit predates this release; it is not a certification
of this version or of Portals' storage integration. Encryption does not prevent
a hostile process-wide reader from obtaining an in-process key or live plaintext.
