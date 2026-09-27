// Rack Registry - durable, transactional store with writer fencing.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_registry/persistence.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include "rack_registry/limits.hpp"
#include "rack_registry/text.hpp"
#include "store_files.hpp"

namespace rackregistry {
namespace {

constexpr std::string_view kLockFileHeader = "rackreg-lock 1";
constexpr std::string_view kNone = "none";
constexpr std::size_t kLockAcquireAttempts = 6;

std::string path_text(const std::filesystem::path& path) { return path.string(); }

std::filesystem::path previous_path_for(const std::filesystem::path& path) {
  std::filesystem::path result = path;
  result += ".prev";
  return result;
}

std::filesystem::path lock_path_for(const std::filesystem::path& path) {
  std::filesystem::path result = path;
  result += ".lock";
  return result;
}

std::string state_temp_prefix(const std::filesystem::path& path) {
  return path.filename().string() + ".tmp-";
}

std::string lock_temp_prefix(const std::filesystem::path& path) {
  return path.filename().string() + ".lock.tmp-";
}

std::filesystem::path directory_of(const std::filesystem::path& path) {
  return path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
}

// ---------------------------------------------------------------------------
// Writer lock record
// ---------------------------------------------------------------------------

std::string encode_lock_record(const WriterLockInfo& info) {
  std::string body;
  body.append(kLockFileHeader);
  body.push_back('\n');
  body.append("writer ")
      .append(info.writer_id.empty() ? std::string(kNone) : info.writer_id.text());
  body.push_back('\n');
  body.append("incarnation ")
      .append(info.incarnation.empty() ? std::string(kNone) : info.incarnation);
  body.push_back('\n');
  body.append("pid ").append(std::to_string(info.pid));
  body.push_back('\n');
  body.append("start ").append(info.process_start_marker == 0
                                   ? std::string(kNone)
                                   : std::to_string(info.process_start_marker));
  body.push_back('\n');
  body.append("epoch ").append(std::to_string(info.epoch.value()));
  body.push_back('\n');
  body.append("acquired ").append(std::to_string(info.acquired_at_unix_ns));
  body.push_back('\n');
  body.append("taken_over_from ")
      .append(info.taken_over_from.empty() ? std::string(kNone) : info.taken_over_from.text());
  body.push_back('\n');
  body.append("taken_over_epoch ")
      .append(info.taken_over_epoch.value() == 0 ? std::string(kNone)
                                                 : std::to_string(info.taken_over_epoch.value()));
  body.push_back('\n');

  const auto digest = sha256(reinterpret_cast<const std::uint8_t*>(body.data()), body.size());
  body.append("sha256 ").append(to_hex(digest.data(), digest.size()));
  body.push_back('\n');
  return body;
}

Result<std::uint64_t> parse_decimal(std::string_view text, std::string_view field) {
  if (text.empty() || text.size() > 20) {
    return make_error(ErrorCode::WriterLockInvalid,
                      "writer lock field " + std::string(field) + " is not a decimal integer",
                      ErrorDetail{.operation = "writer_lock", .related = std::string(field)});
  }
  std::uint64_t value = 0;
  for (const char c : text) {
    if (!ascii_digit(c)) {
      return make_error(ErrorCode::WriterLockInvalid,
                        "writer lock field " + std::string(field) + " is not a decimal integer",
                        ErrorDetail{.operation = "writer_lock", .related = std::string(field)});
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (value > (static_cast<std::uint64_t>(-1) - digit) / 10u) {
      return make_error(ErrorCode::WriterLockInvalid,
                        "writer lock field " + std::string(field) + " overflows its range",
                        ErrorDetail{.operation = "writer_lock", .related = std::string(field)});
    }
    value = value * 10u + digit;
  }
  return value;
}

Result<WriterLockInfo> decode_lock_record(std::string_view text) {
  const auto lines = split(text, '\n');
  // Nine content lines, the digest line, and the empty field after the final
  // newline.
  if (lines.size() != 11 || !lines[10].empty()) {
    return make_error(ErrorCode::WriterLockInvalid,
                      "writer lock file does not have the expected line structure",
                      ErrorDetail{.operation = "writer_lock", .actual = lines.size()});
  }
  if (lines[0] != kLockFileHeader) {
    return make_error(ErrorCode::WriterLockInvalid, "writer lock file has an unexpected header",
                      ErrorDetail{.operation = "writer_lock", .subject = lines[0]});
  }

  std::size_t digest_offset = 0;
  for (std::size_t i = 0; i < 9; ++i) {
    digest_offset += lines[i].size() + 1;
  }
  if (digest_offset > text.size()) {
    return make_error(ErrorCode::WriterLockInvalid, "writer lock file is truncated",
                      ErrorDetail{.operation = "writer_lock"});
  }
  const auto expected = sha256(reinterpret_cast<const std::uint8_t*>(text.data()), digest_offset);
  if (lines[9] != "sha256 " + to_hex(expected.data(), expected.size())) {
    return make_error(ErrorCode::WriterLockInvalid,
                      "writer lock file fails its integrity check",
                      ErrorDetail{.operation = "writer_lock"});
  }

  const auto field = [&lines](std::size_t index, std::string_view key) -> Result<std::string_view> {
    const std::string& line = lines[index];
    if (line.size() <= key.size() + 1 || line.compare(0, key.size(), key) != 0 ||
        line[key.size()] != ' ') {
      return make_error(ErrorCode::WriterLockInvalid,
                        "writer lock line " + std::to_string(index) + " does not begin with '" +
                            std::string(key) + "'",
                        ErrorDetail{.operation = "writer_lock", .related = std::string(key)});
    }
    return std::string_view(line).substr(key.size() + 1);
  };

  const auto writer_field = field(1, "writer");
  const auto incarnation_field = field(2, "incarnation");
  const auto pid_field = field(3, "pid");
  const auto start_field = field(4, "start");
  const auto epoch_field = field(5, "epoch");
  const auto acquired_field = field(6, "acquired");
  const auto taken_writer_field = field(7, "taken_over_from");
  const auto taken_epoch_field = field(8, "taken_over_epoch");
  for (const auto* candidate : {&writer_field, &incarnation_field, &pid_field, &start_field,
                                &epoch_field, &acquired_field, &taken_writer_field,
                                &taken_epoch_field}) {
    if (!candidate->has_value()) {
      return candidate->error();
    }
  }

  WriterLockInfo info;
  if (writer_field.value() != kNone) {
    const auto writer = WriterId::parse(writer_field.value());
    if (!writer) {
      return make_error(ErrorCode::WriterLockInvalid, "writer lock names an invalid writer identity",
                        ErrorDetail{.operation = "writer_lock"});
    }
    info.writer_id = writer.value();
  }
  if (incarnation_field.value() != kNone) {
    if (incarnation_field.value().empty() || incarnation_field.value().size() > 64 ||
        !all_bytes(incarnation_field.value(),
                   [](char c) { return ascii_digit(c) || (c >= 'a' && c <= 'f'); })) {
      return make_error(ErrorCode::WriterLockInvalid, "writer lock names an invalid incarnation",
                        ErrorDetail{.operation = "writer_lock"});
    }
    info.incarnation = std::string(incarnation_field.value());
  }
  const auto pid = parse_decimal(pid_field.value(), "pid");
  if (!pid) {
    return pid.error();
  }
  info.pid = pid.value();
  if (start_field.value() != kNone) {
    const auto start = parse_decimal(start_field.value(), "start");
    if (!start) {
      return start.error();
    }
    info.process_start_marker = start.value();
  }
  const auto epoch = parse_decimal(epoch_field.value(), "epoch");
  if (!epoch) {
    return epoch.error();
  }
  if (epoch.value() == 0) {
    return make_error(ErrorCode::WriterLockInvalid, "writer lock epoch must be at least 1",
                      ErrorDetail{.operation = "writer_lock", .actual = epoch.value()});
  }
  info.epoch = StoreEpoch::create(epoch.value()).value();
  const auto acquired = parse_decimal(acquired_field.value(), "acquired");
  if (!acquired) {
    return acquired.error();
  }
  info.acquired_at_unix_ns = acquired.value();
  if (taken_writer_field.value() != kNone) {
    const auto writer = WriterId::parse(taken_writer_field.value());
    if (!writer) {
      return make_error(ErrorCode::WriterLockInvalid,
                        "writer lock names an invalid previous writer identity",
                        ErrorDetail{.operation = "writer_lock"});
    }
    info.taken_over_from = writer.value();
  }
  if (taken_epoch_field.value() != kNone) {
    const auto taken = parse_decimal(taken_epoch_field.value(), "taken_over_epoch");
    if (!taken) {
      return taken.error();
    }
    if (taken.value() != 0) {
      info.taken_over_epoch = StoreEpoch::create(taken.value()).value();
    }
  }
  info.state = WriterLockState::HeldByAnotherProcess;
  return info;
}

// Reads the writer lock. An unreadable or unverifiable record is reported as
// Invalid rather than as an error: the caller decides what to do with it.
Result<WriterLockInfo> read_lock_record(const std::filesystem::path& lock_path) {
  if (!internal::file_exists(lock_path)) {
    WriterLockInfo info;
    info.state = WriterLockState::Unlocked;
    return info;
  }
  const auto size = internal::file_size(lock_path);
  if (!size.has_value() || size.value() > 4096) {
    WriterLockInfo info;
    info.state = WriterLockState::Invalid;
    info.detail = "the writer lock file is larger than the accepted bound";
    return info;
  }
  auto bytes = internal::read_file(lock_path, 4096);
  if (!bytes) {
    WriterLockInfo info;
    info.state = WriterLockState::Invalid;
    info.detail = bytes.error().message;
    return info;
  }
  const std::string text(bytes.value().begin(), bytes.value().end());
  auto decoded = decode_lock_record(text);
  if (!decoded) {
    WriterLockInfo info;
    info.state = WriterLockState::Invalid;
    info.detail = decoded.error().message;
    return info;
  }
  return decoded;
}

bool holder_is_alive(const WriterLockInfo& info) {
  const std::optional<std::uint64_t> marker =
      info.process_start_marker == 0 ? std::nullopt
                                     : std::optional<std::uint64_t>(info.process_start_marker);
  return internal::process_is_alive(info.pid, marker);
}

Status validate_options(const StoreOptions& options) {
  if (options.path.empty()) {
    return make_error(ErrorCode::InvalidArgument, "a state file path is required",
                      ErrorDetail{.operation = "open"});
  }
  if (!options.read_only && options.writer_id.empty()) {
    return make_error(ErrorCode::EmptyValue,
                      "a writable store requires a writer identity",
                      ErrorDetail{.operation = "open"});
  }
  return Status{};
}

}  // namespace

// ---------------------------------------------------------------------------
// Enum and report rendering
// ---------------------------------------------------------------------------

std::string_view write_stage_name(WriteStage stage) noexcept {
  switch (stage) {
    case WriteStage::AfterLockAcquired:
      return "after_lock_acquired";
    case WriteStage::AfterTempCreated:
      return "after_temp_created";
    case WriteStage::AfterTempWritten:
      return "after_temp_written";
    case WriteStage::AfterTempSynced:
      return "after_temp_synced";
    case WriteStage::AfterPreviousPublished:
      return "after_previous_published";
    case WriteStage::BeforePublishRename:
      return "before_publish_rename";
    case WriteStage::AfterPublishRename:
      return "after_publish_rename";
    case WriteStage::AfterTempRetired:
      return "after_temp_retired";
  }
  return "unknown";
}

Result<WriteStage> parse_write_stage(std::string_view text) {
  for (std::size_t i = 0; i < kWriteStageCount; ++i) {
    const auto stage = static_cast<WriteStage>(i);
    if (write_stage_name(stage) == text) {
      return stage;
    }
  }
  return make_error(ErrorCode::InvalidEnumValue, "unknown write stage",
                    ErrorDetail{.operation = "WriteStage", .subject = std::string(text)});
}

std::string_view writer_lock_state_name(WriterLockState state) noexcept {
  switch (state) {
    case WriterLockState::Unlocked:
      return "unlocked";
    case WriterLockState::HeldByThisProcess:
      return "held_by_this_process";
    case WriterLockState::HeldByAnotherProcess:
      return "held_by_another_process";
    case WriterLockState::Invalid:
      return "invalid";
  }
  return "unknown";
}

std::string_view recovery_action_name(RecoveryAction action) noexcept {
  switch (action) {
    case RecoveryAction::FreshStore:
      return "fresh_store";
    case RecoveryAction::LoadedCurrent:
      return "loaded_current";
    case RecoveryAction::LoadedPrevious:
      return "loaded_previous";
  }
  return "unknown";
}

std::string WriterLockInfo::to_text() const {
  std::string out(writer_lock_state_name(state));
  if (!writer_id.empty()) {
    out.append(" writer=");
    out.append(writer_id.text());
  }
  if (!incarnation.empty()) {
    out.append(" incarnation=");
    out.append(incarnation);
  }
  if (pid != 0) {
    out.append(" pid=");
    out.append(std::to_string(pid));
  }
  if (epoch.value() != 0) {
    out.append(" epoch=");
    out.append(std::to_string(epoch.value()));
  }
  if (acquired_at_unix_ns != 0) {
    out.append(" acquired_at_unix_ns=");
    out.append(std::to_string(acquired_at_unix_ns));
  }
  if (!taken_over_from.empty()) {
    out.append(" taken_over_from=");
    out.append(taken_over_from.text());
    out.append("@epoch=");
    out.append(std::to_string(taken_over_epoch.value()));
  }
  if (!detail.empty()) {
    out.append(" detail=");
    out.append(detail);
  }
  return out;
}

std::string RecoveryReport::to_text() const {
  std::string out(recovery_action_name(action));
  out.append(" epoch=");
  out.append(std::to_string(epoch.value()));
  out.append(" sequence=");
  out.append(std::to_string(sequence.value()));
  out.append(" racks=");
  out.append(std::to_string(rack_count));
  out.append(" members=");
  out.append(std::to_string(member_count));
  if (current_rejection.has_value()) {
    out.append(" current_rejected=");
    out.append(code_name(current_rejection->code));
    out.push_back(':');
    out.append(current_rejection->message);
  }
  for (const std::string& name : retired_temp_files) {
    out.append(" retired_temp=");
    out.append(name);
  }
  for (const std::string& note : notes) {
    out.append(" note=");
    out.append(note);
  }
  return out;
}

std::string StateFileInfo::to_text() const {
  std::string out = "format=";
  out.append(std::to_string(format_version));
  out.append(" layout=");
  out.append(std::to_string(snapshot_layout_version));
  out.append(" slots_per_unit=");
  out.append(std::to_string(mount_slots_per_rack_unit));
  out.append(" epoch=");
  out.append(std::to_string(epoch.value()));
  out.append(" sequence=");
  out.append(std::to_string(sequence.value()));
  out.append(" racks=");
  out.append(std::to_string(rack_count));
  out.append(" members=");
  out.append(std::to_string(member_count));
  out.append(" bytes=");
  out.append(std::to_string(byte_size));
  out.append(" digest=");
  out.append(payload_digest.to_hex());
  return out;
}

// ---------------------------------------------------------------------------
// Store implementation
// ---------------------------------------------------------------------------

struct RackStore::Impl {
  StoreOptions options{};
  // Serializes every mutation end to end, including the durable flush, and
  // stops a query from observing a mutation that has been applied in memory but
  // not yet published. Lock order is store mutex, then registry mutex; the
  // registry never calls into the store, so the order cannot invert.
  mutable std::shared_mutex mutex{};
  RackRegistry registry{};
  RecoveryReport recovery{};
  WriterLockInfo lock_info{};
  bool holds_lock = false;
  bool closed = false;
  StoreEpoch epoch{};
  StoreSequence sequence{};
  std::string incarnation{};
  std::vector<std::filesystem::path> owned_temp_files{};
  std::atomic<std::uint64_t> temp_counter{0};

  [[nodiscard]] std::filesystem::path previous_path() const {
    return previous_path_for(options.path);
  }
  [[nodiscard]] std::filesystem::path lock_path() const { return lock_path_for(options.path); }

  [[nodiscard]] std::filesystem::path state_temp_path() {
    const std::uint64_t counter = temp_counter.fetch_add(1) + 1;
    std::filesystem::path temp = options.path;
    temp += ".tmp-state-" + incarnation + "-" + std::to_string(counter);
    return temp;
  }

  [[nodiscard]] std::filesystem::path previous_temp_path() {
    const std::uint64_t counter = temp_counter.fetch_add(1) + 1;
    std::filesystem::path temp = options.path;
    temp += ".tmp-prev-" + incarnation + "-" + std::to_string(counter);
    return temp;
  }

  [[nodiscard]] std::filesystem::path lock_temp_path() {
    const std::uint64_t counter = temp_counter.fetch_add(1) + 1;
    std::filesystem::path temp = options.path;
    temp += ".lock.tmp-" + incarnation + "-" + std::to_string(counter);
    return temp;
  }

  void fire(WriteStage stage) {
    if (!options.fault_hook) {
      return;
    }
    try {
      options.fault_hook(stage);
    } catch (const std::exception& error) {
      throw RackError{ErrorCode::IoFailure,
                      "fault injected at " + std::string(write_stage_name(stage)) + ": " +
                          error.what(),
                      ErrorDetail{.operation = "publish"}};
    } catch (...) {
      throw RackError{ErrorCode::IoFailure,
                      "fault injected at " + std::string(write_stage_name(stage)),
                      ErrorDetail{.operation = "publish"}};
    }
  }

  void retire_temp(const std::filesystem::path& path) {
    (void)internal::remove_file(path);
    owned_temp_files.erase(std::remove(owned_temp_files.begin(), owned_temp_files.end(), path),
                           owned_temp_files.end());
  }

  // Retires every temporary file this store still owns. A publication that is
  // abandoned while the process is still alive must not leave residue behind,
  // so the failure path calls this before it reports the rejection.
  void retire_owned_temps() {
    for (const std::filesystem::path& temp : owned_temp_files) {
      (void)internal::remove_file(temp);
    }
    owned_temp_files.clear();
  }

  Status retire_and(const std::filesystem::path& path, const Status& status) {
    retire_temp(path);
    return status;
  }

// The publication sequence. Every step that can leave a partial artefact is
// separated by a named crash point, so a test can interrupt the sequence at
// exactly that boundary and prove that recovery stays conservative. `committed`
// is set once the atomic replace has succeeded, which is the authoritative
// completion boundary of the whole transaction.
  Status publish_snapshot(const RackSnapshot& snapshot, bool& committed) {
    committed = false;
    const std::vector<std::uint8_t> bytes = snapshot.to_bytes();
    const StateDigest intended = snapshot.state_digest();
    const std::filesystem::path temp = state_temp_path();
    owned_temp_files.push_back(temp);

    Status status = internal::create_file_new(temp);
    if (!status) {
      return retire_and(temp, status);
    }
    fire(WriteStage::AfterTempCreated);

    status = internal::write_file_data(temp, bytes, false);
    if (!status) {
      return retire_and(temp, status);
    }
    fire(WriteStage::AfterTempWritten);

    status = internal::flush_file(temp);
    if (!status) {
      return retire_and(temp, status);
    }
    fire(WriteStage::AfterTempSynced);

    // The new generation is read back and verified before it is allowed to
    // become authoritative.
    auto read_back = internal::read_file(temp, kMaxStateFileBytes);
    if (!read_back) {
      return retire_and(temp, read_back.error());
    }
    auto decoded = RackSnapshot::from_bytes(read_back.value().data(), read_back.value().size());
    if (!decoded) {
      return retire_and(temp, decoded.error());
    }
    if (!(decoded.value().state_digest() == intended)) {
      return retire_and(
          temp, make_error(ErrorCode::IntegrityCheckFailed,
                           "the newly written generation did not read back identically",
                           ErrorDetail{.operation = "publish", .actual = bytes.size()}));
    }

    if (options.retain_previous && internal::file_exists(options.path)) {
      auto current_bytes = internal::read_file(options.path, kMaxStateFileBytes);
      // The retained generation is only replaced by a current generation that
      // actually verifies. Overwriting a known-good fallback with a damaged
      // file would destroy the last line of defence exactly when it is needed.
      if (current_bytes) {
        const auto verified = RackSnapshot::from_bytes(current_bytes.value().data(),
                                                       current_bytes.value().size());
        if (verified) {
          const std::filesystem::path previous_temp = previous_temp_path();
          owned_temp_files.push_back(previous_temp);
          Status previous_status = internal::create_file_new(previous_temp);
          if (!previous_status) {
            return retire_and(temp, previous_status);
          }
          previous_status = internal::write_file_data(previous_temp, current_bytes.value(), true);
          if (!previous_status) {
            retire_temp(previous_temp);
            return retire_and(temp, previous_status);
          }
          previous_status = internal::replace_file_atomic(previous_temp, previous_path());
          retire_temp(previous_temp);
          if (!previous_status) {
            return retire_and(temp, previous_status);
          }
          internal::sync_directory(options.path);
          fire(WriteStage::AfterPreviousPublished);
        }
      }
    }

    fire(WriteStage::BeforePublishRename);

    // Writer authority is re-checked immediately before the atomic replace, so
    // a writer that lost its epoch while it was planning cannot publish.
    auto lock_now = read_lock_record(lock_path());
    if (!lock_now) {
      return retire_and(temp, lock_now.error());
    }
    const WriterLockInfo& held = lock_now.value();
    if (held.state != WriterLockState::HeldByAnotherProcess ||
        held.incarnation != incarnation || !(held.epoch == epoch)) {
      return retire_and(
          temp,
          make_error(ErrorCode::StaleWriterFenced,
                     "this writer no longer holds epoch " + std::to_string(epoch.value()) +
                         " for " + path_text(options.path) +
                         ", so the new generation was not published",
                     ErrorDetail{.operation = "publish",
                                 .subject = path_text(options.path),
                                 .expected = epoch.value(),
                                 .actual = held.epoch.value()}));
    }

    status = internal::replace_file_atomic(temp, options.path);
    if (!status) {
      return retire_and(temp, status);
    }
    // The authoritative completion boundary. Once the atomic replace has
    // succeeded the new generation is durable, so nothing after this point may
    // be reported as a reason to roll the in-memory image back.
    committed = true;
    retire_temp(temp);
    internal::sync_directory(options.path);
    fire(WriteStage::AfterPublishRename);
    fire(WriteStage::AfterTempRetired);
    return Status{};
  }

  // Applies one registry mutation and publishes the result. A mutation that
  // cannot be published is rolled back in memory, so the in-memory image never
  // runs ahead of the durable generation.
  template <typename Request>
  Result<MutationReceipt> mutate(const Request& request,
                                 Result<MutationReceipt> (RackRegistry::*operation)(
                                     const Request&)) {
    std::unique_lock lock(mutex);
    if (closed) {
      return make_error(ErrorCode::IoFailure, "the store is closed",
                        ErrorDetail{.operation = "mutate"});
    }
    if (options.read_only) {
      return make_error(ErrorCode::ReadOnlyStore,
                        "the store was opened read-only and refuses mutation",
                        ErrorDetail{.operation = "mutate", .subject = path_text(options.path)});
    }
    if (sequence.is_max()) {
      return make_error(ErrorCode::SequenceRegression,
                        "the publication sequence is exhausted and cannot be advanced",
                        ErrorDetail{.operation = "mutate", .subject = path_text(options.path)});
    }

    const RackSnapshot before = registry.snapshot(options.producer);
    auto result = (registry.*operation)(request);
    if (!result) {
      return result;
    }

    const StoreSequence next = sequence.next();
    const RackSnapshot stamped =
        registry.snapshot(options.producer).with_store_position(epoch, next);

    bool committed = false;
    Status published = Status{};
    try {
      published = publish_snapshot(stamped, committed);
    } catch (const RackError& error) {
      published = error;
    } catch (const std::exception& error) {
      published = make_error(ErrorCode::IoFailure,
                             std::string("publication failed: ") + error.what(),
                             ErrorDetail{.operation = "publish"});
    }

    if (!published && !committed) {
      // Nothing crossed the completion boundary, so nothing of this attempt may
      // survive: the temporary artefacts are retired and the in-memory image is
      // returned to exactly the generation that is still authoritative.
      retire_owned_temps();
      const Status rolled_back = registry.restore(before);
      if (!rolled_back) {
        return make_error(ErrorCode::IoFailure,
                          "publication failed and the in-memory image could not be rolled back: " +
                              rolled_back.error().message,
                          ErrorDetail{.operation = "publish"});
      }
      return published.error();
    }

    // Past the completion boundary the generation is authoritative. A failure
    // reported after it describes post-commit housekeeping rather than a
    // rejected mutation, so it is not allowed to undo work the durable store
    // has already accepted.
    sequence = next;
    return result;
  }

  // Acquires writer authority. Returns the lock this process owns.
  Result<WriterLockInfo> acquire_lock() {
    const std::filesystem::path lock_file = lock_path();
    for (std::size_t attempt = 0; attempt < kLockAcquireAttempts; ++attempt) {
      auto existing = read_lock_record(lock_file);
      if (!existing) {
        return existing.error();
      }

      WriterLockInfo candidate;
      candidate.writer_id = options.writer_id;
      candidate.incarnation = incarnation;
      candidate.pid = internal::current_process_id();
      candidate.process_start_marker = internal::process_start_marker(candidate.pid).value_or(0);
      candidate.acquired_at_unix_ns = internal::system_time_unix_ns();

      StoreEpoch base = epoch;
      bool create_only = false;
      if (existing.value().state == WriterLockState::HeldByAnotherProcess) {
        if (holder_is_alive(existing.value())) {
          return make_error(ErrorCode::WriterLockHeld,
                            "writer " +
                                (existing.value().writer_id.empty()
                                     ? std::string("<unknown>")
                                     : existing.value().writer_id.text()) +
                                " in process " + std::to_string(existing.value().pid) +
                                " already holds the writer lock for " + path_text(options.path),
                            ErrorDetail{.operation = "open",
                                        .subject = path_text(options.path),
                                        .related = existing.value().writer_id.text(),
                                        .actual = existing.value().pid});
        }
        if (!options.adopt_abandoned_lock) {
          return make_error(ErrorCode::WriterLockHeld,
                            "the writer lock for " + path_text(options.path) +
                                " is abandoned and adoption is disabled",
                            ErrorDetail{.operation = "open",
                                        .subject = path_text(options.path),
                                        .related = existing.value().writer_id.text()});
        }
        if (base < existing.value().epoch) {
          base = existing.value().epoch;
        }
        candidate.taken_over_from = existing.value().writer_id;
        candidate.taken_over_epoch = existing.value().epoch;
        candidate.detail = "adopted an abandoned writer lock";
      } else if (existing.value().state == WriterLockState::Invalid) {
        if (!options.adopt_abandoned_lock) {
          return make_error(ErrorCode::WriterLockInvalid,
                            "the writer lock for " + path_text(options.path) +
                                " is not verifiable and adoption is disabled",
                            ErrorDetail{.operation = "open",
                                        .subject = path_text(options.path),
                                        .related = existing.value().detail});
        }
        candidate.detail = "replaced a writer lock that failed verification: " +
                           existing.value().detail;
      } else {
        create_only = true;
      }

      if (base.is_max()) {
        return make_error(ErrorCode::StoreEpochRegression,
                          "the store epoch is exhausted and cannot be advanced",
                          ErrorDetail{.operation = "open", .subject = path_text(options.path)});
      }
      candidate.epoch = base.next();

      const std::filesystem::path temp = lock_temp_path();
      const std::string text = encode_lock_record(candidate);
      const std::vector<std::uint8_t> bytes(text.begin(), text.end());
      Status status = internal::create_file_new_with_data(temp, bytes);
      if (!status) {
        return status.error();
      }
      if (create_only) {
        status = internal::publish_file_new(temp, lock_file);
        if (!status) {
          (void)internal::remove_file(temp);
          if (status.error().code == ErrorCode::WriterLockHeld) {
            continue;  // another process created the lock first; re-read and retry
          }
          return status.error();
        }
      } else {
        status = internal::replace_file_atomic(temp, lock_file);
        if (!status) {
          (void)internal::remove_file(temp);
          return status.error();
        }
      }
      internal::sync_directory(lock_file);

      // Ownership is confirmed by reading the file back: the last writer to
      // replace it wins, and every earlier one sees a foreign incarnation and
      // retries.
      auto confirm = read_lock_record(lock_file);
      if (confirm && confirm.value().state == WriterLockState::HeldByAnotherProcess &&
          confirm.value().incarnation == incarnation && confirm.value().epoch == candidate.epoch) {
        candidate.state = WriterLockState::HeldByThisProcess;
        return candidate;
      }
    }
    return make_error(ErrorCode::WriterLockHeld,
                      "could not take writer authority for " + path_text(options.path) +
                          " after several attempts",
                      ErrorDetail{.operation = "open", .subject = path_text(options.path)});
  }
};

namespace {

struct LoadedState {
  RecoveryReport report{};
  std::optional<RackSnapshot> snapshot{};
  // True when at least one state file was present, whether or not it verified.
  bool state_files_present = false;
};

Result<std::optional<RackSnapshot>> load_state_file(const std::filesystem::path& path) {
  if (!internal::file_exists(path)) {
    return std::optional<RackSnapshot>{};
  }
  auto bytes = internal::read_file(path, kMaxStateFileBytes);
  if (!bytes) {
    return bytes.error();
  }
  auto snapshot = RackSnapshot::from_bytes(bytes.value().data(), bytes.value().size());
  if (!snapshot) {
    return snapshot.error();
  }
  return std::optional<RackSnapshot>{std::move(snapshot).value()};
}

LoadedState recover_state(const std::filesystem::path& path) {
  LoadedState loaded;
  const std::filesystem::path previous = previous_path_for(path);
  const bool current_exists = internal::file_exists(path);
  const bool previous_exists = internal::file_exists(previous);
  loaded.state_files_present = current_exists || previous_exists;

  if (!current_exists && !previous_exists) {
    loaded.report.action = RecoveryAction::FreshStore;
    loaded.report.notes.emplace_back("no state file exists, so an empty store was created");
    return loaded;
  }

  if (current_exists) {
    auto current = load_state_file(path);
    if (current) {
      if (current.value().has_value()) {
        loaded.snapshot = std::move(current.value());
        loaded.report.action = RecoveryAction::LoadedCurrent;
        loaded.report.notes.emplace_back(
            "the current generation verified and became authoritative");
        return loaded;
      }
      loaded.report.notes.emplace_back("the state file disappeared while it was being opened");
    } else {
      loaded.report.current_rejection = current.error();
      loaded.report.notes.emplace_back("the current generation was rejected: " +
                                       current.error().message);
    }
  }

  if (previous_exists) {
    auto fallback = load_state_file(previous);
    if (fallback && fallback.value().has_value()) {
      loaded.snapshot = std::move(fallback.value());
      loaded.report.action = RecoveryAction::LoadedPrevious;
      loaded.report.notes.emplace_back(
          "the retained previous generation verified and became authoritative");
      return loaded;
    }
    if (!fallback) {
      loaded.report.notes.emplace_back("the retained previous generation was rejected: " +
                                       fallback.error().message);
    }
  }
  return loaded;
}

std::vector<std::filesystem::path> list_stale_temps(const std::filesystem::path& path) {
  const std::filesystem::path directory = directory_of(path);
  std::vector<std::filesystem::path> temps =
      internal::list_directory_matching(directory, state_temp_prefix(path));
  const auto lock_temps = internal::list_directory_matching(directory, lock_temp_prefix(path));
  temps.insert(temps.end(), lock_temps.begin(), lock_temps.end());
  return temps;
}

}  // namespace

RackStore::RackStore(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

RackStore::~RackStore() {
  if (impl_ != nullptr) {
    (void)close();
  }
}

Result<StateFileInfo> RackStore::inspect(const std::filesystem::path& path) {
  auto bytes = internal::read_file(path, kMaxStateFileBytes);
  if (!bytes) {
    return bytes.error();
  }
  auto snapshot = RackSnapshot::from_bytes(bytes.value().data(), bytes.value().size());
  if (!snapshot) {
    return snapshot.error();
  }
  StateFileInfo info;
  info.format_version = kStateFormatVersion;
  info.snapshot_layout_version = kSnapshotLayoutVersion;
  info.mount_slots_per_rack_unit = kMountSlotsPerRackUnit;
  info.epoch = snapshot.value().store_epoch();
  info.sequence = snapshot.value().store_sequence();
  info.rack_count = snapshot.value().rack_count();
  info.member_count = snapshot.value().member_count();
  info.payload_digest = snapshot.value().state_digest();
  info.byte_size = bytes.value().size();
  return info;
}

Result<WriterLockInfo> RackStore::query_writer_lock(const std::filesystem::path& path) {
  auto info = read_lock_record(lock_path_for(path));
  if (!info) {
    return info.error();
  }
  WriterLockInfo value = info.value();
  if (value.state == WriterLockState::HeldByAnotherProcess) {
    if (value.pid == 0) {
      // A lock with no process behind it is the record a clean release leaves:
      // authority is free, and the epoch it records still fences anything older.
      value.state = WriterLockState::Unlocked;
      value.detail = "the last writer released authority at epoch " +
                     std::to_string(value.epoch.value());
    } else if (holder_is_alive(value)) {
      value.detail = value.detail.empty() ? "the recorded writer process is running" : value.detail;
    } else {
      value.state = WriterLockState::Invalid;
      value.detail = "the recorded writer process is not running, so the lock is abandoned";
    }
  }
  return value;
}

Result<WriterLockInfo> RackStore::force_takeover(const std::filesystem::path& path,
                                                 WriterId new_writer_id,
                                                 std::uint64_t observed_at_unix_ns) {
  if (path.empty()) {
    return make_error(ErrorCode::InvalidArgument, "a state file path is required",
                      ErrorDetail{.operation = "force_takeover"});
  }
  if (new_writer_id.empty()) {
    return make_error(ErrorCode::EmptyValue,
                      "force takeover requires the identity of the new writer",
                      ErrorDetail{.operation = "force_takeover",
                                  .subject = path_text(path)});
  }

  StoreEpoch base{};
  if (internal::file_exists(path)) {
    auto current = load_state_file(path);
    if (current && current.value().has_value()) {
      base = current.value()->store_epoch();
    }
  }

  WriterLockInfo info;
  info.writer_id = new_writer_id;
  info.incarnation = internal::random_hex(16);
  info.pid = 0;
  info.process_start_marker = 0;
  info.acquired_at_unix_ns =
      observed_at_unix_ns != 0 ? observed_at_unix_ns : internal::system_time_unix_ns();

  const std::filesystem::path lock_file = lock_path_for(path);
  auto existing = read_lock_record(lock_file);
  if (!existing) {
    return existing.error();
  }
  if (existing.value().state == WriterLockState::HeldByAnotherProcess) {
    if (base < existing.value().epoch) {
      base = existing.value().epoch;
    }
    info.taken_over_from = existing.value().writer_id;
    info.taken_over_epoch = existing.value().epoch;
    info.detail = "writer authority was taken over by explicit operator fencing";
  } else if (existing.value().state == WriterLockState::Invalid) {
    info.detail = "an unverifiable writer lock was replaced by explicit operator fencing";
  } else {
    info.detail = "writer authority was established by explicit operator fencing";
  }
  if (base.is_max()) {
    return make_error(ErrorCode::StoreEpochRegression,
                      "the store epoch is exhausted and cannot be advanced",
                      ErrorDetail{.operation = "force_takeover", .subject = path_text(path)});
  }
  info.epoch = base.next();

  std::filesystem::path temp = path;
  temp += ".lock.tmp-force-" + info.incarnation;
  const std::string text = encode_lock_record(info);
  const std::vector<std::uint8_t> bytes(text.begin(), text.end());
  Status status = internal::create_file_new_with_data(temp, bytes);
  if (!status) {
    return status.error();
  }
  status = internal::replace_file_atomic(temp, lock_file);
  if (!status) {
    (void)internal::remove_file(temp);
    return status.error();
  }
  internal::sync_directory(lock_file);

  auto confirm = read_lock_record(lock_file);
  if (!confirm || confirm.value().state != WriterLockState::HeldByAnotherProcess ||
      confirm.value().incarnation != info.incarnation) {
    return make_error(ErrorCode::WriterLockInvalid,
                      "the writer lock could not be replaced for " + path_text(path),
                      ErrorDetail{.operation = "force_takeover", .subject = path_text(path)});
  }
  info.state = WriterLockState::HeldByAnotherProcess;
  return info;
}

Result<std::unique_ptr<RackStore>> RackStore::open(const StoreOptions& options) {
  const Status options_status = validate_options(options);
  if (!options_status) {
    return options_status.error();
  }

  auto impl = std::make_unique<Impl>();
  impl->options = options;
  impl->incarnation = internal::random_hex(16);

  LoadedState loaded = recover_state(options.path);
  if (!loaded.snapshot.has_value()) {
    if (loaded.state_files_present) {
      if (loaded.report.current_rejection.has_value()) {
        return make_error(loaded.report.current_rejection->code,
                          "no authoritative generation could be established: " +
                              loaded.report.current_rejection->message,
                          loaded.report.current_rejection->detail);
      }
      return make_error(ErrorCode::NoAuthoritativeState,
                        "no authoritative generation could be established for " +
                            path_text(options.path),
                        ErrorDetail{.operation = "open", .subject = path_text(options.path)});
    }
    if (!options.create_if_missing) {
      return make_error(ErrorCode::NoAuthoritativeState,
                        "no state file exists at " + path_text(options.path) +
                            " and creating one was not permitted",
                        ErrorDetail{.operation = "open", .subject = path_text(options.path)});
    }
  }

  impl->epoch = loaded.snapshot.has_value() ? loaded.snapshot->store_epoch() : StoreEpoch{};
  impl->sequence = loaded.snapshot.has_value() ? loaded.snapshot->store_sequence()
                                               : StoreSequence::initial();

  if (options.read_only) {
    auto lock_info = query_writer_lock(options.path);
    if (!lock_info) {
      return lock_info.error();
    }
    impl->lock_info = lock_info.value();
  } else {
    auto lock_info = impl->acquire_lock();
    if (!lock_info) {
      return lock_info.error();
    }
    impl->lock_info = lock_info.value();
    impl->holds_lock = true;
    impl->epoch = lock_info.value().epoch;

    // Temporary files left by an interrupted publication are retired now that
    // no other writer can be running. They were never authoritative.
    for (const std::filesystem::path& temp : list_stale_temps(options.path)) {
      const Status removed = internal::remove_file(temp);
      if (removed) {
        impl->recovery.retired_temp_files.push_back(temp.filename().string());
      }
    }
    internal::sync_directory(options.path);
    impl->fire(WriteStage::AfterLockAcquired);
  }

  const Status applied = impl->registry.restore(loaded.snapshot.value_or(RackSnapshot{}));
  if (!applied) {
    if (impl->holds_lock) {
      (void)internal::remove_file(impl->lock_path());
    }
    return make_error(applied.error().code,
                      "the recovered generation is not usable: " + applied.error().message,
                      applied.error().detail);
  }

  const std::vector<std::string> retired = impl->recovery.retired_temp_files;
  impl->recovery = loaded.report;
  impl->recovery.retired_temp_files = retired;
  impl->recovery.epoch = impl->epoch;
  impl->recovery.sequence = impl->sequence;
  impl->recovery.rack_count = impl->registry.rack_count();
  impl->recovery.member_count = impl->registry.stats().member_count;

  if (options.retain_previous && internal::file_exists(previous_path_for(options.path))) {
    auto retained = load_state_file(previous_path_for(options.path));
    if (retained && retained.value().has_value()) {
      impl->recovery.notes.emplace_back("the retained previous generation is available for diffing");
    }
  }

  return std::unique_ptr<RackStore>(new RackStore(std::move(impl)));
}

// ---------------------------------------------------------------------------
// Public mutations
// ---------------------------------------------------------------------------

Result<MutationReceipt> RackStore::register_rack(const RegisterRackRequest& request) {
  return impl_->mutate(request, &RackRegistry::register_rack);
}

Result<MutationReceipt> RackStore::set_rack_structure(const SetRackStructureRequest& request) {
  return impl_->mutate(request, &RackRegistry::set_rack_structure);
}

Result<MutationReceipt> RackStore::transition_lifecycle(const TransitionLifecycleRequest& request) {
  return impl_->mutate(request, &RackRegistry::transition_lifecycle);
}

Result<MutationReceipt> RackStore::insert_member(const InsertMemberRequest& request) {
  return impl_->mutate(request, &RackRegistry::insert_member);
}

Result<MutationReceipt> RackStore::remove_member(const RemoveMemberRequest& request) {
  return impl_->mutate(request, &RackRegistry::remove_member);
}

Result<MutationReceipt> RackStore::move_member(const MoveMemberRequest& request) {
  return impl_->mutate(request, &RackRegistry::move_member);
}

Result<MutationReceipt> RackStore::replace_member(const ReplaceMemberRequest& request) {
  return impl_->mutate(request, &RackRegistry::replace_member);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

bool RackStore::contains_rack(const RackId& rack_id) const {
  std::shared_lock lock(impl_->mutex);
  return impl_->registry.contains_rack(rack_id);
}

std::size_t RackStore::rack_count() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->registry.rack_count();
}

Result<RackView> RackStore::rack(const RackId& rack_id) const {
  std::shared_lock lock(impl_->mutex);
  return impl_->registry.rack(rack_id);
}

std::vector<RackView> RackStore::racks() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->registry.racks();
}

Result<std::vector<MemberRecord>> RackStore::members(const RackId& rack_id,
                                                     MemberOrder order) const {
  std::shared_lock lock(impl_->mutex);
  return impl_->registry.members(rack_id, order);
}

Result<std::optional<MemberRecord>> RackStore::member(const RackId& rack_id,
                                                      const RackMemberId& member_id) const {
  std::shared_lock lock(impl_->mutex);
  return impl_->registry.member(rack_id, member_id);
}

Result<std::vector<OccupancyRecord>> RackStore::occupancy(const RackId& rack_id) const {
  std::shared_lock lock(impl_->mutex);
  return impl_->registry.occupancy(rack_id);
}

Result<std::vector<FreeSpan>> RackStore::free_spans(const RackId& rack_id) const {
  std::shared_lock lock(impl_->mutex);
  return impl_->registry.free_spans(rack_id);
}

Result<std::vector<SharedSpanAvailability>> RackStore::shared_availability(
    const RackId& rack_id) const {
  std::shared_lock lock(impl_->mutex);
  return impl_->registry.shared_availability(rack_id);
}

Result<CompatibilityReport> RackStore::evaluate_compatibility(
    const RackId& rack_id, const MemberRequirements& requirements) const {
  std::shared_lock lock(impl_->mutex);
  return impl_->registry.evaluate_compatibility(rack_id, requirements);
}

Result<RackDiff> RackStore::diff_generations(const RackId& rack_id, RackGeneration from) const {
  std::shared_lock lock(impl_->mutex);
  return impl_->registry.diff_generations(rack_id, from);
}

Result<RackDiff> RackStore::diff_generations(const RackId& rack_id, RackGeneration from,
                                             RackGeneration to) const {
  std::shared_lock lock(impl_->mutex);
  return impl_->registry.diff_generations(rack_id, from, to);
}

std::vector<RejectionRecord> RackStore::rejections() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->registry.rejections();
}

RegistryStats RackStore::stats() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->registry.stats();
}

RackSnapshot RackStore::snapshot() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->registry.snapshot(impl_->options.producer)
      .with_store_position(impl_->epoch, impl_->sequence);
}

Result<SnapshotDiff> RackStore::diff_with_previous() const {
  std::shared_lock lock(impl_->mutex);
  if (!impl_->options.retain_previous) {
    return make_error(ErrorCode::NoAuthoritativeState,
                      "this store does not retain a previous generation",
                      ErrorDetail{.operation = "diff_with_previous",
                                  .subject = path_text(impl_->options.path)});
  }
  const std::filesystem::path previous = impl_->previous_path();
  if (!internal::file_exists(previous)) {
    return make_error(ErrorCode::NoAuthoritativeState,
                      "no previous generation has been published yet",
                      ErrorDetail{.operation = "diff_with_previous",
                                  .subject = path_text(impl_->options.path)});
  }
  auto retained = load_state_file(previous);
  if (!retained) {
    return retained.error();
  }
  if (!retained.value().has_value()) {
    return make_error(ErrorCode::NoAuthoritativeState,
                      "the retained previous generation is no longer present",
                      ErrorDetail{.operation = "diff_with_previous",
                                  .subject = path_text(impl_->options.path)});
  }
  return diff_snapshots(retained.value().value(), impl_->registry.snapshot(
                                                      impl_->options.producer)
                                                      .with_store_position(impl_->epoch,
                                                                           impl_->sequence));
}

// ---------------------------------------------------------------------------
// Store state
// ---------------------------------------------------------------------------

const RecoveryReport& RackStore::recovery() const { return impl_->recovery; }

WriterLockInfo RackStore::writer_lock() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->lock_info;
}

bool RackStore::holds_writer_authority() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->holds_lock;
}

bool RackStore::is_read_only() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->options.read_only;
}

StoreEpoch RackStore::epoch() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->epoch;
}

StoreSequence RackStore::sequence() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->sequence;
}

const std::filesystem::path& RackStore::path() const { return impl_->options.path; }

Status RackStore::close() {
  std::unique_lock lock(impl_->mutex);
  if (impl_->closed) {
    return Status{};
  }
  impl_->closed = true;

  for (const std::filesystem::path& temp : impl_->owned_temp_files) {
    (void)internal::remove_file(temp);
  }
  impl_->owned_temp_files.clear();

  if (impl_->holds_lock) {
    auto current = read_lock_record(impl_->lock_path());
    if (current && current.value().state == WriterLockState::HeldByAnotherProcess &&
        current.value().incarnation == impl_->incarnation) {
      // Release rewrites the record instead of removing it. The released record
      // leaves the epoch behind, so epochs stay strictly increasing across clean
      // restarts, and it records which writer held authority last.
      WriterLockInfo released = current.value();
      released.pid = 0;
      released.process_start_marker = 0;
      released.state = WriterLockState::HeldByAnotherProcess;
      released.detail = "released";
      const std::string text = encode_lock_record(released);
      const std::vector<std::uint8_t> bytes(text.begin(), text.end());
      std::filesystem::path temp = impl_->options.path;
      temp += ".lock.tmp-release-" + impl_->incarnation;
      const Status written = internal::create_file_new_with_data(temp, bytes);
      if (written) {
        const Status replaced = internal::replace_file_atomic(temp, impl_->lock_path());
        if (!replaced) {
          (void)internal::remove_file(temp);
        }
      }
      internal::sync_directory(impl_->options.path);
    }
    impl_->holds_lock = false;
  }
  return Status{};
}

}  // namespace rackregistry
