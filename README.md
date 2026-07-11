# Soru Macro Engine

Native (C++) replacement for the R-pixel-watch burst macro. Runs as a
tiny hidden background process instead of inside the big AHK GUI
script — no AHK interpreter, no shared message queue, no GC.

## How the pieces fit together

- **`engine.cpp`** — the whole engine. Reads `config.ini` (written by
  the AHK GUI), watches R + a screen pixel, bursts key groups.
- **`.github/workflows/build.yml`** — on every push to `engine.cpp`,
  GitHub's own Windows runner compiles it with MSVC and commits
  `dist/engine.exe` + `dist/version.txt` back to this repo. You never
  need Visual Studio installed locally.
- **`new.ahk`** (the GUI script) — on startup, downloads
  `dist/version.txt` from this repo. If it's different from what's
  cached in `%AppData%\SoruMacro`, downloads the new `engine.exe` too.
  Then launches it hidden, passing it the path to `config.ini` and its
  own PID (so the engine can watch for the GUI closing).

## One-time setup

1. Create a **public** GitHub repo and push `engine.cpp` and
   `.github/workflows/build.yml` to it (root of the repo, or adjust
   the `paths:` filter in the workflow if you nest them).
2. Wait for the "Build macro engine" Action to finish (Actions tab) —
   it'll commit `dist/engine.exe` and `dist/version.txt` back to the
   repo automatically. Repo must be public (or you'll need a token) for
   `raw.githubusercontent.com` to serve the file without auth.
3. In `new.ahk`, set:
   ```ahk
   global RemoteBase := "https://raw.githubusercontent.com/<your-username>/<your-repo>/main/dist/"
   ```
4. Run `new.ahk`. First run downloads `engine.exe` into
   `%AppData%\SoruMacro\`; every run after that just checks
   `version.txt` (a few bytes) and only re-downloads the exe when it's
   actually changed.

## Making changes to the engine

Edit `engine.cpp`, commit, push to `main`. The Action rebuilds and
commits the new exe automatically — next time you launch `new.ahk` it
picks up the update on its own.

## CPU usage note

Settings are shared with the hot loop via a lock-free atomic pointer
swap, so the loop never copies the config (vector/string) on each
iteration — that copy was the actual bottleneck in earlier versions,
not GetPixel or SendInput.

By default (`#define BUSY_SPIN 0`) the hot loop paces itself to
roughly 1ms per iteration using a real `Sleep(1)`, which actually
yields the CPU back to Windows between checks. That's still up to
~1000 pixel-checks/sec and ~500-1000 R-presses/sec — far faster than
either needs to be — at a small fraction of the CPU cost of a busy
spin.

If you want the absolute lowest possible latency and don't mind
pinning close to one full CPU core the entire time the engine runs,
change that to `#define BUSY_SPIN 1`.
