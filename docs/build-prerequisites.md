# Shared local and CI prerequisites

The root Dockerfile, CI image, and HAP workflow run the same prerequisite repair:

```sh
bash scripts/ensure_build_dependencies.sh
make test-game-runtime
make NATIVE_ARCH=arm64-v8a hap-unsigned
```

Run the repair as root inside the build container, with source and the Harmony
SDK mounted at the configured paths. It checks actual tools and headers and
installs only missing packages. This also supports an older base image without
relying on packages manually installed in another container. The GNU component
source and final payload identity gates remain required.
