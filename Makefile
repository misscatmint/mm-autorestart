.PHONY: all build check lint dist clean

all: build

# Run as the host user so build/ and output/ aren't root-owned.
build:
	docker compose run --rm --build --user "$$(id -u):$$(id -g)" build

check: lint build

lint:
	clang-tidy src/autorestart.cpp

dist: build
	rm -rf dist
	mkdir dist
	cd output/mm-1.12 && zip -qr ../../dist/autorestart-linux-mm-1.12.zip .
	cd output/mm-2.0 && zip -qr ../../dist/autorestart-linux-mm-2.0.zip .

clean:
	rm -rf build output dist
