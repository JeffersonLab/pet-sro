# ---------------------------------------------------------------------------
# EjfatReceiverActor -- the ERSAP plugin.
#
# Built only when both E2SAR and ersap-cpp are present. ERSAP loads the result
# with dlopen() and calls create_engine(), so it is always a shared library,
# and it is installed into $ERSAP_HOME/plugins/<name>/lib like the other
# ERSAP C++ actors.
#
#   cmake -DERSAP_HOME=$ERSAP_HOME ..
#
# ERSAP_HOME defaults to the environment variable of the same name.
# ---------------------------------------------------------------------------

if(NOT DEFINED ERSAP_HOME AND DEFINED ENV{ERSAP_HOME})
  set(ERSAP_HOME "$ENV{ERSAP_HOME}" CACHE PATH "ERSAP installation prefix")
endif()

if(NOT ERSAP_HOME)
  message(STATUS
      "ersap-cpp not found (ERSAP_HOME not set);\n"
      "   EjfatReceiverActor will not be built. Install ersap-cpp and set ERSAP_HOME,\n"
      "   or pass -DERSAP_HOME=/path/to/ersap.")
  return()
endif()

find_path(ERSAP_INCLUDE_DIR ersap/engine.hpp
    HINTS ${ERSAP_HOME}/include
    NO_DEFAULT_PATH)
find_library(ERSAP_LIBRARY ersap
    HINTS ${ERSAP_HOME}/lib
    NO_DEFAULT_PATH)

if(NOT ERSAP_INCLUDE_DIR OR NOT ERSAP_LIBRARY)
  message(STATUS
      "ersap-cpp not found (looked under ERSAP_HOME='${ERSAP_HOME}');\n"
      "   EjfatReceiverActor will not be built. Install ersap-cpp and set ERSAP_HOME,\n"
      "   or pass -DERSAP_HOME=/path/to/ersap.")
  return()
endif()

message(STATUS "ersap-cpp found: ${ERSAP_LIBRARY}")

add_library(EjfatReceiverActor SHARED EjfatReceiverActor.cpp)

target_include_directories(EjfatReceiverActor
    PRIVATE ${ERSAP_INCLUDE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR})

# evio_ejfat_replay_core carries the receiver, the EVIO decoder and the E2SAR
# link line, so the actor duplicates none of the protocol logic.
target_link_libraries(EjfatReceiverActor
    PRIVATE evio_ejfat_replay_core
            ${ERSAP_LIBRARY}
            petsro_warnings)

set_target_properties(EjfatReceiverActor PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    POSITION_INDEPENDENT_CODE ON)

install(TARGETS EjfatReceiverActor
    LIBRARY DESTINATION ${ERSAP_HOME}/lib)
