# Setup runbook

Everything builds and runs inside one linux/amd64 container. The host only ever
needs Docker and git. Follow the section for the machine you are on.

---

## Tonight, macOS (Apple Silicon)

1. **Turn on Rosetta in Docker Desktop.**
   Settings > General > "Use Rosetta for x86/amd64 emulation". Apply & restart.
   Without this the x86-64 simulator binary will not start.

2. **Unzip the challenge package into `pkg/`.**
   The result must be `pkg/bin/swarm_sim`, `pkg/sdk/include/`, `pkg/scenarios/`,
   `pkg/examples/`. `pkg/` is committed to git; it is not ignored.

3. **Confirm the binary is what you think it is.**

       file pkg/bin/swarm_sim

   Expect `ELF 64-bit LSB ... x86-64`.

4. **Store the exec bit in git so it survives the move to Windows.**

       chmod +x pkg/bin/swarm_sim
       git update-index --chmod=+x pkg/bin/swarm_sim

   Windows checkouts have no POSIX permissions; the mode recorded in the index
   is what makes the file executable inside the container on Sunday.

5. **Build the image and get a shell.**

       docker compose build
       docker compose run --rm dev

   Everything below runs inside that shell.

6. **Check the simulator's real command line before trusting the Makefile.**

       cd pkg && ./bin/swarm_sim --help ; cd ..

   The flag spellings in the Makefile are a best guess, written before the
   package existed. Fix them in the variable block at the top of the Makefile
   now if they differ — the scenario runs share one macro, so it is one edit.

7. **Build the packaged example brain.**

       make example

   Then check what it produced: `ls pkg/examples/build`. If the library is not
   called `brain.so`, set `EXAMPLE` at the top of the Makefile to its real path
   (relative to `pkg/`).

8. **Record a fixture trace with the example brain.**

       make fixture

9. **See what is actually in the trace.**

       make inspect

   Read the `header` record carefully: it defines the `frame` column names, and
   the viewer and any analysis depend on it.

10. **Commit the fixture** (`runs/fixture.jsonl`, `runs/fixture.json`). The
    `.gitignore` ignores run output except `runs/fixture.*`, so `git add -f` is
    not needed.

---

## Sunday, Windows

1. Install Docker Desktop with the **WSL2 backend** (Settings > General > "Use
   the WSL 2 based engine").
2. `git clone <this repo>` and `cd` into it.
3. `docker compose run --rm dev`
4. `make example`
5. `make s1`

That is the whole setup. No toolchain, no Python, no compiler on the host.

---

## Gotchas

- **Run the simulator from inside `pkg/`.** It resolves `scenarios/` relative to
  the working directory, so every run target in the Makefile does `cd pkg` first
  and writes its output back up to `../runs/`.
- **Do not tail the trace file.** It is created empty when the run starts and
  written only at the end. An empty file mid-run means nothing is wrong.
- **`--dump-params` is refused for generated `x<tier>-<token>` ids.** Only the
  fixed scenario ids accept it.
- **Scores are bit-identical across machines and thread counts.** A number from
  a Rosetta run on the MacBook compares directly with one from the Windows PC,
  so results recorded tonight stay valid on Sunday. `make determinism` checks
  this by running at 1 and 8 threads and diffing the reports.
- **Tick compute time is measured but not scored.** Rosetta emulation makes the
  dev loop slow, not the result worse. Do not spend time optimising for it, and
  do not read timing numbers from a Mac run as if they were meaningful.
