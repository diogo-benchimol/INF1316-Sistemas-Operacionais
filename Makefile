# =========================
# Makefile - KernelSim T2
# =========================

# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -O2

# Executables
KERNEL = KernelSim_T2
SERVER = sfss_server

# Sources
SRC_KERNEL = KernelSim_T2.c
SRC_SERVER = sfss_server.c

# Protocol header
PROTO_H = sfp_protocol.h

# Root directory for SFSS
SFSS_ROOT = sfss_root
SFSS_SUBDIRS = $(SFSS_ROOT)/A0 $(SFSS_ROOT)/A1 $(SFSS_ROOT)/A2 $(SFSS_ROOT)/A3 $(SFSS_ROOT)/A4 $(SFSS_ROOT)/A5

# ======================================================
# Compilation
# ======================================================

all: $(KERNEL) $(SERVER) dirs
	@echo "[Makefile] Build complete."

$(KERNEL): $(SRC_KERNEL) $(PROTO_H)
	@echo "[Makefile] Compiling KernelSim_T2..."
	$(CC) $(CFLAGS) -o $(KERNEL) $(SRC_KERNEL)

$(SERVER): $(SRC_SERVER) $(PROTO_H)
	@echo "[Makefile] Compiling sfss_server..."
	$(CC) $(CFLAGS) -o $(SERVER) $(SRC_SERVER)

# ======================================================
# Directory setup
# ======================================================

dirs:
	@echo "[Makefile] Ensuring sfss_root directories exist..."
	@mkdir -p $(SFSS_SUBDIRS)
	@echo "[Makefile] All directories ready: $(SFSS_SUBDIRS)"

# Deep clean of sfss_root (files + subdirectories)
clean-root:
	@echo "[Makefile] Deep cleaning sfss_root..."
	@if [ ! -d "$(SFSS_ROOT)" ]; then \
		echo "[Makefile] Error: $(SFSS_ROOT) not found!"; \
		exit 1; \
	fi
	@for d in $(SFSS_SUBDIRS); do \
		if [ -d $$d ]; then \
			echo " - Cleaning $$d"; \
			find $$d -mindepth 1 -delete; \
		fi; \
	done
	@echo "[Makefile] sfss_root fully cleaned."

# ======================================================
# Execution
# ======================================================

run: all clean-root
	@echo "[Makefile] Launching KernelSim_T2..."
	./$(KERNEL)

server: all clean-root
	@echo "[Makefile] Launching SFSS server..."
	./$(SERVER) $(SFSS_ROOT)

# Runs server and kernel automatically (for demo/testing)
demo: all clean-root
	@echo "[Makefile] Starting SFSS server in background..."
	@./$(SERVER) $(SFSS_ROOT) > sfss_server.log 2>&1 &
	@sleep 1
	@echo "[Makefile] Starting KernelSim_T2..."
	@./$(KERNEL)
	@echo "[Makefile] Demo finished. Check sfss_server.log for server output."

# ======================================================
# Cleanup
# ======================================================

clean:
	@echo "[Makefile] Cleaning build files..."
	rm -f $(KERNEL) $(SERVER)
	@echo "[Makefile] Done."
