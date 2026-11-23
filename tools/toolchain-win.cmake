# toolchain-win.cmake — cross-build the launcher for Windows x64 from Linux.
#   tools/llvm-mingw                        llvm-mingw (clang 18, x86_64-w64-mingw32, UCRT)
#   ~/.local/opt/qt6/6.8.2/llvm-mingw_64    Qt 6.8.2 for Windows (win64_llvm_mingw), from
#       python3 -m aqt install-qt windows desktop 6.8.2 win64_llvm_mingw \
#               -O ~/.local/opt/qt6 --archives qtbase
#   ~/.local/opt/qt6/6.8.2/gcc_64           the Linux Qt: supplies moc/rcc/uic (QT_HOST_PATH)
get_filename_component(ROR_REPO "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(ROR_HOME "$ENV{HOME}")
set(ROR_MINGW "${ROR_REPO}/tools/llvm-mingw")
set(ROR_QTWIN "${ROR_HOME}/.local/opt/qt6/6.8.2/llvm-mingw_64")
set(ROR_QTHOST "${ROR_HOME}/.local/opt/qt6/6.8.2/gcc_64")
# overridable from the environment (tools/build-win.sh exports them): LLVM_MINGW, QT_WIN, QT_LINUX
if(DEFINED ENV{LLVM_MINGW})
  set(ROR_MINGW "$ENV{LLVM_MINGW}")
endif()
if(DEFINED ENV{QT_WIN})
  set(ROR_QTWIN "$ENV{QT_WIN}")
endif()
if(DEFINED ENV{QT_LINUX})
  set(ROR_QTHOST "$ENV{QT_LINUX}")
endif()

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER   ${ROR_MINGW}/bin/x86_64-w64-mingw32-clang)
set(CMAKE_CXX_COMPILER ${ROR_MINGW}/bin/x86_64-w64-mingw32-clang++)
set(CMAKE_RC_COMPILER  ${ROR_MINGW}/bin/x86_64-w64-mingw32-windres)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_FIND_ROOT_PATH ${ROR_QTWIN} ${ROR_MINGW}/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

set(CMAKE_PREFIX_PATH ${ROR_QTWIN})
set(QT_HOST_PATH ${ROR_QTHOST})
set(QT_HOST_PATH_CMAKE_DIR ${QT_HOST_PATH}/lib/cmake)
