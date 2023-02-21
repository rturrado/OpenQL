#  OpenQL Framework
Fork of [QuTech-Delft/OpenQL](https://github.com/QuTech-Delft/OpenQL), a QuTech/TU Delft framework for high-level quantum programming in C++/Python.

This fork is playing with:
- Conan package manager.
- C++23.
- CMake 3.20.

## Build

```
~/projects/OpenQL> conan install . -b=missing -e:b OPENQL_BUILD_TESTS=ON
~/projects/OpenQL> conan build -b -c .
```

## Issues

### Installation without `libqasm` package

`conan install` should fail whenever it needs to build `libqasm` (i.e. whenever it cannot retrieve the `libqasm` package from a conan center).

For example, the `conan install` line below fails to build `libqasm`:

```
~/projects/OpenQL> conan install . -if=cmake-build-unixlike-gcc-debug-tests -pr=conan/profiles/unixlike-gcc-debug-tests -b=missing -e:b OPENQL_BUILD_TESTS=ON
~/projects/OpenQL> conan build -b -c -bf=cmake-build-unixlike-gcc-debug-tests .
```

The error log shows:

```
libqasm/0.1: CMake command: cmake --build "/home/rturrado/.conan/data/libqasm/0.1/_/_/build/f724db7bf24bc0c3a98ffb6021ec8c41605025a7/build/Debug" '--' '-j16'
[  2%] Building CXX object src/cqasm/func-gen/CMakeFiles/func-gen.dir/func-gen.cpp.o
[  5%] Linking CXX executable func-gen
[  5%] Built target func-gen
make[2]: *** No rule to make target 'src/cqasm/tree-gen', needed by '/home/rturrado/.conan/data/libqasm/0.1/_/_/build/f724db7bf24bc0c3a98ffb6021ec8c41605025a7/src/cqasm/src/cqasm-v1-values.tree'.  Stop.
make[1]: *** [CMakeFiles/Makefile2:116: src/cqasm/CMakeFiles/cqasm_objlib.dir/all] Error 2
make: *** [Makefile:136: all] Error 2
libqasm/0.1: 
libqasm/0.1: ERROR: Package 'f724db7bf24bc0c3a98ffb6021ec8c41605025a7' build failed
libqasm/0.1: WARN: Build folder /home/rturrado/.conan/data/libqasm/0.1/_/_/build/f724db7bf24bc0c3a98ffb6021ec8c41605025a7/build/Debug
ERROR: libqasm/0.1: Error in build() method, line 42
	cmake.build()
	ConanException: Error 2 while executing cmake --build "/home/rturrado/.conan/data/libqasm/0.1/_/_/build/f724db7bf24bc0c3a98ffb6021ec8c41605025a7/build/Debug" '--' '-j16'
```

Notice there is a rule:

```
/home/rturrado/.conan/data/libqasm/0.1/_/_/build/f724db7bf24bc0c3a98ffb6021ec8c41605025a7/src/cqasm/src/cqasm-v1-types.tree: src/cqasm/tree-gen
```

in:

```
/home/rturrado/.conan/data/libqasm/0.1/_/_/build/f724db7bf24bc0c3a98ffb6021ec8c41605025a7/build/Debug/src/cqasm/CMakeFiles/cqasm_objlib.dir/build.make
```

I do not know yet if this has to do with the way `flex/bison` is used in `libqasm` or with the generation of source files through `generate_tree`.

### Tests are always built

From the last tests I did, it seems tests are built regardless the environment variable `OPENQL_BUILD_TESTS` is used or not.

This may have to do with the management of that variable in `conanfile.py` or with the running of conan commands without resetting the CMake cache.