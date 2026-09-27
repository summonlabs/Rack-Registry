// Rack Registry - minimal test framework.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A deliberately small framework with no third-party dependency. Tests run to
// completion: there is no timeout, no watchdog and no process kill. A test that
// does not terminate is a defect in the test or in the library, and both are
// diagnosed rather than masked.

#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "rack_registry/rack_registry.hpp"

namespace rrtest {

using TestFunction = void (*)();

struct TestCase {
  std::string name;
  TestFunction function;
};

std::vector<TestCase>& registry();
bool register_test(std::string name, TestFunction function);

// Records a failure against the currently running test.
void report_failure(const char* file, int line, const std::string& message);

// Runs every registered test whose name contains `filter`, in registration
// order. Returns the number of failed tests.
int run_all(std::string_view filter);

// ---------------------------------------------------------------------------
// Value rendering for assertion messages
// ---------------------------------------------------------------------------

template <typename T>
concept Streamable = requires(std::ostream& stream, const T& value) { stream << value; };

inline std::string debug_text(bool value) { return value ? "true" : "false"; }
inline std::string debug_text(const std::string& value) { return "\"" + value + "\""; }
inline std::string debug_text(std::string_view value) { return "\"" + std::string(value) + "\""; }
inline std::string debug_text(const char* value) { return std::string("\"") + value + "\""; }

inline std::string debug_text(const rackregistry::RackError& error) {
  return rackregistry::describe(error);
}
inline std::string debug_text(rackregistry::ErrorCode code) {
  return std::string(rackregistry::code_name(code));
}
inline std::string debug_text(rackregistry::MountSpan span) { return span.to_text(); }
inline std::string debug_text(rackregistry::SlotRange range) { return range.to_text(); }
inline std::string debug_text(rackregistry::RackUnitRange range) { return range.to_text(); }
inline std::string debug_text(rackregistry::LifecycleState state) {
  return std::string(rackregistry::lifecycle_state_name(state));
}
inline std::string debug_text(rackregistry::MembershipState state) {
  return std::string(rackregistry::membership_state_name(state));
}
inline std::string debug_text(rackregistry::MountKind kind) {
  return std::string(rackregistry::mount_kind_name(kind));
}
inline std::string debug_text(const rackregistry::StateDigest& digest) { return digest.to_hex(); }
inline std::string debug_text(const rackregistry::RackId& id) { return id.text(); }
inline std::string debug_text(const rackregistry::RackMemberId& id) { return id.text(); }
inline std::string debug_text(const rackregistry::AssetId& id) { return id.text(); }
inline std::string debug_text(const rackregistry::Trait& trait) { return trait.text(); }
inline std::string debug_text(const rackregistry::RequestId& id) { return id.text(); }
inline std::string debug_text(const rackregistry::WriterId& id) { return id.text(); }
inline std::string debug_text(const rackregistry::PowerDomainReference& reference) {
  return reference.text();
}
inline std::string debug_text(const rackregistry::CoolingDomainReference& reference) {
  return reference.text();
}
inline std::string debug_text(const rackregistry::TraitSet& traits) { return traits.to_text(); }
inline std::string debug_text(const rackregistry::CompatibilityReport& report) {
  return report.to_text();
}
inline std::string debug_text(const rackregistry::MutationReceipt& receipt) {
  return receipt.to_text();
}
inline std::string debug_text(const rackregistry::RackDiff& diff) { return diff.to_text(); }
inline std::string debug_text(const rackregistry::SnapshotDiff& diff) { return diff.to_text(); }
inline std::string debug_text(const rackregistry::RecoveryReport& report) {
  return report.to_text();
}
inline std::string debug_text(const rackregistry::WriterLockInfo& info) { return info.to_text(); }
inline std::string debug_text(const rackregistry::StateFileInfo& info) { return info.to_text(); }
inline std::string debug_text(const rackregistry::RackGeneration& generation) {
  return std::to_string(generation.value());
}
inline std::string debug_text(const rackregistry::RackRevision& revision) {
  return std::to_string(revision.value());
}
inline std::string debug_text(const rackregistry::MembershipGeneration& generation) {
  return std::to_string(generation.value());
}
inline std::string debug_text(const rackregistry::StoreEpoch& epoch) {
  return std::to_string(epoch.value());
}
inline std::string debug_text(const rackregistry::StoreSequence& sequence) {
  return std::to_string(sequence.value());
}
inline std::string debug_text(rackregistry::OperationKind kind) {
  return std::string(rackregistry::operation_kind_name(kind));
}
inline std::string debug_text(rackregistry::WriteStage stage) {
  return std::string(rackregistry::write_stage_name(stage));
}
inline std::string debug_text(const rackregistry::RackStructure& structure) {
  return structure.id.text() + "/" + std::to_string(structure.unit_count) + "U";
}

template <typename T>
std::string debug_text(const T& value) {
  if constexpr (Streamable<T>) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
  } else {
    return "<value>";
  }
}

template <typename T>
std::string debug_text(const rackregistry::Result<T>& result) {
  if (result.has_value()) {
    return "ok(" + debug_text(result.value()) + ")";
  }
  return "err(" + debug_text(result.error()) + ")";
}

}  // namespace rrtest

#define RR_TEST(test_name)                                                          \
  static void test_name();                                                          \
  namespace {                                                                       \
  const bool rr_registered_##test_name =                                            \
      ::rrtest::register_test(#test_name, &test_name);                              \
  }                                                                                 \
  static void test_name()

#define RR_CHECK(condition)                                                          \
  do {                                                                               \
    if (!(condition)) {                                                              \
      ::rrtest::report_failure(__FILE__, __LINE__, "check failed: " #condition);      \
    }                                                                                \
  } while (false)

#define RR_CHECK_EQ(actual, expected)                                                \
  do {                                                                               \
    const auto& rr_actual = (actual);                                                \
    const auto& rr_expected = (expected);                                            \
    if (!(rr_actual == rr_expected)) {                                               \
      ::rrtest::report_failure(__FILE__, __LINE__,                                   \
                               std::string("expected " #actual " == " #expected) +    \
                                   "\n    actual:   " + ::rrtest::debug_text(rr_actual) + \
                                   "\n    expected: " + ::rrtest::debug_text(rr_expected)); \
    }                                                                                \
  } while (false)

#define RR_CHECK_NE(actual, unexpected)                                              \
  do {                                                                               \
    const auto& rr_actual = (actual);                                                \
    const auto& rr_unexpected = (unexpected);                                        \
    if (rr_actual == rr_unexpected) {                                                \
      ::rrtest::report_failure(__FILE__, __LINE__,                                   \
                               std::string("expected " #actual " != " #unexpected) +  \
                                   "\n    both: " + ::rrtest::debug_text(rr_actual)); \
    }                                                                                \
  } while (false)

#define RR_REQUIRE(condition)                                                        \
  do {                                                                               \
    if (!(condition)) {                                                              \
      ::rrtest::report_failure(__FILE__, __LINE__, "requirement failed: " #condition); \
      return;                                                                        \
    }                                                                                \
  } while (false)

// Requires that a Result carries a value; on failure the test ends and the
// rejection is reported.
#define RR_REQUIRE_OK(result)                                                        \
  do {                                                                               \
    const auto& rr_result = (result);                                                \
    if (!rr_result.has_value()) {                                                    \
      ::rrtest::report_failure(__FILE__, __LINE__,                                   \
                               std::string("expected success from " #result) +       \
                                   "\n    error: " + ::rrtest::debug_text(rr_result.error())); \
      return;                                                                        \
    }                                                                                \
  } while (false)

// Requires that a Result carries the given error code.
#define RR_REQUIRE_CODE(result, expected_code)                                       \
  do {                                                                               \
    const auto& rr_result = (result);                                                \
    if (rr_result.has_value()) {                                                     \
      ::rrtest::report_failure(__FILE__, __LINE__,                                   \
                               std::string("expected " #result " to fail with " #expected_code) + \
                                   " but it succeeded");                        \
      return;                                                                        \
    }                                                                                \
    if (rr_result.error().code != (expected_code)) {                                 \
      ::rrtest::report_failure(__FILE__, __LINE__,                                   \
                               std::string("expected " #result " to fail with " #expected_code) + \
                                   "\n    actual: " + ::rrtest::debug_text(rr_result.error())); \
      return;                                                                        \
    }                                                                                \
  } while (false)
