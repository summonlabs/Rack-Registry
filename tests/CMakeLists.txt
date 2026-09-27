# Rack Registry - test suite.
#
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# Each suite is one executable registered as one CTest test. No test declares a
# timeout, uses a watchdog, or treats a killed process as a pass: a test that
# does not terminate is a defect to diagnose.

set(RR_TEST_SUPPORT test_framework.cpp test_support.cpp)

function(rr_add_test target)
  add_executable(${target} ${ARGN} ${RR_TEST_SUPPORT})
  rr_configure_target(${target})
  target_link_libraries(${target} PRIVATE RackRegistry::rack_registry)
  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ${target} COMMAND ${target})
endfunction()

rr_add_test(test_text_mount test_text_mount.cpp)
rr_add_test(test_registry_model test_registry_model.cpp)
rr_add_test(test_serialization test_serialization.cpp)
rr_add_test(test_property test_property.cpp)
rr_add_test(test_adversarial test_adversarial.cpp)
rr_add_test(test_persistence test_persistence.cpp)
rr_add_test(test_concurrency test_concurrency.cpp)
rr_add_test(test_crash_recovery test_crash_recovery.cpp)
rr_add_test(test_multiprocess test_multiprocess.cpp)

# The process-level suites start the harness executables, so the harnesses must
# be built first. The dependency is explicit so a partial build cannot silently
# skip the proof.
if(TARGET rr_crash_child)
  add_dependencies(test_crash_recovery rr_crash_child)
endif()
if(TARGET rr_fence_child)
  add_dependencies(test_multiprocess rr_fence_child)
endif()
