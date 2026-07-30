# Unit tests.
#
# No external test framework: TestHarness.hpp provides registration and
# assertions in ~100 lines. Each executable is one CTest test, and each writes
# its temporary capture files into its own working directory.

set(PETSRO_TEST_TARGETS
    test_evio_reader
    test_synchronizer
    test_packetization
    test_rebaser
    test_event_view)

foreach(target IN LISTS PETSRO_TEST_TARGETS)
  add_executable(${target} ${target}.cpp)

  target_link_libraries(${target}
      PRIVATE evio_ejfat_replay_core
              petsro_warnings)

  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

  add_test(NAME ${target} COMMAND ${target})

  # Temporary captures land here and are removed by TempFile's destructor.
  set_tests_properties(${target} PROPERTIES
      WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
      TIMEOUT 120)
endforeach()

# Real-capture regression test.
#
# The captures in <repo>/data are tens of megabytes and are not tracked in git,
# so this test is registered only when they are actually present. Override the
# directory with -DPETSRO_CAPTURE_DIR=/path/to/captures.
set(PETSRO_CAPTURE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../data"
    CACHE PATH "Directory holding real EVIO captures for the optional regression test")

file(GLOB PETSRO_CAPTURES "${PETSRO_CAPTURE_DIR}/evio_*.bin")

add_executable(test_real_captures test_real_captures.cpp)
target_link_libraries(test_real_captures PRIVATE evio_ejfat_replay_core petsro_warnings)
target_include_directories(test_real_captures PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(PETSRO_CAPTURES)
  list(LENGTH PETSRO_CAPTURES PETSRO_CAPTURE_COUNT)
  message(STATUS "  Real captures : ${PETSRO_CAPTURE_COUNT} found in ${PETSRO_CAPTURE_DIR}")

  add_test(NAME test_real_captures COMMAND test_real_captures ${PETSRO_CAPTURES})
  set_tests_properties(test_real_captures PROPERTIES
      WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
      TIMEOUT 600)
else()
  message(STATUS "  Real captures : none in ${PETSRO_CAPTURE_DIR}; skipping that test")
endif()
