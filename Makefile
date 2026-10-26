install-xbuild:
	cargo install --path ./patches/xbuild/xbuild --force

# BUILD_ID=<name> labels the build (see guest/build-overlay.sh).
guest-overlay:
	BUILD_ID=$(BUILD_ID) sh guest/build-overlay.sh

build: install-xbuild guest-overlay
	x build --release --platform android --arch arm64 --format apk

build-docker: guest-overlay
	DOCKER_BUILDKIT=1 docker build --target artifact --output type=local,dest=./target .
