# ravel exports its C++ API from a static library only (the shared library
# carries just the C ABI), so a static build is the one that is useful here.
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO FelixMiddelhoff/ravel
    REF "v${VERSION}"
    SHA512 d4e344731e936bbf5325c68847f4d2ba1fee10111000a4952f01c670f661ee00eb7a57d8c5789f3425dd4301c115669b52f5a4d0296330e448fa6c051c8fd7ae
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
