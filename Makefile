PREFIX ?= /usr/local
JOBS ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

.PHONY: all configure build install clean run

all: configure build install

configure:
	cmake -B build -DCMAKE_INSTALL_PREFIX=$(PREFIX)

build:
	cmake --build build -j$(JOBS)

install:
	cmake --install build

clean:
	rm -rf build

run:
	baresip -v
