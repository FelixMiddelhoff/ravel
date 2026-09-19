# ravel exports its C++ API from a static library only (the shared library
# carries just the C ABI), so a static build is the one that is useful here.
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO FelixMiddelhoff/ravel
    REF "v${VERSION}"
    SHA512 c38a62cbe16f9d60e0864f9c4e076c9aa9cb9e887e56f445363d16e0a911e6c66ceae0855c2c95719ab63cdeefaec4ebb3e85a6f6743bb215979240fc9219c6b
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
