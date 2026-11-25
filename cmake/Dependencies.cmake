include(FetchContent)
set(PET_WX_SOURCE "" CACHE PATH "Optional local wxWidgets source checkout")
set(wxBUILD_SHARED OFF CACHE BOOL "" FORCE)
# Static CRT as well (CMAKE_MSVC_RUNTIME_LIBRARY in CMakeLists.txt): the exe needs no
# Visual C++ redistributable.
set(wxBUILD_USE_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
set(wxBUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(wxBUILD_TESTS OFF CACHE BOOL "" FORCE)
set(wxBUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(wxBUILD_INSTALL OFF CACHE BOOL "" FORCE)
set(wxBUILD_PRECOMP OFF CACHE BOOL "" FORCE)
# DETECT_SM, UIACTIONSIMULATOR and XTEST would link libSM, libICE and libXtst, which a
# Wayland-only system may lack (an AppImage then fails to start).
foreach(feature WEBVIEW WEBREQUEST MEDIACTRL RICHTEXT STC AUI RIBBON PROPGRID SVG LIBSDL
                DETECT_SM UIACTIONSIMULATOR XTEST)
  set(wxUSE_${feature} OFF CACHE BOOL "" FORCE)
endforeach()
foreach(feature LIBJPEG LIBTIFF LIBWEBP NANOSVG)
  set(wxUSE_${feature} OFF CACHE STRING "" FORCE)
endforeach()
# PNG (and the zlib it needs) for F6 screenshots and the embedded icons. Windows uses
# wx's own copies; Linux the system's, which GTK needs anyway and the Flatpak runtime
# provides (wx's bundled zlib does not build with GCC 14).
if(WIN32)
  set(wx_image_libs builtin)
else()
  set(wx_image_libs sys)
endif()
foreach(feature LIBPNG ZLIB)
  set(wxUSE_${feature} ${wx_image_libs} CACHE STRING "" FORCE)
endforeach()
set(wxUSE_GLCANVAS ON CACHE BOOL "" FORCE)
if(NOT WIN32)
  set(wxBUILD_TOOLKIT gtk3 CACHE STRING "" FORCE)
  set(wxUSE_GLCANVAS_EGL ON CACHE BOOL "" FORCE)
  set(wxUSE_WAYLAND ON CACHE BOOL "" FORCE)
endif()
if(PET_WX_SOURCE)
  add_subdirectory("${PET_WX_SOURCE}" "${CMAKE_BINARY_DIR}/_deps/wx-build" EXCLUDE_FROM_ALL)
  set(PET_WX_LICENCE "${PET_WX_SOURCE}/docs/licence.txt")
else()
  # Always the latest wxWidgets release; tools/build-linux-appimage.sh pins the same file.
  FetchContent_Declare(wx
    URL https://github.com/wxWidgets/wxWidgets/releases/download/v3.3.4/wxWidgets-3.3.4.tar.bz2
    URL_HASH SHA256=815c16cafa4fcbfcf25c5c0a3418309c62257b8a3b6f91d85a1367a134f98c98
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(wx)
  set(PET_WX_LICENCE "${wx_SOURCE_DIR}/docs/licence.txt")
endif()
