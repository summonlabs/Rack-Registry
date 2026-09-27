// Rack Registry - file and process primitives.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "store_files.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <system_error>
#include <utility>

#include "rack_registry/text.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace rackregistry {
namespace internal {
namespace {

Status io_failure(const std::string& action, const std::filesystem::path& path,
                  const std::string& reason) {
  return make_error(ErrorCode::IoFailure, action + " failed for " + path.string() + ": " + reason,
                    ErrorDetail{.operation = action, .subject = path.string()});
}

std::string path_text(const std::filesystem::path& path) { return path.string(); }

#if defined(_WIN32)

std::string last_error_text() {
  const DWORD code = GetLastError();
  if (code == 0) {
    return "no error reported";
  }
  LPWSTR buffer = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&buffer),
      0, nullptr);
  std::string text;
  if (length != 0 && buffer != nullptr) {
    const std::wstring wide(buffer, length);
    text.reserve(wide.size());
    for (const wchar_t wc : wide) {
      text.push_back(wc >= 0 && wc < 128 ? static_cast<char>(wc) : '?');
    }
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) {
      text.pop_back();
    }
  }
  if (buffer != nullptr) {
    LocalFree(buffer);
  }
  text.append(" (win32 ");
  text.append(std::to_string(code));
  text.push_back(')');
  return text;
}

Status write_and_flush(HANDLE handle, const std::vector<std::uint8_t>& data) {
  std::size_t offset = 0;
  while (offset < data.size()) {
    const std::size_t remaining = data.size() - offset;
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, 1u << 20));
    DWORD written = 0;
    if (WriteFile(handle, data.data() + offset, chunk, &written, nullptr) == 0) {
      return make_error(ErrorCode::IoFailure, "writing state failed: " + last_error_text(),
                        ErrorDetail{.operation = "write"});
    }
    if (written == 0) {
      return make_error(ErrorCode::IoFailure, "writing state made no progress",
                        ErrorDetail{.operation = "write"});
    }
    offset += written;
  }
  if (FlushFileBuffers(handle) == 0) {
    return make_error(ErrorCode::IoFailure, "flushing state failed: " + last_error_text(),
                      ErrorDetail{.operation = "flush"});
  }
  return Status{};
}

Status write_file(const std::filesystem::path& path, const std::vector<std::uint8_t>& data,
                  bool must_not_exist) {
  const DWORD disposition = must_not_exist ? CREATE_NEW : CREATE_ALWAYS;
  const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, disposition,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return io_failure(must_not_exist ? "create" : "replace", path, last_error_text());
  }
  const Status status = write_and_flush(handle, data);
  CloseHandle(handle);
  if (!status) {
    (void)DeleteFileW(path.c_str());
  }
  return status;
}

#else  // POSIX

std::string last_error_text() {
  const int code = errno;
  return std::string(std::strerror(code)) + " (errno " + std::to_string(code) + ")";
}

Status write_and_flush(int descriptor, const std::vector<std::uint8_t>& data) {
  std::size_t offset = 0;
  while (offset < data.size()) {
    const ssize_t written = ::write(descriptor, data.data() + offset, data.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return make_error(ErrorCode::IoFailure, "writing state failed: " + last_error_text(),
                        ErrorDetail{.operation = "write"});
    }
    if (written == 0) {
      return make_error(ErrorCode::IoFailure, "writing state made no progress",
                        ErrorDetail{.operation = "write"});
    }
    offset += static_cast<std::size_t>(written);
  }
  if (::fsync(descriptor) != 0) {
    return make_error(ErrorCode::IoFailure, "flushing state failed: " + last_error_text(),
                      ErrorDetail{.operation = "flush"});
  }
  return Status{};
}

Status write_file(const std::filesystem::path& path, const std::vector<std::uint8_t>& data,
                  bool must_not_exist) {
  const int flags = O_WRONLY | O_CREAT | (must_not_exist ? O_EXCL : O_TRUNC);
  const int descriptor = ::open(path.c_str(), flags, 0644);
  if (descriptor < 0) {
    return io_failure(must_not_exist ? "create" : "replace", path, last_error_text());
  }
  Status status = write_and_flush(descriptor, data);
  if (::close(descriptor) != 0 && status) {
    status = make_error(ErrorCode::IoFailure, "closing state failed: " + last_error_text(),
                        ErrorDetail{.operation = "close"});
  }
  if (!status) {
    (void)::unlink(path.c_str());
  }
  return status;
}

#endif

}  // namespace

Result<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path,
                                            std::uint64_t max_bytes) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    return make_error(ErrorCode::IoFailure,
                      "cannot determine the size of " + path_text(path) + ": " + error.message(),
                      ErrorDetail{.operation = "read", .subject = path_text(path)});
  }
  if (size > max_bytes) {
    return make_error(ErrorCode::StateTooLarge,
                      "state file " + path_text(path) + " declares " + std::to_string(size) +
                          " bytes which exceeds the accepted bound",
                      ErrorDetail{.operation = "read",
                                  .subject = path_text(path),
                                  .expected = max_bytes,
                                  .actual = size});
  }

  std::FILE* stream = nullptr;
#if defined(_WIN32)
  if (_wfopen_s(&stream, path.c_str(), L"rb") != 0) {
    stream = nullptr;
  }
#else
  stream = std::fopen(path.c_str(), "rb");
#endif
  if (stream == nullptr) {
    return make_error(ErrorCode::IoFailure, "cannot open " + path_text(path) + " for reading",
                      ErrorDetail{.operation = "read", .subject = path_text(path)});
  }

  std::vector<std::uint8_t> data;
  data.resize(static_cast<std::size_t>(size));
  std::size_t offset = 0;
  while (offset < data.size()) {
    const std::size_t read_count =
        std::fread(data.data() + offset, 1, data.size() - offset, stream);
    if (read_count == 0) {
      break;
    }
    offset += read_count;
  }
  const bool short_read = offset != data.size();
  const bool failed = std::ferror(stream) != 0;
  std::fclose(stream);

  if (short_read || failed) {
    return make_error(ErrorCode::TruncatedState,
                      "state file " + path_text(path) + " ended after " + std::to_string(offset) +
                          " of " + std::to_string(data.size()) + " bytes",
                      ErrorDetail{.operation = "read",
                                  .subject = path_text(path),
                                  .expected = data.size(),
                                  .actual = offset});
  }
  return data;
}

Status create_file_new(const std::filesystem::path& path) {
  return write_file(path, {}, true);
}

Status write_file_data(const std::filesystem::path& path, const std::vector<std::uint8_t>& data,
                       bool flush) {
#if defined(_WIN32)
  const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return io_failure("write", path, last_error_text());
  }
  Status status = Status{};
  std::size_t offset = 0;
  while (offset < data.size()) {
    const std::size_t remaining = data.size() - offset;
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, 1u << 20));
    DWORD written = 0;
    if (WriteFile(handle, data.data() + offset, chunk, &written, nullptr) == 0 || written == 0) {
      status = make_error(ErrorCode::IoFailure, "writing state failed: " + last_error_text(),
                          ErrorDetail{.operation = "write", .subject = path_text(path)});
      break;
    }
    offset += written;
  }
  if (status && flush && FlushFileBuffers(handle) == 0) {
    status = make_error(ErrorCode::IoFailure, "flushing state failed: " + last_error_text(),
                        ErrorDetail{.operation = "flush", .subject = path_text(path)});
  }
  CloseHandle(handle);
  return status;
#else
  const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (descriptor < 0) {
    return io_failure("write", path, last_error_text());
  }
  Status status = Status{};
  std::size_t offset = 0;
  while (offset < data.size()) {
    const ssize_t written = ::write(descriptor, data.data() + offset, data.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      status = make_error(ErrorCode::IoFailure, "writing state failed: " + last_error_text(),
                          ErrorDetail{.operation = "write", .subject = path_text(path)});
      break;
    }
    offset += static_cast<std::size_t>(written);
  }
  if (status && flush && ::fsync(descriptor) != 0) {
    status = make_error(ErrorCode::IoFailure, "flushing state failed: " + last_error_text(),
                        ErrorDetail{.operation = "flush", .subject = path_text(path)});
  }
  if (::close(descriptor) != 0 && status) {
    status = make_error(ErrorCode::IoFailure, "closing state failed: " + last_error_text(),
                        ErrorDetail{.operation = "close", .subject = path_text(path)});
  }
  return status;
#endif
}

Status flush_file(const std::filesystem::path& path) {
#if defined(_WIN32)
  const HANDLE handle =
      CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                  nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return io_failure("open for flush", path, last_error_text());
  }
  const Status status =
      FlushFileBuffers(handle) == 0
          ? Status(make_error(ErrorCode::IoFailure, "flushing state failed: " + last_error_text(),
                              ErrorDetail{.operation = "flush", .subject = path_text(path)}))
          : Status{};
  CloseHandle(handle);
  return status;
#else
  const int descriptor = ::open(path.c_str(), O_RDONLY);
  if (descriptor < 0) {
    return io_failure("open for flush", path, last_error_text());
  }
  const Status status = ::fsync(descriptor) != 0
                            ? Status(make_error(ErrorCode::IoFailure,
                                                "flushing state failed: " + last_error_text(),
                                                ErrorDetail{.operation = "flush",
                                                            .subject = path_text(path)}))
                            : Status{};
  (void)::close(descriptor);
  return status;
#endif
}

Status create_file_new_with_data(const std::filesystem::path& path,
                                 const std::vector<std::uint8_t>& data) {
  return write_file(path, data, true);
}

Status replace_file_atomic(const std::filesystem::path& source,
                           const std::filesystem::path& target) {
#if defined(_WIN32)
  if (MoveFileExW(source.c_str(), target.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    return io_failure("atomic replace", target, last_error_text());
  }
  return Status{};
#else
  if (::rename(source.c_str(), target.c_str()) != 0) {
    return io_failure("atomic replace", target, last_error_text());
  }
  return Status{};
#endif
}

Status publish_file_new(const std::filesystem::path& source,
                        const std::filesystem::path& target) {
#if defined(_WIN32)
  if (MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) == 0) {
    const DWORD code = GetLastError();
    if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) {
      return make_error(ErrorCode::WriterLockHeld,
                        "another writer already holds " + path_text(target),
                        ErrorDetail{.operation = "publish", .subject = path_text(target)});
    }
    return io_failure("publish", target, last_error_text());
  }
  return Status{};
#else
  // link() publishes the new name atomically and fails when it already exists,
  // which is exactly the mutual-exclusion primitive the writer lock needs.
  if (::link(source.c_str(), target.c_str()) != 0) {
    if (errno == EEXIST) {
      return make_error(ErrorCode::WriterLockHeld,
                        "another writer already holds " + path_text(target),
                        ErrorDetail{.operation = "publish", .subject = path_text(target)});
    }
    return io_failure("publish", target, last_error_text());
  }
  (void)::unlink(source.c_str());
  return Status{};
#endif
}

void sync_directory(const std::filesystem::path& path) noexcept {
#if !defined(_WIN32)
  const std::filesystem::path directory = path.has_parent_path() ? path.parent_path() : ".";
  const int descriptor = ::open(directory.c_str(), O_RDONLY);
  if (descriptor >= 0) {
    (void)::fsync(descriptor);
    (void)::close(descriptor);
  }
#else
  (void)path;
#endif
}

Status remove_file(const std::filesystem::path& path) noexcept {
  std::error_code error;
  const bool removed = std::filesystem::remove(path, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    return make_error(ErrorCode::IoFailure,
                      "cannot remove " + path_text(path) + ": " + error.message(),
                      ErrorDetail{.operation = "remove", .subject = path_text(path)});
  }
  (void)removed;
  return Status{};
}

bool file_exists(const std::filesystem::path& path) noexcept {
  std::error_code error;
  const bool exists = std::filesystem::exists(path, error);
  return !error && exists;
}

std::optional<std::uint64_t> file_size(const std::filesystem::path& path) noexcept {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(size);
}

std::vector<std::filesystem::path> list_directory_matching(
    const std::filesystem::path& directory, const std::string& prefix) {
  std::vector<std::filesystem::path> matches;
  std::error_code error;
  if (!std::filesystem::is_directory(directory, error) || error) {
    return matches;
  }
  for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
    if (error) {
      break;
    }
    const std::string name = entry.path().filename().string();
    if (name.size() >= prefix.size() && name.compare(0, prefix.size(), prefix) == 0) {
      matches.push_back(entry.path());
    }
  }
  std::sort(matches.begin(), matches.end(), [](const std::filesystem::path& a,
                                               const std::filesystem::path& b) {
    return byte_less(a.filename().string(), b.filename().string());
  });
  return matches;
}

std::uint64_t current_process_id() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

std::optional<std::uint64_t> process_start_marker(std::uint64_t pid) noexcept {
#if defined(_WIN32)
  const HANDLE handle =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (handle == nullptr) {
    return std::nullopt;
  }
  FILETIME creation{};
  FILETIME exit_time{};
  FILETIME kernel{};
  FILETIME user{};
  std::optional<std::uint64_t> marker;
  if (GetProcessTimes(handle, &creation, &exit_time, &kernel, &user) != 0) {
    ULARGE_INTEGER value{};
    value.LowPart = creation.dwLowDateTime;
    value.HighPart = creation.dwHighDateTime;
    marker = value.QuadPart;
  }
  CloseHandle(handle);
  return marker;
#else
  const std::filesystem::path stat_path = "/proc/" + std::to_string(pid) + "/stat";
  std::FILE* stream = std::fopen(stat_path.c_str(), "rb");
  if (stream == nullptr) {
    return std::nullopt;
  }
  std::array<char, 4096> buffer{};
  const std::size_t read_count = std::fread(buffer.data(), 1, buffer.size() - 1, stream);
  std::fclose(stream);
  if (read_count == 0) {
    return std::nullopt;
  }
  buffer[read_count] = '\0';
  // Field 22 of /proc/<pid>/stat is the process start time in clock ticks. The
  // second field is the executable name in parentheses and may contain spaces,
  // so parsing starts after the closing parenthesis.
  const char* cursor = std::strrchr(buffer.data(), ')');
  if (cursor == nullptr) {
    return std::nullopt;
  }
  cursor += 1;
  int field = 2;
  while (*cursor != '\0') {
    while (*cursor == ' ') {
      ++cursor;
    }
    if (*cursor == '\0') {
      break;
    }
    ++field;
    if (field == 22) {
      std::uint64_t value = 0;
      while (*cursor >= '0' && *cursor <= '9') {
        value = value * 10 + static_cast<std::uint64_t>(*cursor - '0');
        ++cursor;
      }
      return value;
    }
    while (*cursor != ' ' && *cursor != '\0') {
      ++cursor;
    }
  }
  return std::nullopt;
#endif
}

bool process_is_alive(std::uint64_t pid, const std::optional<std::uint64_t>& start_marker) noexcept {
  if (pid == 0) {
    return false;
  }
#if defined(_WIN32)
  const HANDLE handle =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (handle == nullptr) {
    return false;
  }
  DWORD exit_code = 0;
  const bool alive = GetExitCodeProcess(handle, &exit_code) != 0 && exit_code == STILL_ACTIVE;
  CloseHandle(handle);
  if (!alive) {
    return false;
  }
#else
  if (::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) {
    return false;
  }
#endif
  if (!start_marker.has_value()) {
    // The platform could not supply a start marker when the lock was written,
    // so the identifier alone is the evidence available. Reporting the process
    // as alive is the conservative choice: it never lets a second writer in.
    return true;
  }
  const auto current_marker = process_start_marker(pid);
  if (!current_marker.has_value()) {
    return true;
  }
  return *current_marker == *start_marker;
}

std::string random_hex(std::size_t byte_count) {
  std::random_device device;
  std::vector<std::uint8_t> bytes;
  bytes.reserve(byte_count);
  for (std::size_t i = 0; i < byte_count; ++i) {
    bytes.push_back(static_cast<std::uint8_t>(device() & 0xFFu));
  }
  return to_hex(bytes);
}

std::uint64_t system_time_unix_ns() noexcept {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  return nanoseconds > 0 ? static_cast<std::uint64_t>(nanoseconds) : 0;
}

}  // namespace internal
}  // namespace rackregistry
