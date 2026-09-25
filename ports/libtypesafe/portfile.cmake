# Overlay port that builds this checkout:
#   vcpkg install libtypesafe --overlay-ports=<repo>/ports
# vcpkg caches by port files, not source, so pass --no-binarycaching after changing the source.
# For a registry, replace SOURCE_PATH with vcpkg_from_github(...) once a release is tagged.
get_filename_component(SOURCE_PATH "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DLIBTYPESAFE_BUILD_TESTS=OFF
        -DLIBTYPESAFE_BUILD_EXAMPLES=OFF
        -DLIBTYPESAFE_BUILD_API_CHECKS=OFF
        -DLIBTYPESAFE_FETCH_DEPS=OFF
        -DLIBTYPESAFE_INSTALL=ON
        -DLIBTYPESAFE_WITH_CURL=ON
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/libtypesafe)

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
    "${CURRENT_PACKAGES_DIR}/share/licenses")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
