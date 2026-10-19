# PC build (SDL2). Switch build: see Makefile.switch. Audio needs SDL2_mixer; without it the build is silent.
CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
HAVE_MIXER := $(shell pkg-config --exists SDL2_mixer && echo 1)
ifeq ($(HAVE_MIXER),1)
AUDIO   := src/audio.c
PKGS    := sdl2 SDL2_mixer
else
AUDIO   := src/audio_stub.c
PKGS    := sdl2
endif
CFLAGS  += $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -lz -lm
SRC     := src/cpu.c src/hle.c src/native.c src/apk.c src/sha256.c $(AUDIO) src/main.c

destinia: $(SRC) src/guest.h src/game.h src/apk.h src/audio.h src/jni_names.h
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDLIBS)

clean:
	rm -f destinia
.PHONY: clean

# Differential instruction test (tools/isa_diff.py): single-step interpreter binary, no game needed
isatest: src/cpu.c src/isatest.c src/guest.h
	$(CC) -O1 -g -Wall -DISA_TEST src/cpu.c src/isatest.c -o $@ -lz
