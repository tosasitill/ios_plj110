# Source attribution

Imported from `UniXNU/UniXNU`, commit
`0e2fb2bcdf11b104d28b0d4aae4f77a7945e3de5`, `scripts/appsync/`,
on 2026-11-12. Reference board: `UniXNU/UniXNU-sheng`, commit
`e8937177309d88c93b305a57bad147fd5fac8a86`.

The Objective-C hooks follow AppSync Unified's installation / FrontBoard /
LSApplicationWorkspace separation. Original AppSync authors: Karen/あけみ
and Linus Yang; upstream AppSync commit
`235aca6cddfbdc9fa87fcb5b2aec2df37ed6d65a`. These files are
GPL-3.0-or-later; see `LICENSE`. They are separately linked user-space
components, not relicensed under the repository's MIT license.

X9 changes: arm64 iOS16 target, private runtime prefix and private build cache
in `signer.py`. `Signing.m` receives its signer path from the build command.
No sheng Apple executables, binary patches, board firmware or test fixtures
are imported. The source pins and archive SHA256 values of Procursus ldid,
OpenSSL and libplist remain in `signer-sources.json`; dependency licenses and
relinkable objects are retained by the signer builder.
