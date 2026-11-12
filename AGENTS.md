# Working on ios-sweet

- Read `NOTES.md` before starting; it contains the project goal, hardware facts,
  current blocker, local artifact locations and the next concrete action.
- The user switches agents frequently. Update the active handoff in `NOTES.md`
  after each milestone and before stopping. Record commits, CI run IDs, exact
  build/boot results and unfinished work. Distinguish observations from guesses.
- Commit completed changes. Use `gh` for GitHub Actions kernel builds and logs.
  Local single-file cross-compiles are useful before spending a CI run.
- Device bring-up uses temporary `fastboot boot` images. Preserve the Android
  installation and partition backups. The user approved (2026-10-15) using
  internal UFS for iOS and said Android may be deleted: the iOS root goes
  into the existing `userdata` partition (no GPT changes). Keep bootloaders,
  recovery, modem, persist and the backups intact, and confirm each
  destructive step with the user before running it.
- Keep device dumps, serial numbers and downloaded Apple artifacts out of git.
- Repository layout and workflow (2026-11-12, user rule):
  - Builds depend only on this repository plus compile tools (Xcode,
    KDK_13.5_22G74, Homebrew LLVM/lld/ccache/python). Private inputs live in
    the fixed `.local/` layout listed in `NOTES.md` §2.2 and checked by
    `x9/build/prepare-local.py --check`; never point builds at `~/HelioDarwin`
    or other paths outside the repo.
  - XNU source changes are the patches in `x9/xnu/`; rebuild the object tree
    with `x9/xnu/build-xnu.sh` (NOTES §3.1).
  - Each feature or experiment gets a git branch (`feat/<topic>`, `fix/<topic>`);
    parallel work uses `git worktree add .worktrees/<topic>` with a `.local`
    symlink to the main checkout. Do not create new experiment or analysis
    folders under `.local/`; scratch output goes in the worktree's gitignored
    `x9/out/`. Merge into main when done, then remove the worktree and branch.
  - Push completed work to `origin` (tosasitill/ios_plj110).
