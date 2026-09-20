# ravel exports its C++ API from a static library only (the shared library
# carries just the C ABI), so a static build is the one that is useful here.
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO FelixMiddelhoff/ravel
    REF "v${VERSION}"
    SHA512 bdbd5d27ca34e60dd54ae108201c65f8d208c11e454c03aa78f5557ab4adacb88346651d3f1b91d0945567116a817f9877523a438c9d350b6514ec64c8a6f503
    HEAD_REF master
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DRAVEL_BUILD_TESTS=OFF
        -DRAVEL_BUILD_BENCH=OFF
        -DRAVEL_BUILD_EXAMPLES=OFF
        -DRAVEL_BUILD_FUZZ=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME ravel CONFIG_PATH lib/cmake/ravel)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
