# Development {#audio_development}

## Building

```bash
export PKG_CONFIG_PATH="$PWD/../../.local/share/pkgconfig"
make -C . test PREFIX="$PWD/../../.local"
```

`PREFIX` is not optional in practice. Without it nothing sets an rpath, and a
test binary dies looking for `libghoti.io-cutil-0.so.0` - and because the link
line has not changed, a later `make` will not relink it for you.

## The gates

`make test` runs `TEST_GATES` first: `check-symbols` and `check-aliasing`.

`check-fixtures` exists and is **not** in `TEST_GATES` yet. It refuses to run
against a tree with no `tests/data`, which is correct - a gate that measures an
empty set reports success, which is worse than no gate. It goes back into
`TEST_GATES` in phase 1, with the first fixtures.

## Both dependency arms

`ghoti.io-image` is optional. Build and test both ways:

```bash
make test PREFIX=...                    # with image, where it is installed
make WITHOUT_IMAGE=1 test PREFIX=...    # without
```

`WITHOUT_IMAGE=1` forces the second arm on a machine that has `image`, which
is what makes it testable at all in CI. The flag stamp notices the change and
rebuilds, so the two do not contaminate each other.

The arm that is not compiled is the arm that rots, and the failure is silent:
the library and its callers end up with different ideas about the build.
`tests/unit/test_core.cpp` has one test for exactly that - it compares
`gaud_have_image_validation()`, compiled into the library, against the
preprocessor's answer in the test, which are two separate compilations.

## A clean tree is a different test

Every compile rule carries `| $(LIBVER_GEN)`, because every translation unit
reaches the generated `libver_gen.h` through `macros.h`. A rule missing it
works on a tree something already built and fails on a fresh clone - and under
`-j` it is a race instead. So the check that matters is:

```bash
make clean && make test PREFIX=...
make clean && make test-asan PREFIX=...
make clean && make -j8 test PREFIX=...
```

This was inherited broken from the Makefile this one was copied from, and
found by building the ASan tree before anything else had run.
