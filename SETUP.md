# Setup runbook

Native Windows is the primary path: the package ships `bin\swarm_sim.exe`, so
there is nothing between me and the simulator. WSL works too if I prefer it —
see the bottom.

Run the PowerShell scripts like this, so the execution policy never gets in the
way:

    powershell -ExecutionPolicy Bypass -File scripts\build.ps1

---

## Tonight, Windows

1. **Check what toolchain is already here.** In a normal PowerShell window:

       where.exe cmake
       where.exe cl
       where.exe g++
       where.exe python

   `cmake` plus either `cl` (MSVC) or `g++` (MinGW) is enough. If `cl` is found
   but only inside the VS developer prompt, use that prompt for everything below.
   If none of them are found, jump to **If there is no compiler** and come back.

2. **Unzip the challenge package into `pkg\`.**
   The result must be `pkg\bin\swarm_sim.exe`, `pkg\sdk\include\`,
   `pkg\scenarios\`, `pkg\examples\`. `pkg\` is committed to git, not ignored.

3. **Commit the package.** `.gitattributes` already marks `pkg/bin/**` and
   `*.exe` binary, so the exe is safe to commit as-is.

       git add pkg
       git commit -m "Add challenge package v0.6.0"

4. **Find out what the simulator's command line actually is.**

       cd pkg
       .\bin\swarm_sim.exe --help
       cd ..

   The flags in `scripts\run.ps1` and `scripts\example.ps1`
   (`--scenario --brain --trace --report`) are a guess written before the
   package existed. Correct them now if they differ — it is a two-line edit in
   each and it saves a confusing failure later.

5. **Build and run THEIR example brain.** This produces tonight's fixture
   recording and proves the toolchain, the simulator and the trace format all
   work before any of my code exists.

       powershell -ExecutionPolicy Bypass -File scripts\example.ps1

   Output: `runs\fixture.jsonl` and `runs\fixture.json`.

6. **See what is actually in the recording.**

       python tools\inspect_trace.py runs\fixture.jsonl

   Prints record-type counts and the first example of each type. Read the
   `header` record: it defines the `frame` column names, and the viewer
   depends on them. The source of truth is the JSONL itself, not a dump of it.

7. **Commit the fixture.** `.gitignore` keeps `runs\fixture.jsonl` and
   `runs\fixture.json` while ignoring the rest of `runs\`, so no `-f` needed.

       git add runs\fixture.jsonl runs\fixture.json
       git commit -m "Add fixture recording"

8. **Build the viewer dataset** from that recording. Python owns all parsing;
   Unity only loads the two files this writes. See [FORMAT.md](FORMAT.md).

       python tools\build_viewer_data.py runs\fixture.jsonl runs\fixture
       python tools\plot_fixture.py
       powershell -ExecutionPolicy Bypass -File scripts\sync_viewer_data.ps1

   `runs\fixture_check.png` must show a clean 60 m ring of friendlies at
   y ≈ +30. If it does not, the parse is wrong — do not start the viewer.
   `sync_viewer_data.ps1` copies `fixture.bin` and `fixture.meta.json` into
   `viewer\fiducial-swarm-viz\Assets\StreamingAssets\` (the viewer loads a
   copy; skipping this is how those files go stale). Pass `-Rebuild` to run
   the sidecar and copy in one step.

9. **Paste in my own sources**, then build and run them:

       # brain sources -> brain\src\   (at least one .cpp)
       powershell -ExecutionPolicy Bypass -File scripts\build.ps1
       powershell -ExecutionPolicy Bypass -File scripts\run.ps1 -Scenario s1

   `build.ps1` refuses to configure an empty `brain\src\` and says so; so does
   `brain\CMakeLists.txt`. That is deliberate — an empty SHARED library fails
   later and far more confusingly.

---

## If there is no compiler

Install **Visual Studio Build Tools** (not full Visual Studio — the Build Tools
package is enough) and tick the **"Desktop development with C++"** workload.
That gives `cl`, the Windows SDK, and a bundled CMake.

Afterwards, open **"x64 Native Tools Command Prompt for VS"** from the Start
menu and run the scripts from there — that prompt puts `cl` and `cmake` on PATH.
A plain PowerShell window will not find them unless you added them yourself.

Check it worked: `cl` should print a version banner, and `cmake --version`
should answer.

---

## Running under WSL instead

The package ships both binaries, so the Linux one works too if you would rather
work in WSL. Build with cmake as usual and run `bin/swarm_sim` instead of
`bin/swarm_sim.exe`; `tools/check_determinism.sh` expects that path. Everything
else — the scenarios, the scores, the trace format — is identical, and scores
are bit-identical across machines, so runs from either side compare directly.

---

## Gotchas

- **Run the simulator from inside `pkg\`.** It resolves `scenarios\` relative to
  the working directory. Every script does `Push-Location pkg` first and passes
  absolute paths for the brain, trace and report; that is the single most
  important thing `scripts\common.ps1` does.
- **Do not tail the trace file.** It is created empty when the run starts and
  written only at the end. An empty file mid-run means nothing is wrong.
  `inspect_trace.py` says so explicitly if it finds no records.
- **`--dump-params` is refused for generated `x<tier>-<token>` ids.** Only the
  fixed scenario ids accept it.
- **Scores are bit-identical across machines and thread counts.** A score is
  comparable no matter where it was produced, so numbers recorded tonight stay
  valid all week and there is no need to re-run everything on a different box.

---

## Replaying a RESULTS.md version

The ladder in RESULTS.md is `scripts\versions.csv`. To rebuild one commit's
brain and the eight-id sweep (without checking out that commit):

    powershell -ExecutionPolicy Bypass -File scripts\ablation.ps1 -List
    powershell -ExecutionPolicy Bypass -File scripts\ablation.ps1 -Version V7
    powershell -ExecutionPolicy Bypass -File scripts\ablation.ps1 -Version V7 -Trace

Reports, a log, and optional viewer sidecars land in `runs\ablation\V7\`.
Gitignored. See RESULTS.md "Reproducing the ladder".
