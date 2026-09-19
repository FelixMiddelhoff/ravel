# ravel exports its C++ API from a static library only (the shared library
# carries just the C ABI), so a static build is the one that is useful here.
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO FelixMiddelhoff/ravel
    REF "v${VERSION}"
    SHA512 d0fa5aef71805f762cdf5648b2a975d5f3e548e9ea35178d4da45b6b2c71519032085c92c1c6292c18e6f4a03f07eec4cdbbd51fc14e11a843332633083aa9e9
    HEAD_REF master
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DRAVEL_BUILD_TESTS=OFF
        -DRAVEL_BUILD_BENCH=OFF
        -DRAVEL_BUILD_EXAMPLES=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME ravel CONFIG_PATH lib/cmake/ravel)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
