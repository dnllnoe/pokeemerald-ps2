CC ?= gcc

CFLAGS = -Wall -Wextra -Werror -std=c11 -O2

.PHONY: all clean

SRCS = elf2macho.c

ifeq ($(OS),Windows_NT)
EXE := .exe
else
EXE :=
endif

all: elf2macho$(EXE)
	@:

elf2macho$(EXE): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $@ $(LDFLAGS)

clean:
	$(RM) elf2macho elf2macho.exe
