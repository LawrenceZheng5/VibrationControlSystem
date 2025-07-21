CC = gcc
CFLAGS = -O3 -I./ImageStreamIO/src -Wall
LDLIBS = -lImageStreamIO -lm -lportaudio
LDFLAGS = -L./ImageStreamIO/build/lib

TARGETS = dataProcessEX paRead

all: $(TARGETS)

%: %.c
	$(CC) $(CFLAGS) $< $(LDFLAGS) $(LDLIBS) -o $@

clean:
	rm -f $(TARGETS)

