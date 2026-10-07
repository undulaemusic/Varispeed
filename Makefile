# Command-line tools and the bridge. The driver is built by scripts/build_driver.sh.
CC      = clang
CFLAGS  = -O2 -Wall -arch arm64 -arch x86_64 -mmacosx-version-min=12.0
LIBS    = -framework CoreAudio -framework CoreFoundation
LSR     = third_party/libsamplerate
LSR_SRC = $(LSR)/samplerate.c $(LSR)/src_linear.c $(LSR)/src_sinc.c $(LSR)/src_zoh.c

TOOLS = build/varispeedctl build/vstest build/vssweep build/vsaggtest build/vsbridge

all: $(TOOLS)

build/%: tools/%.c tools/vsdevice.h driver/BlackHole/VarispeedProperties.h | build
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

build/vsbridge: bridge/vsbridge_cli.c bridge/VSBridge.c bridge/VSBridge.h bridge/VSRecorder.c bridge/VSRecorder.h $(LSR_SRC) | build
	$(CC) $(CFLAGS) -DHAVE_CONFIG_H -I$(LSR) -o $@ bridge/vsbridge_cli.c bridge/VSBridge.c bridge/VSRecorder.c $(LSR_SRC) $(LIBS)

build:
	mkdir -p build

clean:
	rm -f $(TOOLS)

.PHONY: all clean
