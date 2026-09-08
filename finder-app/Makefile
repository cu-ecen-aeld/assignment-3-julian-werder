# Makefile for the assignment 2 "writer" utility.
#
#   make                                   -> native build (uses gcc)
#   make CROSS_COMPILE=aarch64-none-linux-gnu-   -> aarch64 cross build
#   make clean                             -> remove writer and all .o files
#
# CROSS_COMPILE is prefixed onto the tool names, so leaving it unset selects
# the native toolchain and setting it selects the cross toolchain, per the
# assignment requirements.

CROSS_COMPILE ?=
CC      = $(CROSS_COMPILE)gcc
CFLAGS  ?= -Wall -Werror -g
LDFLAGS ?=

TARGET  = writer
SRCS    = writer.c
OBJS    = $(SRCS:.c=.o)

# Default target: build the writer application.
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS) *.o

.PHONY: all clean
