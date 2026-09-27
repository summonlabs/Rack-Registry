// Rack Registry - shared test support implementation.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "test_support.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace rrtest {
namespace {

#if defined(_WIN32)

std::wstring widen(const std::string& text) {
  std::wstring out;
  out.reserve(text.size());
  for (const char c : text) {
    out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
  }
  return out;
}

std::wstring command_line_for(const std::filesystem::path& executable,
                              const std::vector<std::string>& arguments) {
  std::wstring command = L"\"" + executable.wstring() + L"\"";
  for (const std::string& argument : arguments) {
    command.append(L" \"");
    command.append(widen(argument));
    command.push_back(L'"');
  }
  return command;
}

#endif

std::filesystem::path executable_directory() {
#if defined(_WIN32)
  std::vector<wchar_t> buffer(32768);
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                          static_cast<DWORD>(buffer.size()));
  if (length == 0) {
    return std::filesystem::current_path();
  }
  return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
#else
  std::error_code error;
  const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", error);
  if (error) {
    return std::filesystem::current_path();
  }
  return self.parent_path();
#endif
}

std::string read_text_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream.good()) {
    return {};
  }
  std::string contents;
  stream.seekg(0, std::ios::end);
  const std::streamoff size = stream.tellg();
  stream.seekg(0, std::ios::beg);
  if (size > 0) {
    contents.resize(static_cast<std::size_t>(size));
    stream.read(contents.data(), size);
  }
  return contents;
}

std::string unique_suffix() {
  static std::random_device device;
  const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  return std::to_string(now) + "-" + std::to_string(device());
}

}  // namespace

TempDir::TempDir(std::string_view label) {
  std::error_code error;
  const std::filesystem::path base = std::filesystem::temp_directory_path(error);
  const std::filesystem::path root = error ? std::filesystem::path(".") : base;
  path_ = root / ("rack-registry-test-" + std::string(label) + "-" + unique_suffix());
  std::filesystem::create_directories(path_, error);
}

TempDir::~TempDir() {
  std::error_code error;
  std::filesystem::remove_all(path_, error);
}

std::filesystem::path TempDir::file(std::string_view name) const { return path_ / name; }

std::filesystem::path helper_executable(std::string_view name) {
  const std::filesystem::path directory = executable_directory();
#if defined(_WIN32)
  const std::string file_name = std::string(name) + ".exe";
#else
  const std::string file_name = std::string(name);
#endif
  // The suites live in a subdirectory of the build tree while the harnesses are
  // built in the build root, so the search walks up a bounded number of levels
  // rather than assuming one layout.
  std::filesystem::path candidate = directory;
  for (int level = 0; level < 4; ++level) {
    const std::filesystem::path probe = candidate / file_name;
    std::error_code error;
    if (std::filesystem::exists(probe, error) && !error) {
      return probe;
    }
    if (!candidate.has_parent_path() || candidate.parent_path() == candidate) {
      break;
    }
    candidate = candidate.parent_path();
  }
  return directory / file_name;
}

ProcessResult run_process(const std::filesystem::path& executable,
                          const std::vector<std::string>& arguments) {
  ProcessResult result;
  const std::filesystem::path out_path =
      std::filesystem::temp_directory_path() / ("rr-proc-out-" + unique_suffix() + ".txt");
  const std::filesystem::path err_path =
      std::filesystem::temp_directory_path() / ("rr-proc-err-" + unique_suffix() + ".txt");

#if defined(_WIN32)
  std::wstring command = L"\"" + executable.wstring() + L"\"";
  for (const std::string& argument : arguments) {
    command.append(L" \"");
    for (const char c : argument) {
      command.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }
    command.push_back(L'"');
  }

  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;

  HANDLE out_handle = CreateFileW(out_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  HANDLE err_handle = CreateFileW(err_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (out_handle == INVALID_HANDLE_VALUE || err_handle == INVALID_HANDLE_VALUE) {
    if (out_handle != INVALID_HANDLE_VALUE) {
      CloseHandle(out_handle);
    }
    if (err_handle != INVALID_HANDLE_VALUE) {
      CloseHandle(err_handle);
    }
    return result;
  }
  startup.hStdOutput = out_handle;
  startup.hStdError = err_handle;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION process{};
  std::vector<wchar_t> mutable_command(command.begin(), command.end());
  mutable_command.push_back(L'\0');
  const BOOL created = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, 0,
                                      nullptr, nullptr, &startup, &process);
  CloseHandle(out_handle);
  CloseHandle(err_handle);
  if (created == 0) {
    return result;
  }
  result.started = true;
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = 0;
  GetExitCodeProcess(process.hProcess, &exit_code);
  result.exit_code = static_cast<int>(exit_code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
#else
  const pid_t pid = ::fork();
  if (pid < 0) {
    return result;
  }
  if (pid == 0) {
    const int out_descriptor = ::open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    const int err_descriptor = ::open(err_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_descriptor >= 0) {
      (void)::dup2(out_descriptor, STDOUT_FILENO);
    }
    if (err_descriptor >= 0) {
      (void)::dup2(err_descriptor, STDERR_FILENO);
    }
    std::vector<char*> argv;
    std::string program = executable.string();
    argv.push_back(program.data());
    for (const std::string& argument : arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(program.c_str(), argv.data());
    ::_exit(127);
  }
  int status = 0;
  (void)::waitpid(pid, &status, 0);
  result.started = true;
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif

  result.standard_output = read_text_file(out_path);
  result.standard_error = read_text_file(err_path);
  std::error_code error;
  std::filesystem::remove(out_path, error);
  std::filesystem::remove(err_path, error);
  return result;
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : started_(other.started_),
      reaped_(other.reaped_),
      process_id_(other.process_id_),
      handle_(other.handle_) {
  other.started_ = false;
  other.reaped_ = true;
  other.handle_ = nullptr;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    if (started_ && !reaped_) {
      terminate();
      (void)wait();
    }
    started_ = other.started_;
    reaped_ = other.reaped_;
    process_id_ = other.process_id_;
    handle_ = other.handle_;
    other.started_ = false;
    other.reaped_ = true;
    other.handle_ = nullptr;
  }
  return *this;
}

ChildProcess::~ChildProcess() {
  if (started_ && !reaped_) {
    terminate();
    (void)wait();
  }
}

ChildProcess ChildProcess::spawn(const std::filesystem::path& executable,
                                 const std::vector<std::string>& arguments,
                                 const std::filesystem::path& output_file,
                                 const std::filesystem::path& error_file) {
  ChildProcess child;
#if defined(_WIN32)
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  HANDLE out_handle = CreateFileW(output_file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  HANDLE err_handle = CreateFileW(error_file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (out_handle == INVALID_HANDLE_VALUE || err_handle == INVALID_HANDLE_VALUE) {
    if (out_handle != INVALID_HANDLE_VALUE) {
      CloseHandle(out_handle);
    }
    if (err_handle != INVALID_HANDLE_VALUE) {
      CloseHandle(err_handle);
    }
    return child;
  }
  startup.hStdOutput = out_handle;
  startup.hStdError = err_handle;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION process{};
  std::wstring command = command_line_for(executable, arguments);
  std::vector<wchar_t> mutable_command(command.begin(), command.end());
  mutable_command.push_back(L'\0');
  const BOOL created = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
  CloseHandle(out_handle);
  CloseHandle(err_handle);
  if (created == 0) {
    return child;
  }
  CloseHandle(process.hThread);
  child.started_ = true;
  child.process_id_ = static_cast<std::uint64_t>(process.dwProcessId);
  child.handle_ = process.hProcess;
#else
  const pid_t pid = ::fork();
  if (pid < 0) {
    return child;
  }
  if (pid == 0) {
    const int out_descriptor = ::open(output_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    const int err_descriptor = ::open(error_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_descriptor >= 0) {
      (void)::dup2(out_descriptor, STDOUT_FILENO);
    }
    if (err_descriptor >= 0) {
      (void)::dup2(err_descriptor, STDERR_FILENO);
    }
    std::vector<char*> argv;
    std::string program = executable.string();
    argv.push_back(program.data());
    for (const std::string& argument : arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(program.c_str(), argv.data());
    ::_exit(127);
  }
  child.started_ = true;
  child.process_id_ = static_cast<std::uint64_t>(pid);
  child.handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(pid));
#endif
  return child;
}

void ChildProcess::terminate() {
  if (!started_ || reaped_) {
    return;
  }
#if defined(_WIN32)
  if (handle_ != nullptr) {
    (void)TerminateProcess(static_cast<HANDLE>(handle_), 1);
  }
#else
  (void)::kill(static_cast<pid_t>(process_id_), SIGKILL);
#endif
}

int ChildProcess::wait() {
  if (!started_ || reaped_) {
    return -1;
  }
  reaped_ = true;
  int exit_code = -1;
#if defined(_WIN32)
  if (handle_ != nullptr) {
    WaitForSingleObject(static_cast<HANDLE>(handle_), INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(static_cast<HANDLE>(handle_), &code);
    exit_code = static_cast<int>(code);
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
#else
  int status = 0;
  (void)::waitpid(static_cast<pid_t>(process_id_), &status, 0);
  exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
  return exit_code;
}

bool wait_for_file(const std::filesystem::path& path, int milliseconds) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
  while (std::chrono::steady_clock::now() < deadline) {
    std::error_code error;
    if (std::filesystem::exists(path, error) && !error) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  std::error_code error;
  return std::filesystem::exists(path, error) && !error;
}

rackregistry::ProvenanceRecord provenance_for(std::string_view actor, std::uint64_t sequence) {
  rackregistry::ProvenanceRecord provenance;
  provenance.source = rackregistry::ProvenanceSource::System;
  provenance.actor = rackregistry::ActorId::parse(actor).value();
  provenance.source_sequence = sequence;
  provenance.observed_at_unix_ns = 1'800'000'000'000'000'000ull + sequence;
  return provenance;
}

rackregistry::StoreOptions store_options_for(const std::filesystem::path& path,
                                             std::string_view writer) {
  rackregistry::StoreOptions options;
  options.path = path;
  if (!writer.empty()) {
    options.writer_id = rackregistry::WriterId::parse(writer).value();
  }
  options.producer = rackregistry::SourceReference::parse("rack-registry-tests/1.0.0").value();
  return options;
}

}  // namespace rrtest
