# splashd Development Notes

## Build

```sh
make
```

The binary is written to:

```text
build/splashd
```

## Test

```sh
make test
```

## MiSTer Cross Build

The current deployment build uses an ARM hard-float toolchain with `libz` and
`libturbojpeg`:

```sh
docker run --rm -v "$PWD":/work -w /work debian:bullseye sh -lc '
  dpkg --add-architecture armhf &&
  apt-get update >/dev/null &&
  apt-get install -y --no-install-recommends \
    g++-arm-linux-gnueabihf zlib1g-dev:armhf libturbojpeg0-dev:armhf \
    binutils-arm-linux-gnueabihf file make >/dev/null &&
  make clean &&
  make CXX=arm-linux-gnueabihf-g++ LDFLAGS="-static-libstdc++ -static-libgcc" &&
  arm-linux-gnueabihf-strip build/splashd &&
  file build/splashd &&
  arm-linux-gnueabihf-readelf -d build/splashd | grep NEEDED
'
```

Expected runtime dependencies on MiSTer:

```text
libz.so.1
libturbojpeg.so.0
libc.so.6
ld-linux-armhf.so.3
```

## Installer Testing

`scripts/install.sh` supports fixture testing with:

```sh
SPLASHD_FAT_DIR=/tmp/fake-fat SPLASHD_NO_RESTART=1 scripts/install.sh
```

`SPLASHD_NO_RESTART=1` is only for tests; normal MiSTer installs restart
`splashd`.
