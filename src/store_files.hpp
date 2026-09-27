// Rack Registry - internal file and process primitives.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Nothing in this header is installed: it is not part of the public API.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "rack_registry/result.hpp"

namespace rackregistry {
namespace internal {

// Reads a whole file. The declared size of the file is checked against
// `max_bytes` before any buffer is reserved.
[[nodiscard]] Result<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path,
                                                          std::uint64_t max_bytes);

// Creates an empty file that must not already exist and flushes the directory
// entry. Fails when the name is taken, which is what makes it usable as the
// first step of a race-free lock or temporary file creation.
[[nodiscard]] Status create_file_new(const std::filesystem::path& path);

// Creates or truncates a file and writes `data`. When `flush` is true the data
// is forced to the device before the call returns.
[[nodiscard]] Status write_file_data(const std::filesystem::path& path,
                                     const std::vector<std::uint8_t>& data, bool flush);

// Forces previously written contents of `path` to the device.
[[nodiscard]] Status flush_file(const std::filesystem::path& path);

// Creates a file that must not already exist, writes `data` and flushes it to
// the device.
[[nodiscard]] Status create_file_new_with_data(const std::filesystem::path& path,
                                               const std::vector<std::uint8_t>& data);

// Atomically replaces `target` with `source`. On success `source` no longer
// exists as a separate name.
[[nodiscard]] Status replace_file_atomic(const std::filesystem::path& source,
                                         const std::filesystem::path& target);

// Atomically publishes `source` as `target` and fails when `target` exists.
// This is the primitive behind race-free writer-lock acquisition.
[[nodiscard]] Status publish_file_new(const std::filesystem::path& source,
                                      const std::filesystem::path& target);

// Flushes the directory entry changes for `path` where the platform supports
// it. A no-op on Windows, where the atomic replace is already durable.
void sync_directory(const std::filesystem::path& path) noexcept;

[[nodiscard]] Status remove_file(const std::filesystem::path& path) noexcept;
[[nodiscard]] bool file_exists(const std::filesystem::path& path) noexcept;
[[nodiscard]] std::optional<std::uint64_t> file_size(const std::filesystem::path& path) noexcept;

// Names in `directory` that begin with `prefix`, in ascending byte order.
[[nodiscard]] std::vector<std::filesystem::path> list_directory_matching(
    const std::filesystem::path& directory, const std::string& prefix);

[[nodiscard]] std::uint64_t current_process_id() noexcept;

// A value that changes when the operating system recycles a process
// identifier, used to make writer-lock liveness checks immune to reuse.
// Returns nullopt when the platform cannot supply one, in which case the
// liveness check falls back to reporting the process as alive.
[[nodiscard]] std::optional<std::uint64_t> process_start_marker(std::uint64_t pid) noexcept;

// True when a process with this identifier is running and, when a start marker
// is known, was started at that marker.
[[nodiscard]] bool process_is_alive(std::uint64_t pid,
                                    const std::optional<std::uint64_t>& start_marker) noexcept;

// Cryptographically unpredictable identifier text, from the operating system
// random source. Used for writer incarnations only; it never takes part in
// authoritative facility state.
[[nodiscard]] std::string random_hex(std::size_t byte_count);

// Wall-clock time in nanoseconds since the Unix epoch. Used only for the
// diagnostic timestamp recorded in the writer lock file; no authoritative
// state and no accept/reject decision depends on it.
[[nodiscard]] std::uint64_t system_time_unix_ns() noexcept;

}  // namespace internal
}  // namespace rackregistry
