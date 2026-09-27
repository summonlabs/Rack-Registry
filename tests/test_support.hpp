// Rack Registry - shared test support.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "rack_registry/rack_registry.hpp"

namespace rrtest {

// A directory that is created on construction and removed, with everything in
// it, on destruction. Tests never write into the source tree.
class TempDir {
 public:
  explicit TempDir(std::string_view label);
  ~TempDir();

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  TempDir(TempDir&&) = delete;
  TempDir& operator=(TempDir&&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  [[nodiscard]] std::filesystem::path file(std::string_view name) const;

 private:
  std::filesystem::path path_{};
};

// Result of running another operating-system process.
struct ProcessResult {
  int exit_code = -1;
  std::string standard_output{};
  std::string standard_error{};
  bool started = false;
};

// Runs an executable as a separate process with the given arguments, capturing
// its output through temporary files rather than pipes. A test that starts a
// process and then inspects the files the process produced is proving real
// process-level behaviour, not an in-process simulation.
[[nodiscard]] ProcessResult run_process(const std::filesystem::path& executable,
                                        const std::vector<std::string>& arguments);

// A process started without waiting for it, so a test can terminate it while it
// is running. Used to prove that writer authority is released when a holder
// dies without unwinding.
class ChildProcess {
 public:
  ChildProcess() = default;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;
  ~ChildProcess();

  // Starts `executable`, redirecting its output into the given files.
  [[nodiscard]] static ChildProcess spawn(const std::filesystem::path& executable,
                                          const std::vector<std::string>& arguments,
                                          const std::filesystem::path& output_file,
                                          const std::filesystem::path& error_file);

  [[nodiscard]] bool started() const noexcept { return started_; }

  // Terminates the process without giving it a chance to clean up, which is
  // what a crash looks like from the outside.
  void terminate();

  // Waits for the process and returns its exit code.
  [[nodiscard]] int wait();

  [[nodiscard]] std::uint64_t process_id() const noexcept { return process_id_; }

 private:
  bool started_ = false;
  bool reaped_ = false;
  std::uint64_t process_id_ = 0;
  void* handle_ = nullptr;
};

// Waits until `path` exists or the deadline passes. Returns true when the file
// appeared. The wait is bounded so a defective helper cannot hang a test; the
// bound is on the helper's readiness signal, never on a test's completion.
[[nodiscard]] bool wait_for_file(const std::filesystem::path& path, int milliseconds);

// Absolute path of a helper executable built alongside the tests.
[[nodiscard]] std::filesystem::path helper_executable(std::string_view name);

// Deterministic pseudo random generator used by the property tests. The seed is
// fixed, printed by the test and part of the reproduction instructions.
class Random {
 public:
  explicit Random(std::uint64_t seed) noexcept : state_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}

  [[nodiscard]] std::uint64_t next_u64() noexcept {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return state_;
  }

  [[nodiscard]] std::uint32_t below(std::uint32_t bound) noexcept {
    return bound == 0 ? 0 : static_cast<std::uint32_t>(next_u64() % bound);
  }

  [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }

 private:
  std::uint64_t state_;
  std::uint64_t seed_ = 0;
};

// Convenience builders shared by several suites.
[[nodiscard]] rackregistry::ProvenanceRecord provenance_for(std::string_view actor,
                                                            std::uint64_t sequence);

[[nodiscard]] rackregistry::StoreOptions store_options_for(const std::filesystem::path& path,
                                                           std::string_view writer);

}  // namespace rrtest
