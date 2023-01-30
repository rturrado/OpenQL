from conans import ConanFile, CMake, tools


class OpenQLConan(ConanFile):
    settings = "os", "compiler", "build_type", "arch"
    generators = "cmake"

    def requirements(self):
        self.requires("backward-cpp/1.6")
        self.requires("cimg/3.2.0")
        self.requires("coin-lemon/1.3.1")
        self.requires("doctest/2.4.9")
        self.requires("eigen/3.4.0")
        self.requires("nlohmann_json/3.11.2")
        if tools.get_env("OPENQL_BUILD_TESTS", True):
            self.requires("gtest/1.12.1")

    def _configure_cmake(self):
        cmake = CMake(self)
        cmake.definitions["ASAN_ENABLED"] = "ON" if tools.get_env("OPENQL_BUILD_TESTS", True) else "OFF"
        cmake.definitions["OPENQL_BUILD_TESTS"] = "ON" if tools.get_env("OPENQL_BUILD_TESTS", True) else "OFF"
        cmake.configure()
        return cmake

    def build(self):
        cmake = self._configure_cmake()
        cmake.build()
