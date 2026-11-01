# Historical X9 helper sources

These sources were recovered from the old X9 work directories and the older
HelioDarwin tools. Maintained build entrypoints live in `../build/scripts/`.
The historical files preserve workflow and investigation logic; they are not
all interchangeable with current inputs, and were syntax-checked rather than
executed against a device.

For a Python tool under `work/`, `python3 x9/legacy/run.py <relative-script>`
rebases the old workspace roots to `.local/` and uses the corresponding local
data directory for `__file__`. Device-specific literals were replaced with
`X9_SERIAL_REQUIRED`; the runner requires `X9_SERIAL` when needed. Shell/C
sources require manual configuration before use. Helpers under `heliodarwin/`
are source references and are not supported by this runner.

Historical restore/install tools can write raw storage. Review their target,
inputs and extent checks and obtain approval for each destructive device step.
The migration did not execute them. Old root before/after pairs and runtime
source versions were pruned from the new local workspace, so old differential
restore plans cannot be replayed without reconstructing their exact baselines.
Do not substitute the retained newer root into an old restore plan.

The one retained root is pan172/edited-root.hfs, selected using original
modification times plus its successful fsck/install logs. Its copied content
was SHA256-verified against the original; the digest and cleanup inventory
are in `.local/migration-cleanup.json`. It is a historical install image, not
a snapshot of the current phone. After verification, the user authorized deleting the corresponding obsolete
images, runtime trees and build caches from the old workspace too. Source
repositories and partition backups remain. Exact deletions are recorded in
`.local/old-workspace-cleanup.json`.

Apple images, device dumps, logs and manifests remain outside git.

## Superseded user-space build steps (moved 2026-11-01)

`work/x9-metal-render/` also holds scripts moved out of `build/scripts/`
because the retained `.local` Mesa tree already contains their effect or a
newer entrypoint replaces them: the one-shot source preparation passes
(`prepare-panvk*.py`, `prepare-shared-cache.py`), the staged partial PanVK
builds (`build-panvk.py`, `build-panvk-ios.py`, `build-panvk-native*.py`
other than `build-panvk-native-ios.py`), the pan170 in-kernel probe deployment
(`build-render-deploy.py`) and the candidate-chain `prepare-pan208.py`. They
edit third-party trees in place without recording what was applied; use them
only to rebuild the Mesa tree from scratch, after `00-mesa-panvk-x9.patch`.
