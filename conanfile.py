import os

from conan import ConanFile
from conan.tools.build import check_min_cppstd
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import rmdir

required_conan_version = ">=2.0"


class LibtypesafeConan(ConanFile):
    name = "libtypesafe"
    version = "0.1.0"
    license = "MIT"
    url = "https://github.com/jasonduncan/libtypesafe"
    homepage = "https://github.com/jasonduncan/libtypesafe"
    description = "Unofficial C++17 client for the TypeSafe AI API: typed answers, no exceptions required, non-blocking calls."
    topics = ("typesafe", "ai", "classification", "http-client")
    package_type = "static-library"
    settings = "os", "arch", "compiler", "build_type"
    options = {"with_curl": [True, False], "fPIC": [True, False]}
    default_options = {"with_curl": True, "fPIC": True}
    exports_sources = "CMakeLists.txt", "LICENSE", "cmake/*", "include/*", "src/*"

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def layout(self):
        cmake_layout(self)

    def requirements(self):
        # Json appears in the public headers.
        self.requires("nlohmann_json/3.11.3", transitive_headers=True)
        if self.options.with_curl:
            self.requires("libcurl/[>=8.6 <9]")

    def validate(self):
        check_min_cppstd(self, 17)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.cache_variables["LIBTYPESAFE_BUILD_TESTS"] = False
        tc.cache_variables["LIBTYPESAFE_BUILD_EXAMPLES"] = False
        tc.cache_variables["LIBTYPESAFE_BUILD_API_CHECKS"] = False
        tc.cache_variables["LIBTYPESAFE_FETCH_DEPS"] = False
        tc.cache_variables["LIBTYPESAFE_INSTALL"] = True
        tc.cache_variables["LIBTYPESAFE_WITH_CURL"] = bool(self.options.with_curl)
        tc.generate()
        CMakeDeps(self).generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        CMake(self).install()
        # Conan generates its own package config for consumers.
        rmdir(self, os.path.join(self.package_folder, "lib", "cmake"))
        rmdir(self, os.path.join(self.package_folder, "share", "cmake"))
        rmdir(self, os.path.join(self.package_folder, "share", "pkgconfig"))

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "libtypesafe")
        self.cpp_info.set_property("cmake_target_name", "libtypesafe::libtypesafe")
        self.cpp_info.libs = ["typesafe"]
        self.cpp_info.requires = ["nlohmann_json::nlohmann_json"]
        if self.options.with_curl:
            self.cpp_info.requires.append("libcurl::libcurl")
