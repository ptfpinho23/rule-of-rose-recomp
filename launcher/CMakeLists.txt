cmake_minimum_required(VERSION 3.21)
project(ror_launcher CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ROR_PACKAGE=ON builds the relocatable binary that recomp/package.sh ships:
# it looks for its libraries in lib/ beside itself and nowhere else.
option(ROR_PACKAGE "Build the launcher for the dist/ package" OFF)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

qt_add_executable(ror_launcher
  main.cpp
  mainwindow.cpp mainwindow.h
  style.cpp style.h
  discinfo.cpp discinfo.h
  gpuprobe.cpp gpuprobe.h
  widgets.cpp widgets.h
  resources.qrc
)
target_link_libraries(ror_launcher PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)

if(WIN32)
  # GUI subsystem: no console window behind the launcher.
  set_target_properties(ror_launcher PROPERTIES WIN32_EXECUTABLE ON)
  # Version information and manifest; the version is the newest one in the
  # changelog. Programs without them look anonymous to Windows and to
  # antivirus heuristics.
  set(ROR_VERSION "0.0.0")
  file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../CHANGELOG.md" ROR_CHANGELOG_HEAD REGEX "^## [0-9]+\\.[0-9]+\\.[0-9]+" LIMIT_COUNT 1)
  if(ROR_CHANGELOG_HEAD MATCHES "([0-9]+\\.[0-9]+\\.[0-9]+)")
    set(ROR_VERSION "${CMAKE_MATCH_1}")
  endif()
  string(REPLACE "." "," ROR_VER "${ROR_VERSION},0")
  set(ROR_VER_STR "${ROR_VERSION}")
  set(ROR_DESC "Rule of Rose PC Port launcher")
  set(ROR_FILE "RuleOfRose.exe")
  set(ROR_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/../release/win/app.manifest")
  configure_file(../release/win/app.rc.in "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)
  enable_language(RC)
  target_sources(ror_launcher PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/app.rc")
else()
  target_link_libraries(ror_launcher PRIVATE ${CMAKE_DL_LIBS})
  # Qt6::Gui's CMake package adds the system OpenGL libraries to the link
  # line. The launcher calls nothing in them, and libOpenGL.so.0 is not
  # installed on every desktop (Debian/Ubuntu: package libopengl0), so a
  # launcher linked against it does not start there. Keep only the
  # libraries that are really used.
  target_link_options(ror_launcher PRIVATE -Wl,--as-needed)
  if(ROR_PACKAGE)
    set(ROR_RPATH "$ORIGIN/lib")
  else()
    set(ROR_RPATH "${ROR_DEV_RPATH}")
  endif()
  set_target_properties(ror_launcher PROPERTIES
    SKIP_BUILD_RPATH OFF
    BUILD_WITH_INSTALL_RPATH ON
    INSTALL_RPATH "${ROR_RPATH}")
endif()
