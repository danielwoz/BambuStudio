find_package(OpenGL QUIET REQUIRED)

# libtiff 4.1.0's CMakeLists.txt unconditionally adds_subdirectory(tools),
# (test), (contrib). tools/tiffgt links freeglut.lib, which we don't ship in
# our deps tree — and the build for tools/tests is wasted work anyway (we
# only consume libtiff itself). Comment them out via PATCH_COMMAND.
set(_TIFF_DISABLE_EXTRAS_PATCH "${CMAKE_COMMAND}" -E chdir "<SOURCE_DIR>"
    "${CMAKE_COMMAND}" -DSED_TARGET=CMakeLists.txt
    -P "${CMAKE_CURRENT_LIST_DIR}/tiff_disable_extras.cmake")

bambustudio_add_cmake_project(TIFF
    URL https://download.osgeo.org/libtiff/tiff-4.1.0.zip
    URL_HASH SHA256=6F3DBED9D2ECFED33C7192B5C01884078970657FA21B4AD28E3CDF3438EB2419
    DEPENDS ${ZLIB_PKG} ${PNG_PKG} ${JPEG_PKG}
    PATCH_COMMAND ${_TIFF_DISABLE_EXTRAS_PATCH}
    CMAKE_ARGS
        -Dlzma:BOOL=OFF
        -Dwebp:BOOL=OFF
        -Djbig:BOOL=OFF
        -Dzstd:BOOL=OFF
        -Dpixarlog:BOOL=OFF
)
