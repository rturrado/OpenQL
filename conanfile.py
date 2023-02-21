import os
from conans import ConanFile, tools
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout


class OpenQLConan(ConanFile):
    name = "OpenQL"
    version = "0.1"

    settings = "os", "compiler", "build_type", "arch"

    def requirements(self):
        self.requires("backward-cpp/1.6")
        self.requires("cimg/3.2.0")
        self.requires("coin-lemon/1.3.1")
        self.requires("doctest/2.4.9")
        self.requires("eigen/3.4.0")
        self.requires("libqasm/0.1")
        self.requires("nlohmann_json/3.11.2")
        self.requires("tree-gen/0.1")
        if tools.get_env("OPENQL_BUILD_TESTS", True):
            self.requires("gtest/1.12.1")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        tc = CMakeToolchain(self)
        tc.variables["ASAN_ENABLED"] = "ON" if tools.get_env("OPENQL_BUILD_TESTS", True) else "OFF"
        tc.variables["OPENQL_BUILD_TESTS"] = "ON" if tools.get_env("OPENQL_BUILD_TESTS", True) else "OFF"
        tc.variables["TREE-GEN_BINARY_PATH"] = os.path.join(self.dependencies["tree-gen"].package_folder, "bin", "tree-gen")
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
