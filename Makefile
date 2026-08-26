# All targets assume you are ALREADY INSIDE the dev container. Get there with:
#
#     docker compose run --rm dev
#
# Nothing here is expected to work on the host: the simulator is a linux/amd64
# ELF binary and the Python deps live in the container's /opt/venv.
#
# FLAG SPELLINGS: the simulator's command line below is a best guess from the
# brief -- I have not run it yet. On the first evening run
# `cd pkg && ./bin/swarm_sim --help` and correct this block if it differs.
# s0/s1/run/fixture all go through the run_sim macro, so they are one edit;
# sweep and determinism have their own lines because their flags differ.
# EXAMPLE is likewise a guess at what pkg/examples/CMakeLists.txt names its
# output -- check pkg/examples/build after `make example` and fix it here.

PKG        := pkg
SIM        := ./bin/swarm_sim
BRAIN      ?= ../brain/build/brain.so
EXAMPLE    ?= examples/build/brain.so
SCENARIO   ?= s1
SCENARIOS  ?= s1,x1-a,x1-b
JOBS       ?= 4

# $(1) scenario id, $(2) brain path relative to pkg/, $(3) output stem relative to pkg/
# The cd is required: the simulator resolves scenarios/ against the working directory.
run_sim = mkdir -p runs && cd $(PKG) && $(SIM) --scenario $(1) --brain $(2) --trace $(3).jsonl --report $(3).json

.DEFAULT_GOAL := help
.PHONY: help example brain s0 s1 run fixture sweep determinism inspect clean

help:
	@echo "Run these inside the container (docker compose run --rm dev):"
	@echo "  make example      build the packaged example brain into pkg/examples/build"
	@echo "  make brain        build brain/ into brain/build (Release)"
	@echo "  make s0           run scenario s0 with my brain -> runs/last.{jsonl,json}"
	@echo "  make s1           run scenario s1 with my brain -> runs/last.{jsonl,json}"
	@echo "  make run          same, with SCENARIO=$(SCENARIO) BRAIN=$(BRAIN) overridable"
	@echo "  make fixture      run s1 with the EXAMPLE brain -> runs/fixture.{jsonl,json}"
	@echo "  make sweep        sweep SCENARIOS=$(SCENARIOS) -> runs/sweep"
	@echo "  make determinism  same run at 1 and 8 threads, then diff the reports"
	@echo "  make inspect      dump the record types in runs/fixture.jsonl"
	@echo "  make clean        remove build dirs and non-fixture run output"

example:
	cmake -S $(PKG)/examples -B $(PKG)/examples/build -DCMAKE_BUILD_TYPE=Release
	cmake --build $(PKG)/examples/build -j

brain:
	cmake -S brain -B brain/build -DCMAKE_BUILD_TYPE=Release
	cmake --build brain/build -j

s0: brain
	$(call run_sim,s0,$(BRAIN),../runs/last)

s1: brain
	$(call run_sim,s1,$(BRAIN),../runs/last)

run: brain
	$(call run_sim,$(SCENARIO),$(BRAIN),../runs/last)

fixture: example
	$(call run_sim,s1,$(EXAMPLE),../runs/fixture)

sweep: brain
	mkdir -p runs/sweep
	cd $(PKG) && $(SIM) --sweep $(SCENARIOS) --brain $(BRAIN) --report-dir ../runs/sweep --jobs $(JOBS)

determinism: brain
	mkdir -p runs
	cd $(PKG) && $(SIM) --scenario $(SCENARIO) --brain $(BRAIN) --threads 1 --record --trace ../runs/det1.jsonl --report ../runs/det1.json
	cd $(PKG) && $(SIM) --scenario $(SCENARIO) --brain $(BRAIN) --threads 8 --replay --trace ../runs/det2.jsonl --report ../runs/det2.json
	diff runs/det1.json runs/det2.json && echo "determinism OK: reports are identical"

inspect:
	python tools/inspect_trace.py runs/fixture.jsonl

clean:
	rm -rf brain/build $(PKG)/examples/build runs/last.* runs/det1.* runs/det2.* runs/sweep
