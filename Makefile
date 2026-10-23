install-xbuild:
	cargo install --path ./patches/xbuild/xbuild --force

build: install-xbuild
	x build --release --platform android --arch arm64 --format apk

build-docker:
	DOCKER_BUILDKIT=1 docker build --target artifact --output type=local,dest=./target .
