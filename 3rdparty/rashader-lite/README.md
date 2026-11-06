# rashader-lite

A small, dependency-free rashader implementation (bring your own glsl compiler and runtime)

rashader-lite handles parsing of slang/slangp to glsl, parsing of the resulting SPIR-V, and planning an optimal mapping of passes to render targets.

glslang is required to build the test executable, but not for the main library, so you can use whatever GLSL compiler you prefer.

## Building

```sh
cmake -B build
make -C build
```

## Running Tests

```sh
CHECKOUT=1 test/test.sh build/rashader-lite-tests
```
