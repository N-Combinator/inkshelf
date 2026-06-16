# inkshelf — build & wireless deploy
#
# Targets:
#   make build                      cross-compile inkshelf.app (wraps build.sh; needs the SDK)
#   make test                       run host unit/integration tests (no SDK needed)
#   make deploy    DEVICE=<ip>       scp the .app onto a jailbroken (PBJB) reader over WiFi
#   make deploy-nc DEVICE=<ip>       push the .app via the on-device netcat receiver.app
#   make clean                      remove the build/ dir
#
# The build artifact is a single ELF renamed inkshelf.app (see CMakeLists.txt),
# installed on the reader under  /mnt/ext1/applications/.

APP        := build/inkshelf.app
REMOTE_DIR := /mnt/ext1/applications
REMOTE_APP := $(REMOTE_DIR)/inkshelf.app

# --- SSH/scp deploy knobs ---------------------------------------------------
# PBJB ships an old dropbear: modern OpenSSH disables ssh-rsa by default, so we
# have to re-enable it for both the host key and pubkey auth or the connection
# is refused outright. StrictHostKeyChecking=accept-new avoids an interactive
# prompt on first connect without blindly trusting a changed key later.
# SSH_USER (not USER — make inherits $USER from the environment, so ?= on USER
# would never take effect). PORT default 22; PBJB dropbear is sometimes on 2468.
SSH_USER ?= root
PORT     ?= 22
SSH_OPTS := -o HostKeyAlgorithms=+ssh-rsa \
            -o PubkeyAcceptedAlgorithms=+ssh-rsa \
            -o StrictHostKeyChecking=accept-new
SSH      := ssh $(SSH_OPTS) -p $(PORT)
SCP      := scp $(SSH_OPTS) -P $(PORT)

# --- netcat deploy knobs ----------------------------------------------------
# Must match the on-device receiver.app listen port.
NC_PORT  ?= 19991

.PHONY: all build test deploy deploy-nc clean

all: build

build:
	./build.sh

test:
	tests/run_host_tests.sh

# --- make deploy DEVICE=<ip> [PORT=22] [SSH_USER=root] ----------------------
# scp over SSH to a PBJB reader. A running inkshelf.app holds its ELF open, so
# writing straight over it fails with ETXTBSY. We sidestep that completely:
# copy to a .new file (different inode, never busy), best-effort kill the live
# instance, then rename over the target — renaming over a busy/unlinked inode
# is fine on Linux, and the next launch picks up the new binary.
deploy:
	@test -n "$(DEVICE)" || { echo "usage: make deploy DEVICE=<ip> [PORT=22] [SSH_USER=root]"; exit 2; }
	@test -f "$(APP)" || { echo "error: $(APP) not built — run 'make build' on an SDK host first"; exit 1; }
	@echo "==> uploading $(APP) -> $(SSH_USER)@$(DEVICE):$(REMOTE_APP)"
	$(SCP) "$(APP)" "$(SSH_USER)@$(DEVICE):$(REMOTE_APP).new"
	@echo "==> stopping any running instance + swapping binary in place"
	$(SSH) "$(SSH_USER)@$(DEVICE)" \
		'kill $$(pidof inkshelf.app) 2>/dev/null; killall inkshelf.app 2>/dev/null; \
		 mv -f "$(REMOTE_APP).new" "$(REMOTE_APP)" && sync'
	@echo "==> done. Relaunch inkshelf from the reader's application list."

# --- make deploy-nc DEVICE=<ip> [NC_PORT=19991] -----------------------------
# No-root path: the reader runs receiver.app (a shell script doing `nc -l`).
# Protocol (per blog.flxzt.net/posts/pb-cheatsheet): the host first sends the
# target filename on one connection, then the raw binary on a second. The
# receiver writes it into applications/ under that name.
# NOTE: some host nc builds (OpenBSD/GNU) don't close the socket on stdin EOF —
# if the binary transfer hangs, add  -N  (BSD) or  -q0  (traditional) to the
# second nc below.
deploy-nc:
	@test -n "$(DEVICE)" || { echo "usage: make deploy-nc DEVICE=<ip> [NC_PORT=19991]"; exit 2; }
	@test -f "$(APP)" || { echo "error: $(APP) not built — run 'make build' on an SDK host first"; exit 1; }
	@command -v nc >/dev/null 2>&1 || { echo "error: nc (netcat) not found on host"; exit 1; }
	@echo "==> sending filename to $(DEVICE):$(NC_PORT)"
	echo inkshelf.app | nc "$(DEVICE)" "$(NC_PORT)"
	@sleep 3
	@echo "==> sending binary"
	nc "$(DEVICE)" "$(NC_PORT)" < "$(APP)"
	@echo "==> done. Relaunch inkshelf from the reader's application list."

clean:
	rm -rf build
