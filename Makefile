CC      ?= gcc
CFLAGS  += -O2 -Wall -Wextra
PKGS    := gtk+-3.0

cpu_meter: cpu_meter.c
	$(CC) $(CFLAGS) $(shell pkg-config --cflags $(PKGS)) -o $@ $< $(shell pkg-config --libs $(PKGS)) -lm

.PHONY: clean
clean:
	rm -f cpu_meter
