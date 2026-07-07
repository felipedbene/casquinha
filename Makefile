# Casquinha — a gopher-spot Spotify remote for Mac OS 9.2 (PowerPC / Open Transport).
# Sibling to DeToca (10.6/i386) and DeGelato (10.5/ppc); see fhb/CLIENT-PATTERN.md.
#
# Two target families:
#   * HOST  — the pure core + its offline test suite, built with the system cc.
#             Needs NO Retro68 and NO emulator: `make test` runs on this Mac.
#   * PPC   — the Mac OS 9 app (Open Transport + Toolbox), cross-built with
#             Retro68. Wired up from Fio 3 on; see the `app` target below.

# ---- host (pure core + tests) -----------------------------------------------
CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -pedantic -O0 -g -Isrc

CORE_SRC = \
	src/cq_codec.c \
	src/cq_now.c \
	src/cq_track.c \
	src/cq_guard.c \
	src/cq_view.c \
	src/cq_debounce.c \
	src/cq_backoff.c \
	src/cq_cache.c \
	src/cq_pls.c \
	src/cq_mp3.c \
	src/cq_mp3dec.c \
	src/cq_decring.c

# Transport: the POSIX impl is the host/test one; the Open Transport impl
# (src/cq_transport_ot.c) is compiled only in the ppc app build (-DCQ_OS9).
NET_SRC = src/cq_transport_posix.c

TEST_SRC = \
	tests/cq_test.c \
	tests/codec_test.c \
	tests/now_test.c \
	tests/track_test.c \
	tests/guard_test.c \
	tests/view_test.c \
	tests/debounce_test.c \
	tests/pls_test.c \
	tests/mp3_test.c \
	tests/mp3dec_test.c \
	tests/decring_test.c \
	tests/backoff_test.c \
	tests/cache_test.c \
	tests/transport_test.c \
	tests/run_tests.c

FIXTURES = $(CURDIR)/tests/Fixtures

.PHONY: all test probe clean app logtail

all: test

# Build and run the pure-core + transport suite, fully offline (localhost
# loopback), with the fixtures wired in.
test: build/run_tests
	CQ_FIXTURES=$(FIXTURES) build/run_tests

build/run_tests: $(CORE_SRC) $(NET_SRC) $(TEST_SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) $(CORE_SRC) $(NET_SRC) $(TEST_SRC) -o $@

# End-to-end check against the REAL server (host-side portkit). Override on the
# command line: make probe HOST=10.0.100.112 PORT=70
HOST ?= 10.0.100.112
PORT ?= 70
probe: build/probe
	build/probe $(HOST) $(PORT)

build/probe: $(CORE_SRC) $(NET_SRC) tools/probe.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(CORE_SRC) $(NET_SRC) tools/probe.c -o $@

# Live-tail the app's UDP log mirror (b34): every DbgLog line on the VM
# arrives here as one datagram while you test — no share round-trip.
# (NOT `nc -kul`: macOS nc latches onto the first sender and drops the rest.)
logtail:
	@python3 tools/loglisten.py 5514

clean:
	rm -rf build Casquinha.app

# ---- ppc (Mac OS 9 app via Retro68) -----------------------------------------
# Cross-builds the classic-PowerPC/CFM app (os9/) with the Retro68 toolchain:
# GCC + Rez + MakePEF + MacBinary packaging, driven by CMake's add_application.
# Point RETRO68 at your toolchain (default: ~/Retro68-build/toolchain).
RETRO68 ?= $(HOME)/Retro68-build/toolchain
PPC_TOOLCHAIN = $(RETRO68)/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
# Drop the app on the netatalk AFP share for the OS 9 VM to pick up (override
# with SHARE=... ; leave empty to skip the copy).
SHARE ?= $(HOME)/OrbStack/docker/containers/netatalk/share

app:
	@if [ ! -f "$(PPC_TOOLCHAIN)" ]; then \
	  echo "Retro68 not found at $(RETRO68)."; \
	  echo "Build it (--no-68k --no-carbon) or set RETRO68=<path-to-toolchain>."; \
	  echo "The pure core + transport are verifiable now with 'make test' — no Retro68 needed."; \
	  exit 1; \
	fi
	@mkdir -p os9/build
	cd os9/build && PATH="$(RETRO68)/bin:$$PATH" cmake .. \
	    -DCMAKE_TOOLCHAIN_FILE=$(PPC_TOOLCHAIN) -DCMAKE_BUILD_TYPE=Release >/dev/null
	cd os9/build && PATH="$(RETRO68)/bin:$$PATH" $(MAKE)
	@echo "built: os9/build/Casquinha.bin (MacBinary PPC app), .dsk (disk image)"
	@if [ -n "$(SHARE)" ] && [ -d "$(SHARE)" ]; then \
	  TAG=$$(sed -n 's/.*CQ_BUILD_TAG *"\([^"]*\)".*/\1/p' os9/casquinha.c | head -1); \
	  rm -f "$(SHARE)/Casquinha.bin" "$(SHARE)/Casquinha.dsk"; \
	  cp -X os9/build/Casquinha.bin "$(SHARE)/Casquinha-$$TAG.bin"; \
	  cp -X os9/build/Casquinha.dsk "$(SHARE)/Casquinha-$$TAG.dsk"; \
	  cp -X os9/build/Casquinha.bin os9/build/Casquinha.dsk "$(SHARE)/"; \
	  echo "dropped Casquinha-$$TAG.bin/.dsk (+ unversioned latest) on $(SHARE) (sha1 $$(shasum -a1 os9/build/Casquinha.bin | cut -c1-12))"; \
	else \
	  echo "SHARE not found ($(SHARE)) -- .bin NOT copied"; \
	fi
