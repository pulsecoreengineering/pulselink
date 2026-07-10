# Optional: only builds when libmosquitto is available on the host. CI
# doesn't have it installed, so this target is silently skipped there —
# it's a real-broker validation tool for local development, not something
# the test suite depends on (see host_bridge_demo.cpp's header comment).
find_library(MOSQUITTO_LIBRARY NAMES mosquitto)
find_path(MOSQUITTO_INCLUDE_DIR NAMES mosquitto.h)

if(MOSQUITTO_LIBRARY AND MOSQUITTO_INCLUDE_DIR)
  add_executable(part4_host_bridge_demo host_bridge_demo.cpp)
  target_include_directories(part4_host_bridge_demo PRIVATE ${MOSQUITTO_INCLUDE_DIR})
  target_link_libraries(part4_host_bridge_demo PRIVATE ${MOSQUITTO_LIBRARY})
else()
  message(STATUS "libmosquitto not found - skipping part4_host_bridge_demo")
endif()
