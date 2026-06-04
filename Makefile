# The Guild II Renaissance -- Ultrawide HUD Fix
# Builds the in-place d3d9.dll proxy (32-bit, mingw cross-compiler).

CC      = i686-w64-mingw32-gcc
CFLAGS  = -O2 -Wall -m32
SRC     = src/uwfix.c src/d3d9_proxy.c
OUT     = build/d3d9.dll

.PHONY: all deploy clean

all: $(OUT)

$(OUT): $(SRC) | build
	$(CC) $(CFLAGS) -shared -o $(OUT) $(SRC) -Wl,--kill-at -lkernel32

build:
	mkdir -p build

# Copy the built DLL + default ini into the live game dir ($GUILD2_LIVE_DIR).
deploy: $(OUT)
	scripts/deploy.sh

clean:
	rm -rf build
