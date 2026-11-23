if((NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  # Observe physical timing before hold-tap and combo events are captured.
  target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/behavior_auto_shift.c)
  zephyr_library()
  zephyr_library_sources(src/behavior_resolved_repeat.c)
  zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)
endif()
