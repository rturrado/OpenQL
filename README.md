#  OpenQL Framework
Fork of [QuTech-Delft/OpenQL](https://github.com/QuTech-Delft/OpenQL), a QuTech/TU Delft framework for high-level quantum programming in C++/Python.

This fork is playing with:
- Package managers (FetchContent, conan...).
- C++23.
- CMake 3.20.

## Build

```
~/projects/OpenQL> conan install . -if=cmake-build-unixlike-gcc-debug-tests -pr=conan/profiles/unixlike-gcc-debug-tests -b=missing -e:b OPENQL_BUILD_TESTS=ON
~/projects/OpenQL> conan build -b -c -bf=cmake-build-unixlike-gcc-debug-tests .
```
