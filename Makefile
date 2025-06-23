CC = gcc

CFLAGS = -I./ImageStreamIO/src -Wall
LDFLAGS = -L./ImageStreamIO/build/lib -lImageStreamIO -lm -lusb-1.0 -lportaudio

TARGET = dataProcessEX dataRead paRead

all: $(TARGET)

dataProcessEX: dataProcessEX.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

dataRead: dataRead.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

paRead: paRead.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f $(TARGET)

