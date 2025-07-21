CC = gcc

CFLAGS = -O3 -I./ImageStreamIO/src -Wall
LDFLAGS = -L./ImageStreamIO/build/lib -lImageStreamIO -lm -lportaudio

TARGET = dataProcessEX paRead

all: $(TARGET)

dataProcessEX: dataProcessEX.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

paRead: paRead.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f $(TARGET)

