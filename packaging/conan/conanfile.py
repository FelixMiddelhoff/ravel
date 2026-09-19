import os

from conan import ConanFile
from conan.tools.build import check_min_cppstd
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import copy, rmdir


class RavelConan(ConanFile):
    name = "ravel"
    version = "0.3.0"
    license = "MIT"
    url = "https://github.com/FelixMiddelhoff/ravel"
    homepage = "https://github.com/FelixMiddelhoff/ravel"
    description = (
        "Deterministic simulation testing for C++20: seeded scheduling, virtual time, "
        "network and disk faults, shrinking and replay."
    )
    topics = ("testing", "deterministic-simulation-testing", "fault-injection", "cpp20")

    # The C++ API lives in the static library. The shared library only carries
    # the C ABI, so there is no shared option.
    package_type = "static-library"
    settings = "os", "arch", "compiler", "build_type"

    def export_sources(self):
        # This recipe lives in packaging/conan; the sources are two levels up.
        repository = os.path.join(self.recipe_folder, "..", "..")
        for pattern in ("CMakeLists.txt", "cmake/*", "include/*", "src/*", "LICENSE"):
            copy(self, pattern, repository, self.export_sources_folder)

    def validate(self):
        check_min_cppstd(self, 20)

    def layout(self):
        cmake_layout(self)

    def generate(self):
        toolchain = CMakeToolchain(self)
        toolchain.variables["RAVEL_BUILD_TESTS"] = False
        toolchain.variables["RAVEL_BUILD_BENCH"] = False
        toolchain.variables["RAVEL_BUILD_EXAMPLES"] = False
        toolchain.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        copy(self, "LICENSE", self.source_folder, os.path.join(self.package_folder, "licenses"))
        CMake(self).install()
        # Conan generates its own CMake files for consumers.
        rmdir(self, os.path.join(self.package_folder, "lib", "cmake"))

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "ravel")
        self.cpp_info.set_property("cmake_target_name", "ravel::ravel")
        self.cpp_info.libs = ["ravel"]
        if self.settings.os in ("Linux", "FreeBSD"):
            self.cpp_info.system_libs = ["pthread"]
