# Makefile for the assignment 5 "aesdsocket" server.
#
#   make                                         -> native build (uses gcc)
#   make CROSS_COMPILE=aarch64-none-linux-gnu-   -> aarch64 cross build
#   make clean                                   -> remove aesdsocket and all .o files
#
# CROSS_COMPILE is prefixed onto the compiler name, so leaving it unset selects
# the native toolchain and setting it selects the cross toolchain. CC, CFLAGS
# and LDFLAGS may all be overridden on the command line or from the
# environment; Buildroot does exactly that when it builds this package.

CROSS_COMPILE ?=

# make pre-defines CC=cc, so "CC ?=" would never take effect. Only replace CC
# when it still has make's built-in default value.
ifeq ($(origin CC),default)
CC = $(CROSS_COMPILE)gcc
endif

CFLAGS  ?= -g -Wall -Werror
LDFLAGS ?=

TARGET  = aesdsocket
SRCS    = aesdsocket.c

# Default target: build the aesdsocket server.
all default: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -f $(TARGET) *.o

.PHONY: all default clean
