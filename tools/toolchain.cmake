# toolchain.cmake — self-contained Linux build of the Rule of Rose launcher.
# The host has no system C++ toolchain (no-root WSL2). Everything comes from
# user-space downloads, all in durable locations (nothing under /tmp):
#   ~/.local/opt/clang               clang 18.1.8 + lld + libc++ (headers & runtime)
#   ~/.local/opt/qt6/6.8.2/gcc_64    Qt 6.8.2
#   tools/debs/extract/usr           glibc headers, CRT objects, libgcc 14
#                                    (shared with the game build, recomp/build.sh)
#   tools/debs/qtdev/usr             kernel, GL/EGL (GLVND) and xkbcommon headers
#   tools/debs/xcbrt/usr             run-time libs for Qt's xcb platform plugin
#   tools/debs/linklibs              linker scripts written by this file
# qtdev and xcbrt are created by tools/fetch-debs.sh.
get_filename_component(ROR_REPO "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(ROR_HOME "$ENV{HOME}")
set(ROR_CLANG "${ROR_HOME}/.local/opt/clang")

set(CMAKE_C_COMPILER   ${ROR_CLANG}/bin/clang)
set(CMAKE_CXX_COMPILER ${ROR_CLANG}/bin/clang++)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(QTDIR   ${ROR_HOME}/.local/opt/qt6/6.8.2/gcc_64)
set(GLIBC   ${ROR_REPO}/tools/debs/extract/usr)
set(QTDEV   ${ROR_REPO}/tools/debs/qtdev/usr)
set(GLINC   ${GLIBC}/include)
set(GLIB    ${GLIBC}/lib/x86_64-linux-gnu)
set(GCCRT   ${GLIBC}/lib/gcc/x86_64-linux-gnu/14)
set(DEVINC  ${QTDEV}/include)
set(SYSLIB  /usr/lib/x86_64-linux-gnu)
set(LIBCXX  ${ROR_CLANG}/lib/x86_64-unknown-linux-gnu)
set(ROR_XCBRT ${ROR_REPO}/tools/debs/xcbrt/usr/lib/x86_64-linux-gnu)

if(NOT EXISTS ${DEVINC}/linux/limits.h OR NOT EXISTS ${ROR_XCBRT}/libxcb-cursor.so.0)
  message(FATAL_ERROR "tools/debs/qtdev or tools/debs/xcbrt is missing - run tools/fetch-debs.sh")
endif()

# Where the launcher finds its shared libraries when run from the build tree
# (launcher/CMakeLists.txt turns this into the RUNPATH; the packaged build
# uses $ORIGIN/lib instead).
set(ROR_DEV_RPATH "${QTDIR}/lib;${LIBCXX};${ROR_XCBRT}")

# Compile flags: glibc + kernel + GL + xkbcommon headers.
set(ROR_SYSINC "-isystem ${GLINC} -isystem ${GLINC}/x86_64-linux-gnu -isystem ${DEVINC} -isystem ${DEVINC}/x86_64-linux-gnu")
set(CMAKE_C_FLAGS_INIT   "${ROR_SYSINC}")
# libc++ headers must come before the C library headers.
set(CMAKE_CXX_FLAGS_INIT "-stdlib=libc++ -isystem ${ROR_CLANG}/include/x86_64-unknown-linux-gnu/c++/v1 -isystem ${ROR_CLANG}/include/c++/v1 ${ROR_SYSINC}")

# Link flags. The libc.so/libm.so/libgcc_s.so "libraries" in the extracted
# -dev packages are linker scripts with absolute /usr/lib paths to files this
# host does not have (libc_nonshared.a ...), so write our own into
# tools/debs/linklibs (regenerated on every configure; cheap and always
# matches where the repo lives). CRT objects come from the glibc tree,
# crtbeginS/libgcc from gcc 14, the shared objects from the system.
set(LINKLIBS ${ROR_REPO}/tools/debs/linklibs)
file(MAKE_DIRECTORY ${LINKLIBS})
file(WRITE ${LINKLIBS}/libc.so "GROUP ( ${SYSLIB}/libc.so.6 ${GLIB}/libc_nonshared.a AS_NEEDED ( /lib64/ld-linux-x86-64.so.2 ) )\n")
file(WRITE ${LINKLIBS}/libm.so "GROUP ( ${SYSLIB}/libm.so.6 )\n")
file(WRITE ${LINKLIBS}/libgcc_s.so "GROUP ( ${SYSLIB}/libgcc_s.so.1 ${GCCRT}/libgcc.a )\n")
file(WRITE ${LINKLIBS}/libatomic.so "GROUP ( ${SYSLIB}/libatomic.so.1 )\n")
file(WRITE ${LINKLIBS}/libdl.so "GROUP ( ${SYSLIB}/libdl.so.2 )\n")
file(WRITE ${LINKLIBS}/libpthread.so "GROUP ( ${SYSLIB}/libpthread.so.0 )\n")
file(WRITE ${LINKLIBS}/librt.so "GROUP ( ${SYSLIB}/librt.so.1 )\n")
set(ROR_LINK "-fuse-ld=lld -B${GLIB} -B${GCCRT} -L${LINKLIBS} -L${GCCRT} -L${LIBCXX} -Wl,-rpath-link,${SYSLIB}")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "${ROR_LINK}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${ROR_LINK}")

# Qt6Gui's CMake package requires WrapOpenGL; the stock FindOpenGL cannot see
# our GLVND headers, so provide the imported target ourselves.
set(OPENGL_INCLUDE_DIR ${DEVINC})
set(OPENGL_opengl_LIBRARY ${SYSLIB}/libGL.so.1)
set(OPENGL_glx_LIBRARY ${SYSLIB}/libGL.so.1)
set(OPENGL_gl_LIBRARY ${SYSLIB}/libGL.so.1)
if(NOT TARGET OpenGL::GL)
  add_library(OpenGL::GL UNKNOWN IMPORTED)
  set_target_properties(OpenGL::GL PROPERTIES
    IMPORTED_LOCATION ${SYSLIB}/libGL.so.1
    INTERFACE_INCLUDE_DIRECTORIES ${DEVINC})
endif()
set(WrapOpenGL_FOUND TRUE)

set(CMAKE_PREFIX_PATH ${QTDIR})
set(Qt6_DIR ${QTDIR}/lib/cmake/Qt6)
