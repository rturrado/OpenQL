#  OpenQL Framework

Fork of [QuTech-Delft/OpenQL](https://github.com/QuTech-Delft/OpenQL),
a QuTech/TU Delft framework for high-level quantum programming in C++/Python.

This fork is playing with:
- Conan package manager.
- C++23.
- CMake 3.20.

The following dependencies are now installed as conan packages:
- `Backward`.
- `cimg`.
- `doctest`.
- `Eigen3`.
- `LEMON`.
- `libqasm`.
- `nlohmann_json`.

The following dependencies are still installed as git submodules:
- `highs`.
- `tree-gen`.

## Build

```
~/projects/OpenQL> OPENQL_BUILD_TESTS=ON conan install . -b=missing
~/projects/OpenQL> conan build -b -c .
```

You can also use a profile.
Notice that you should not specify the build folder as a command line option
since that information is already provided by the `layout()` method at `conanfile.py`.
Should you need to use a different build folder, you could set the `build_folder` argument in `cmake_layout()`.

```
~/projects/OpenQL> OPENQL_BUILD_TESTS=ON conan install . -pr=conan/profiles/unixlike-gcc-debug-tests -b=missing
~/projects/OpenQL> conan build -b -c .
```
