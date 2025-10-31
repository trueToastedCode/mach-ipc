# ============================================================================
# Mach IPC Framework - Complete Makefile
# ============================================================================

PROJECT = mach_ipc
VERSION = 1.0.0

# Directories
SRC_DIR = src
INC_DIR = include
BUILD_DIR = build
LIB_DIR = lib
EXAMPLE_DIR = examples
TEST_DIR = tests

# Compiler settings
CC = clang
AR = ar
CFLAGS = -Wall -Wextra -std=c11 -I$(INC_DIR) -I$(SRC_DIR)
CFLAGS += -O2 -g
LDFLAGS = -framework CoreFoundation -lpthread

# Debug build
DEBUG ?= 0
ifeq ($(DEBUG), 1)
    CFLAGS += -DDEBUG -O0 -fsanitize=address,undefined
    LDFLAGS += -fsanitize=address,undefined
endif

# Source files
FRAMEWORK_SRCS = \
    $(SRC_DIR)/server.c \
    $(SRC_DIR)/client.c \
    $(SRC_DIR)/protocol.c \
    $(SRC_DIR)/resources.c \
    $(SRC_DIR)/pool.c \
    $(SRC_DIR)/utils.c

FRAMEWORK_OBJS = $(FRAMEWORK_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

# Headers (for dependency tracking)
FRAMEWORK_HDRS = \
    $(INC_DIR)/mach_ipc.h \
    $(SRC_DIR)/internal.h \
    $(SRC_DIR)/pool.h \
    $(SRC_DIR)/event_framework.h \
    $(SRC_DIR)/log.h

# Library targets
STATIC_LIB = $(LIB_DIR)/lib$(PROJECT).a
DYNAMIC_LIB = $(LIB_DIR)/lib$(PROJECT).dylib

# Example targets
EXAMPLES = \
    $(BUILD_DIR)/echo_server \
    $(BUILD_DIR)/echo_client

# Test targets
TESTS = \
    $(BUILD_DIR)/test_pool \
    $(BUILD_DIR)/test_basic

# ============================================================================
# Main targets
# ============================================================================

.PHONY: all clean test examples install uninstall help

all: $(STATIC_LIB) $(DYNAMIC_LIB)
	@echo "Build complete!"
	@echo "Static library:  $(STATIC_LIB)"
	@echo "Dynamic library: $(DYNAMIC_LIB)"

examples: $(EXAMPLES)
	@echo "Examples built successfully"

test: $(TESTS)
	@echo "Running tests..."
	@for test in $(TESTS); do \
		echo ""; \
		echo "Running $$test..."; \
		$$test || exit 1; \
	done
	@echo ""
	@echo "✓ All tests passed!"

clean:
	rm -rf $(BUILD_DIR) $(LIB_DIR)
	@echo "Clean complete"

install: $(STATIC_LIB) $(DYNAMIC_LIB) $(INC_DIR)/$(PROJECT).h
	@echo "Installing to /usr/local..."
	install -d /usr/local/lib
	install -d /usr/local/include
	install -m 644 $(STATIC_LIB) /usr/local/lib/
	install -m 755 $(DYNAMIC_LIB) /usr/local/lib/
	install -m 644 $(INC_DIR)/$(PROJECT).h /usr/local/include/
	@echo "Installation complete"

uninstall:
	rm -f /usr/local/lib/lib$(PROJECT).*
	rm -f /usr/local/include/$(PROJECT).h
	@echo "Uninstall complete"

# ============================================================================
# Build rules
# ============================================================================

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(LIB_DIR):
	@mkdir -p $(LIB_DIR)

# Object files depend on sources and all headers
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(FRAMEWORK_HDRS) | $(BUILD_DIR)
	@echo "CC $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(STATIC_LIB): $(FRAMEWORK_OBJS) | $(LIB_DIR)
	@echo "AR $@"
	@$(AR) rcs $@ $^

$(DYNAMIC_LIB): $(FRAMEWORK_OBJS) | $(LIB_DIR)
	@echo "LD $@"
	@$(CC) -dynamiclib -o $@ $^ $(LDFLAGS)

# ============================================================================
# Examples
# ============================================================================

$(BUILD_DIR)/echo_server: $(EXAMPLE_DIR)/echo_server.c $(STATIC_LIB) | $(BUILD_DIR)
	@echo "CC $@"
	@$(CC) $(CFLAGS) $< -L$(LIB_DIR) -l$(PROJECT) $(LDFLAGS) -o $@

$(BUILD_DIR)/echo_client: $(EXAMPLE_DIR)/echo_client.c $(STATIC_LIB) | $(BUILD_DIR)
	@echo "CC $@"
	@$(CC) $(CFLAGS) $< -L$(LIB_DIR) -l$(PROJECT) $(LDFLAGS) -o $@

# ============================================================================
# Tests
# ============================================================================

$(BUILD_DIR)/test_%: $(TEST_DIR)/test_%.c $(STATIC_LIB) | $(BUILD_DIR)
	@echo "CC $@"
	@$(CC) $(CFLAGS) $< -L$(LIB_DIR) -l$(PROJECT) $(LDFLAGS) -o $@

# ============================================================================
# Project structure setup
# ============================================================================

.PHONY: setup
setup:
	@echo "Creating project structure..."
	@mkdir -p $(SRC_DIR) $(INC_DIR) $(BUILD_DIR) $(LIB_DIR)
	@mkdir -p $(EXAMPLE_DIR) $(TEST_DIR)
	@touch $(SRC_DIR)/.gitkeep
	@touch $(EXAMPLE_DIR)/.gitkeep
	@touch $(TEST_DIR)/.gitkeep
	@echo "Project structure created"
	@echo ""
	@echo "Directory structure:"
	@find . -type d -not -path '*/\.*' | sed 's|[^/]*/|  |g'

# ============================================================================
# Development helpers
# ============================================================================

.PHONY: format check

format:
	@echo "Formatting code..."
	@clang-format -i $(SRC_DIR)/*.c $(INC_DIR)/*.h $(EXAMPLE_DIR)/*.c 2>/dev/null || echo "clang-format not found"

check:
	@echo "Running static analysis..."
	@clang-tidy $(SRC_DIR)/*.c -- $(CFLAGS) 2>/dev/null || echo "clang-tidy not found"

# ============================================================================
# Help
# ============================================================================

help:
	@echo "$(PROJECT) v$(VERSION) - Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all          - Build static and dynamic libraries (default)"
	@echo "  examples     - Build all example programs"
	@echo "  test         - Build and run all tests"
	@echo "  clean        - Remove all build artifacts"
	@echo "  install      - Install libraries to /usr/local (requires sudo)"
	@echo "  uninstall    - Remove installed libraries (requires sudo)"
	@echo "  setup        - Create project directory structure"
	@echo "  format       - Format source code with clang-format"
	@echo "  check        - Run static analysis with clang-tidy"
	@echo ""
	@echo "Options:"
	@echo "  DEBUG=1      - Build with debug symbols and sanitizers"
	@echo ""
	@echo "Examples:"
	@echo "  make                    # Build libraries"
	@echo "  make examples           # Build examples"
	@echo "  make test               # Run tests"
	@echo "  make DEBUG=1 all        # Debug build"
	@echo "  sudo make install       # Install system-wide"
	@echo ""
	@echo "Quick Start:"
	@echo "  1. make all"
	@echo "  2. make examples"
	@echo "  3. Terminal 1: ./build/echo_server"
	@echo "  4. Terminal 2: ./build/echo_client"

.DEFAULT_GOAL := all