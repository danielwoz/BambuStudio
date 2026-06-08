set(_wx_toolkit "")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_gtk_ver 2)
    if (DEP_WX_GTK3)
        set(_gtk_ver 3)
    endif ()
    set(_wx_toolkit "-DwxBUILD_TOOLKIT=gtk${_gtk_ver}")
    set(_wx_private_font "-DwxUSE_PRIVATE_FONTS=1")
    set(_wx_egl "-DwxUSE_GLCANVAS_EGL=OFF")
else ()
    set(_wx_egl "")
endif()

if (MSVC)
    set(_wx_edge "-DwxUSE_WEBVIEW_EDGE=ON")
else ()
    set(_wx_edge "-DwxUSE_WEBVIEW_EDGE=OFF")
endif ()

# Force wxWidgets to build against OUR libjpeg-turbo, not whatever find_package
# stumbles onto. Our deps install libjpeg-turbo as `jpeg-static.lib`, but
# CMake's FindJPEG searches for `jpeg`/`libjpeg`, misses it, and falls back to
# Strawberry Perl's libjpeg 9 (C:/Strawberry/c/lib/libjpeg.a, JPEG_LIB_VERSION
# 90). wxJPEGHandler then compiles against the v90 jpeg_decompress_struct while
# the app links our libjpeg-turbo (v62) -> wrong struct offsets -> wxImage JPEG
# decode crashes on every thumbnail (see memory: wx_libjpeg_abi_mismatch).
# Pin the include + lib explicitly so the layouts match.
if (MSVC)
    set(_wx_jpeg
        "-DJPEG_INCLUDE_DIR=${DESTDIR}/usr/local/include"
        "-DJPEG_LIBRARY_RELEASE=${DESTDIR}/usr/local/lib/jpeg-static.lib"
        "-DJPEG_LIBRARY=${DESTDIR}/usr/local/lib/jpeg-static.lib")
else ()
    set(_wx_jpeg "")
endif ()

# if (MSVC)
#     set(_patch_cmd ${PATCH_CMD} ${CMAKE_CURRENT_LIST_DIR}/0001-wxWidget-fix.patch)
# else ()
#     set(_patch_cmd test -f WXWIDGETS_PATCHED || ${PATCH_CMD} ${CMAKE_CURRENT_LIST_DIR}/0001-wxWidget-fix.patch && touch WXWIDGETS_PATCHED)
# endif ()

bambustudio_add_cmake_project(wxWidgets
    GIT_REPOSITORY "https://github.com/bambulab/wxWidgets"
    GIT_TAG master
    DEPENDS ${PNG_PKG} ${ZLIB_PKG} ${EXPAT_PKG} ${TIFF_PKG} ${JPEG_PKG}
    CMAKE_ARGS
        -DwxBUILD_PRECOMP=ON
        ${_wx_toolkit}
        "-DCMAKE_DEBUG_POSTFIX:STRING="
        -DwxBUILD_DEBUG_LEVEL=0
        -DwxBUILD_SAMPLES=OFF
        -DwxBUILD_SHARED=OFF
        -DwxUSE_MEDIACTRL=ON
        -DwxUSE_DETECT_SM=OFF
        -DwxUSE_UNICODE=ON
        ${_wx_private_font}
        -DwxUSE_OPENGL=ON
        -DwxUSE_WEBVIEW=ON
        ${_wx_edge}
        -DwxUSE_WEBVIEW_IE=OFF
        -DwxUSE_REGEX=builtin
        -DwxUSE_LIBMSPACK=OFF
        -DwxUSE_LIBSDL=OFF
        -DwxUSE_XTEST=OFF
        -DwxUSE_STC=OFF
        -DwxUSE_AUI=ON
        -DwxUSE_EXCEPTIONS=OFF
        -DwxUSE_LIBPNG=sys
        -DwxUSE_ZLIB=sys
        -DwxUSE_LIBJPEG=sys
        -DwxUSE_LIBTIFF=sys
        -DwxUSE_EXPAT=sys
        ${_wx_jpeg}
        ${_wx_egl}
)

if (MSVC)
    add_debug_dep(dep_wxWidgets)
endif ()
