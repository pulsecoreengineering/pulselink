add_executable(pulselink_host_tests
  main.cpp
  test_skeleton.cpp
  test_frame.cpp
  test_fields.cpp
  test_dedupe.cpp
  test_ring.cpp
  test_fake_transport.cpp
)

target_include_directories(pulselink_host_tests PRIVATE ${CMAKE_SOURCE_DIR})

add_test(NAME host_tests COMMAND pulselink_host_tests)
