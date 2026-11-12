# halfcraft's tasks. each one wraps a script in tools\ (read it for the details); the real builds are
# vpc + msbuild (half-life side) and gradle (minecraft side). needs gnu make 4+ on windows, e.g.
# `winget install ezwinports.make`, python 3.13+ and windows powershell. on linux: gnu make, python 3.13+, docker
# (the builds run in containers) and jdk 25; only ENGINE=hl2dm, half-life 2: deathmatch's linux engine.
#
#   make setup                     clone both sdk trees and apply halfcraft's patches
#   make build [ENGINE=hl2]        dlls, shaders, dev game folders build\game-<engine>, HalfCraft.exe
#   make mc                        the minecraft mod's jar
#   make mc-run                    the minecraft dev client
#   make mc-server                 the halfcraft server's minecraft: a dedicated server in minecraft/run/server
#   make mc-test [LINK=guest]      the minecraft test client (after test-start): its own run folder and world
#   make run [ENGINE=hl2] [MAP=d1_canals_01] [ARGS="+sv_cheats 1"] [LINK=guest] [WAIT=1]
#   make cmd [LINK=guest] C="'save a' 'load a'"   console commands into the running game (that link's copy)
#   make server [MAP=d2_coast_01] [PORT=27015]   linux: the source dedicated server with halfcraft (HALFCRAFT_SRCDS)
#   make server-layout             linux: lay out build/server/game-hl2dm for it (make server does it too)
#   make server-cmd C="status"      linux: console commands into the running server (rcon)
#   make test-start [ENGINE=hl2]   set the game's saves and settings aside for tests; make test-stop: back
#   make patches                   write sdk edits back into source\sdk\halfcraft-<engine>.patch
#   make proto                     protocol\halfcraft_protocol.h, minecraft's Proto.java and ProtoStructs.java from protocol\schema
#   make package                   dist\HalfCraft-<version>.zip (NOBUILD=1 packs what's built)
#   make format                    clang-format the c++, ruff the python, eclipse's formatter the java; make lint: check
#                                  (and that the protocol files are what the schema gives)
#   make tidy                      clang-tidy over halfcraft's c++, both engines (after make build)
#   make core-test                 the c++ core's own tests (tools\debug\core_tests.cpp), then the tools' (tool_tests.py): no game
#
# ENGINE: hl2 (half-life 2's own 32-bit engine), hl2dm (half-life 2: deathmatch's 64-bit one) or all
# (default; `make run` takes hl2dm then). NOPROJECTS=1 skips vpc when no .vpc file changed. LINK=guest: the
# second pair on one pc for co-op tests (its own link, copy, console-guest.log and minecraft). WAIT=1: make run
# returns when the game exits (tools/game/hidden_desktop.py keeps its desktop while it waits).

# windows powershell runs the recipes on windows: python calls, with its quoting (make cmd C="'save a' 'load a'");
# bash on linux, where half-life 2: deathmatch's linux engine is the one built (docker builds it: tools/halfcraft/linux.py)
ifeq ($(OS),Windows_NT)
SHELL := powershell.exe
.SHELLFLAGS := -NoProfile -Command
PYTHON := python
else
SHELL := /bin/bash
PYTHON := python3
endif
.DEFAULT_GOAL := help
.PHONY: help setup build mc mc-run mc-server mc-test run cmd server server-layout server-cmd test-start test-stop patches proto package format lint tidy core-test clean

ENGINE ?= all
RUN_ENGINE := $(if $(filter all,$(ENGINE)),hl2dm,$(ENGINE))
MAP ?=
ARGS ?=
C ?=
LINK ?=
WAIT ?=

help:
ifeq ($(OS),Windows_NT)
	@Get-Content Makefile | Select-Object -First 29 | ForEach-Object { $$_ -replace '^# ?', '' }
else
	@head -n 29 Makefile | sed -E 's/^# ?//'
endif

setup:
	$(PYTHON) tools/build/setup_sdk.py --engine $(ENGINE)

build:
	$(PYTHON) tools/build/build_hl2.py --engine $(ENGINE) $(if $(NOPROJECTS),--no-projects)

mc:
	$(PYTHON) tools/build/gradle.py build

mc-run:
	$(PYTHON) tools/game/test_session.py dev-client

mc-server:
	$(PYTHON) tools/build/gradle.py runServer

mc-test:
	$(PYTHON) tools/game/test_session.py minecraft $(if $(LINK),--link $(LINK))

run:
	$(PYTHON) tools/game/run_hl2.py --engine $(RUN_ENGINE) $(if $(LINK),--link $(LINK)) $(if $(WAIT),--wait) $(if $(MAP),--map $(MAP)) $(if $(ARGS),-- $(ARGS))

cmd:
	$(PYTHON) tools/game/hl2_command.py $(if $(LINK),--link $(LINK)) -- $(C)

server:
	$(PYTHON) tools/server/srcds.py run --map $(or $(MAP),d2_coast_01) $(if $(PORT),--port $(PORT)) --log build/server/srcds-$(or $(PORT),27015).log

server-layout:
	$(PYTHON) tools/server/srcds.py layout

server-cmd:
	$(PYTHON) tools/server/rcon.py $(if $(PORT),--port $(PORT)) $(C)

test-start:
	$(PYTHON) tools/game/test_session.py start --engine $(ENGINE)

test-stop:
	$(PYTHON) tools/game/test_session.py stop --engine $(ENGINE)

patches:
	$(PYTHON) tools/build/update_patches.py --engine $(ENGINE)

proto:
	$(PYTHON) tools/build/protocol.py

package:
	$(PYTHON) tools/build/package.py $(if $(NOBUILD),--no-build)

format:
	$(PYTHON) tools/lint/format.py
	$(PYTHON) tools/build/gradle.py spotlessApply

lint:
	$(PYTHON) tools/build/protocol.py --check
	$(PYTHON) tools/lint/format.py --check
	$(PYTHON) tools/build/gradle.py spotlessCheck

tidy:
	$(PYTHON) tools/lint/tidy.py

core-test:
	$(PYTHON) tools/debug/core_tests.py
	$(PYTHON) tools/debug/tool_tests.py

# build output that's safe to throw away: not build\game-* (saves and settings live there)
clean:
ifeq ($(OS),Windows_NT)
	'build\shaders', 'build\launcher', 'dist' | Where-Object { Test-Path $$_ } | Remove-Item -Recurse -Force
else
	rm -rf build/shaders build/launcher dist
endif
