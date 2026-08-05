cmake_minimum_required(VERSION 3.16)

project(evio_ejfat_replay
        VERSION 1.0.0
        DESCRIPTION "Replay EVIO captures through an EJFAT load balancer"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Handy for clangd / IDE integration; costs nothing.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(PETSRO_BUILD_TESTS "Build the unit tests" ON)
option(PETSRO_WERROR "Treat warnings as errors" OFF)

# ---------------------------------------------------------------------------
# Warnings
#
# Every warning below is fixed rather than suppressed. If a suppression ever
# becomes necessary it belongs here, with a comment saying why.
# ---------------------------------------------------------------------------
add_library(petsro_warnings INTERFACE)
if(MSVC)
  target_compile_options(petsro_warnings INTERFACE /W4)
else()
  target_compile_options(petsro_warnings INTERFACE
      -Wall -Wextra -Wpedantic
      -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wnon-virtual-dtor)
endif()
if(PETSRO_WERROR)
  target_compile_options(petsro_warnings INTERFACE
      $<IF:$<CXX_COMPILER_ID:MSVC>,/WX,-Werror>)
endif()

# ---------------------------------------------------------------------------
# Dependencies
#
#  Boost.program_options -- command line parsing. Same library e2sar_root.cpp
#      uses. Located by CMake's FindBoost / BoostConfig; set BOOST_ROOT if it
#      lives somewhere unusual.
#
#  Threads -- required by E2SAR and by the standard library's <thread>.
#
#  E2SAR -- EJFAT segmentation and transmission. Located via pkg-config, using
#      the same e2sar.pc that e2sar-utils/meson.build consumes, so
#      PKG_CONFIG_PATH set up for that project works here unchanged. Optional:
#      when it is absent the program still builds and can run --dry-run, and
#      all tests still run. See PETSRO_ENABLE_EJFAT below.
# ---------------------------------------------------------------------------
find_package(Boost REQUIRED COMPONENTS program_options)
find_package(Threads REQUIRED)

find_package(PkgConfig QUIET)

set(PETSRO_ENABLE_EJFAT OFF)
if(PkgConfig_FOUND)
  pkg_check_modules(E2SAR IMPORTED_TARGET e2sar)
  if(E2SAR_FOUND)
    set(PETSRO_ENABLE_EJFAT ON)
  endif()
endif()

if(PETSRO_ENABLE_EJFAT)
  message(STATUS "E2SAR found (version ${E2SAR_VERSION}); EJFAT sending enabled")
else()
  message(WARNING
      "E2SAR not found via pkg-config; building WITHOUT EJFAT transmission.\n"
      "   The binary will still read, synchronize and account for packets under --dry-run,\n"
      "   and the full test suite still runs. To enable sending, put e2sar.pc on\n"
      "   PKG_CONFIG_PATH, e.g.:\n"
      "     export PKG_CONFIG_PATH=/path/to/e2sar/lib/pkgconfig:$PKG_CONFIG_PATH")
endif()

# ---------------------------------------------------------------------------
# Core library -- everything except main(), so the tests can link it
# ---------------------------------------------------------------------------
add_library(evio_ejfat_replay_core STATIC
    src/EvioFileReader.cpp
    src/EvioEventView.cpp
    src/EventSynchronizer.cpp
    src/EjfatSender.cpp
    src/ReplayLoop.cpp
    src/ReplayStats.cpp
    src/TimestampRebaser.cpp
    src/SignalHandler.cpp
    src/Logging.cpp)

target_include_directories(evio_ejfat_replay_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(evio_ejfat_replay_core
    PUBLIC Threads::Threads
    PRIVATE petsro_warnings)

if(PETSRO_ENABLE_EJFAT)
  target_compile_definitions(evio_ejfat_replay_core PUBLIC PETSRO_HAVE_E2SAR)
  # PUBLIC: e2sar pulls in Boost.Log and gRPC symbols that the final link needs.
  target_link_libraries(evio_ejfat_replay_core PUBLIC PkgConfig::E2SAR)
  # libe2sar.a is static, so pkg-config's plain Libs (-le2sar) omits the Boost
  # libraries (log, thread, url, chrono, ...) it links against internally --
  # those only show up in Libs.private. Pull those in too, or anything that
  # calls into e2sar fails to link with dozens of undefined boost symbols.
  target_link_libraries(evio_ejfat_replay_core PUBLIC ${E2SAR_STATIC_LDFLAGS})
endif()

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
add_executable(evio_ejfat_replay src/main.cpp)

target_link_libraries(evio_ejfat_replay
    PRIVATE evio_ejfat_replay_core
            Boost::program_options
            petsro_warnings)

install(TARGETS evio_ejfat_replay RUNTIME DESTINATION bin)

# ---------------------------------------------------------------------------
# Receiver
#
# Needs the E2SAR Reassembler, so unlike the sender there is no useful
# degraded mode: it is built only when E2SAR is present.
# ---------------------------------------------------------------------------
if(PETSRO_ENABLE_EJFAT)
  add_executable(evio_ejfat_recv src/recv_main.cpp)

  target_link_libraries(evio_ejfat_recv
      PRIVATE evio_ejfat_replay_core
              Boost::program_options
              petsro_warnings)

  install(TARGETS evio_ejfat_recv RUNTIME DESTINATION bin)
else()
  message(STATUS "evio_ejfat_recv needs E2SAR and will not be built")
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(PETSRO_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "")
message(STATUS "evio_ejfat_replay configuration:")
message(STATUS "  Build type    : ${CMAKE_BUILD_TYPE}")
message(STATUS "  EJFAT sending : ${PETSRO_ENABLE_EJFAT}")
message(STATUS "  Tests         : ${PETSRO_BUILD_TESTS}")
message(STATUS "")
