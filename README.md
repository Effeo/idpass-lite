# Description

```
git clone --recurse-submodules ssh://git@github.com/newlogic42/lab_idpass_lite.git
```

## How to build `libidpasslite.so` only for your local machine

To build `libidpasslite.so` locally in a compatible Linux machine without 
using Docker:

```
cd lab_idpass_lite/
./build.sh desktop
```

The locally built library is: `build/idpass/build.desktop/libidpasslite.so`

## How to build `libidpasslite.so` for all architectures

This uses the Docker container in order to contain the required setup to build
the following Android architectures:

- armeabi-v7a
- arm64-v8a
- x86
- x86_64

```
cd lab_idpass_lite/
./build.sh
```

The Android outputs are in: `build/idpass/jniLibs/`

## Notes

Old `cmake --version` of `3.17` does not support `-S` argument so check your
`cmake` version accordingly. It can still be made to build, but you have to `cd` 
into the folder containing the `CMakeList.txt` and setup manually the needed
environment variables.
