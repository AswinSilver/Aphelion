CC ?= gcc
# GNU make supplies a built-in `CC=cc`; prefer GCC for the documented default
# while still honoring an explicit command-line or environment override.
ifeq ($(origin CC),default)
CC := gcc
endif
TARGET ?= aphelion
PYTHON ?= python
# CC and PYTHON name single executables, including paths containing spaces.
# Forward slashes work in native Windows programs and in MSYS/Git Bash.
CC_COMMAND = $(subst \,/,$(CC))
PYTHON_COMMAND = $(subst \,/,$(PYTHON))

CPPFLAGS += -Iinclude
override CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror
LDFLAGS ?=

# MinGW's runtime supplies the math functions directly; Unix-like toolchains
# need the separate libm dependency.  TARGET remains the portable base name,
# so TARGET=aphelion produces aphelion.exe on Windows.
ifeq ($(OS),Windows_NT)
EXEEXT ?= .exe
LDLIBS ?=
LDLIBS += -lshell32
else
EXEEXT ?=
LDLIBS ?=
LDLIBS += -lm
endif

SOURCES := $(sort $(wildcard src/*.c))
OBJECTS := $(SOURCES:.c=.o)
DEPENDENCIES := $(OBJECTS:.o=.d)
BINARY := $(TARGET)$(EXEEXT)
SANITIZE_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -g
SANITIZE_TARGET := $(TARGET)-sanitize$(EXEEXT)
empty :=
space := $(empty) $(empty)
escape_spaces = $(subst $(space),\$(space),$(1))

.PHONY: all build test sanitize clean $(call escape_spaces,$(SANITIZE_TARGET))

all: build

build: $(call escape_spaces,$(BINARY))

$(call escape_spaces,$(BINARY)): $(OBJECTS)
	"$(CC_COMMAND)" $(LDFLAGS) -o "$@" $^ $(LDLIBS)

src/%.o: src/%.c
	"$(CC_COMMAND)" $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o "$@" "$<"

test: build
	# Resolve the artifact with native Python, not make's MSYS-style abspath.
	# Quote CC for the C-driver helper's platform-specific argument parser.
	"$(PYTHON_COMMAND)" -c "import os, pathlib, shlex, subprocess, sys; cc = sys.argv[1]; cc = subprocess.check_output(['cygpath', '-m', cc], text=True).strip() if os.name == 'nt' and cc.startswith('/') else cc; os.environ['CC'] = subprocess.list2cmdline([cc]) if os.name == 'nt' else shlex.join([cc]); os.environ['APHELION_TEST_EXECUTABLE'] = str(pathlib.Path(sys.argv[2]).resolve()); import pytest; sys.exit(pytest.main(['-q']))" "$(CC_COMMAND)" "$(BINARY)"

# This is intentionally a real sanitizer link.  If the selected compiler
# lacks AddressSanitizer/UBSan runtime libraries, the command fails instead
# of reporting a misleading successful sanitizer build.
sanitize: $(call escape_spaces,$(SANITIZE_TARGET))

$(call escape_spaces,$(SANITIZE_TARGET)): $(SOURCES)
	"$(CC_COMMAND)" $(CPPFLAGS) $(CFLAGS) $(SANITIZE_FLAGS) $(LDFLAGS) -o "$@" $^ $(LDLIBS)

clean:
	$(RM) $(OBJECTS) $(DEPENDENCIES) "$(TARGET)" "$(TARGET).exe" "$(SANITIZE_TARGET)" "$(TARGET)-sanitize" "$(TARGET)-sanitize.exe"

-include $(DEPENDENCIES)
