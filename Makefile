CC = gcc

MILK_INSTALLDIR ?= /usr/local/milk-1.03.00

CFLAGS = -O3 -I$(MILK_INSTALLDIR)/include -Wall -Iinclude
LDLIBS = -lImageStreamIO -lm -lportaudio
LDFLAGS = -L$(MILK_INSTALLDIR)/lib

TARGETS = dataProcessEX paRead posTrack test monitorCount monitorTime

all: $(TARGETS)

%: src/%.c
	$(CC) $(CFLAGS) $< $(LDFLAGS) $(LDLIBS) -o bin/$@

clean:
	rm -f bin/*