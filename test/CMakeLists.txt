add_executable(pulselink_host_tests
  main.cpp
  test_skeleton.cpp
  test_frame.cpp
  test_fields.cpp
  test_dedupe.cpp
  test_ring.cpp
  test_fake_transport.cpp
  test_registry.cpp
  test_join.cpp
  test_join_flow.cpp
  test_cmdtable.cpp
  test_cmd_flow.cpp
  test_topics.cpp
  test_spool.cpp
  test_fake_mqtt.cpp
  test_gateway_hsm.cpp
  test_gateway_bridge.cpp
  test_loss_tracker.cpp
  test_cmd_status.cpp
  test_health_metrics.cpp
)

target_include_directories(pulselink_host_tests PRIVATE ${CMAKE_SOURCE_DIR})

add_test(NAME host_tests COMMAND pulselink_host_tests)
