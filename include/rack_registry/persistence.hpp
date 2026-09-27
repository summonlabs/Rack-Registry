// Rack Registry - durable store: transactional publication and writer fencing.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rack_registry/digest.hpp"
#include "rack_registry/export.hpp"
#include "rack_registry/ids.hpp"
#include "rack_registry/registry.hpp"
#include "rack_registry/requests.hpp"
#include "rack_registry/result.hpp"

namespace rackregistry {

// Named boundaries between the durable steps of a publication. A crash at any
// of them must leave either the previous or the new generation authoritative,
// never a mixture.
enum class WriteStage : std::uint8_t {
  AfterLockAcquired = 0,
  AfterTempCreated = 1,
  AfterTempWritten = 2,
  AfterTempSynced = 3,
  AfterPreviousPublished = 4,
  BeforePublishRename = 5,
  AfterPublishRename = 6,
  AfterTempRetired = 7,
};

inline constexpr std::size_t kWriteStageCount = 8;

[[nodiscard]] RACK_REGISTRY_API std::string_view write_stage_name(WriteStage stage) noexcept;
[[nodiscard]] RACK_REGISTRY_API Result<WriteStage> parse_write_stage(std::string_view text);

// Failure-injection seam. It is empty in every normal configuration: the store
// then performs no indirect call at all along the publication path. Tests
// install a hook that throws or terminates the process to simulate a crash at
// exactly one durable step boundary.
//
// A hook runs while the store's writer mutex and the registry's exclusive lock
// are held. It must not call back into the store or into any registry; its only
// supported behaviours are to throw and to terminate the process.
using FaultHook = std::function<void(WriteStage)>;

struct StoreOptions {
  // Path of the state file. The store also owns "<path>.prev" and
  // "<path>.lock" and creates "<path>.tmp-<pid>-<counter>" while publishing.
  std::filesystem::path path{};

  // Identity of the writer that will own this store. Required for a writable
  // store and recorded in the writer lock file.
  WriterId writer_id{};

  // Identity of the component that produces state through this store. Recorded
  // verbatim in every published generation and in the snapshot digest.
  std::optional<SourceReference> producer{};

  // When true the store takes no writer authority: it loads whatever
  // generation is authoritative and refuses every mutation with ReadOnlyStore.
  // Several read-only stores may be open at the same time as one writer.
  bool read_only = false;

  // Retain the previous published generation as "<path>.prev". The retained
  // file is the fallback when the current file fails verification.
  bool retain_previous = true;

  // Create an empty store when no state file exists yet.
  bool create_if_missing = true;

  // Adopt an existing writer lock whose recorded process is provably gone, or
  // whose lock file is unreadable or fails its own integrity check. When false,
  // such a lock is reported as WriterLockHeld and the caller must decide.
  bool adopt_abandoned_lock = true;

  // See FaultHook.
  FaultHook fault_hook{};
};

enum class WriterLockState : std::uint8_t {
  // No lock file exists.
  Unlocked = 0,
  // The lock file exists and names this store's writer identity and process.
  HeldByThisProcess = 1,
  // The lock file exists and names a different, live process.
  HeldByAnotherProcess = 2,
  // The lock file exists but is unreadable, truncated, or fails its integrity
  // check. It is never trusted.
  Invalid = 3,
};

[[nodiscard]] RACK_REGISTRY_API std::string_view writer_lock_state_name(
    WriterLockState state) noexcept;

struct WriterLockInfo {
  WriterLockState state = WriterLockState::Unlocked;
  WriterId writer_id{};
  std::string incarnation{};
  std::uint64_t pid = 0;
  std::uint64_t process_start_marker = 0;
  StoreEpoch epoch{};
  std::uint64_t acquired_at_unix_ns = 0;
  WriterId taken_over_from{};
  StoreEpoch taken_over_epoch{};
  std::string detail{};

  [[nodiscard]] std::string to_text() const;
};

// What happened when the store was opened.
enum class RecoveryAction : std::uint8_t {
  // No state file existed and an empty store was created.
  FreshStore = 0,
  // The current state file verified and became authoritative.
  LoadedCurrent = 1,
  // The current state file was missing, unreadable or failed verification, and
  // the retained previous generation became authoritative instead.
  LoadedPrevious = 2,
  // Neither file verified. No state was accepted; the store is empty and the
  // current file was left untouched.
  NoStateAccepted = 3,
};

[[nodiscard]] RACK_REGISTRY_API std::string_view recovery_action_name(
    RecoveryAction action) noexcept;

struct RecoveryReport {
  RecoveryAction action = RecoveryAction::FreshStore;
  StoreEpoch epoch{};
  StoreSequence sequence{};
  std::size_t rack_count = 0;
  std::size_t member_count = 0;
  // Set when the current state file existed but was rejected, naming the code
  // and explanation. The rejected file is never modified.
  std::optional<RackError> current_rejection{};
  // Temporary files left behind by an interrupted publication, retired during
  // recovery.
  std::vector<std::string> retired_temp_files{};
  // Ordered notes describing what recovery did and why.
  std::vector<std::string> notes{};

  [[nodiscard]] std::string to_text() const;
};

// Metadata of a state file, read without taking writer authority.
struct StateFileInfo {
  std::uint32_t format_version = 0;
  std::uint32_t snapshot_layout_version = 0;
  std::uint32_t mount_slots_per_rack_unit = 0;
  StoreEpoch epoch{};
  StoreSequence sequence{};
  std::size_t rack_count = 0;
  std::size_t member_count = 0;
  StateDigest payload_digest{};
  std::uint64_t byte_size = 0;

  [[nodiscard]] std::string to_text() const;
};

// Durable, transactional store for authoritative rack state.
//
// Publication is a single sequence: plan, validate, serialize into a new
// temporary file, flush it, verify its integrity by reading it back, publish it
// atomically, then retire the superseded temporary state. A crash or partial
// write therefore leaves either the previous generation or the new one
// authoritative, never a mixture, and never a file that decodes into a
// half-applied occupancy change.
//
// Concurrency model: one writer per state file, enforced by a lock file that
// carries the writer identity, its process incarnation and the store epoch.
// The lock is re-validated immediately before the atomic publish, so a writer
// that lost its authority while it was planning cannot publish stale results.
// Within one process a writer mutex serializes every mutation end to end,
// including the durable flush.
class RACK_REGISTRY_API RackStore {
 public:
  // Opens a store. Fails with IoFailure when the state cannot be read or
  // written, with IntegrityCheckFailed or CorruptState when no generation can
  // be verified and the store was not allowed to start empty, and with
  // WriterLockHeld when another live writer owns the file.
  [[nodiscard]] static Result<std::unique_ptr<RackStore>> open(const StoreOptions& options);

  // Reads and validates the state file at `path` without taking writer
  // authority and without mutating anything on disk.
  [[nodiscard]] static Result<StateFileInfo> inspect(const std::filesystem::path& path);

  // Reports the current writer lock without taking it.
  [[nodiscard]] static Result<WriterLockInfo> query_writer_lock(const std::filesystem::path& path);

  // Operator fencing. Takes writer authority unconditionally for `new_writer_id`
  // and advances the store epoch, so any writer still holding the previous epoch
  // is refused at its next publication. The previous holder's identity and epoch
  // are recorded in the lock file, and the new lock names no live process, so the
  // next writer to open the store adopts it normally. This is the only path that
  // takes authority away from a live writer, and it is always an explicit
  // operator action.
  [[nodiscard]] static Result<WriterLockInfo> force_takeover(
      const std::filesystem::path& path, WriterId new_writer_id,
      std::uint64_t observed_at_unix_ns);

  ~RackStore();
  // A store is owned through the std::unique_ptr that open() returns, and it is
  // not movable: a moved-from store would hold no implementation and every
  // method would have to be defensive about it. Removing the operation removes
  // the whole class of mistake.
  RackStore(RackStore&&) = delete;
  RackStore& operator=(RackStore&&) = delete;
  RackStore(const RackStore&) = delete;
  RackStore& operator=(const RackStore&) = delete;

  // -------------------------------------------------------------------------
  // Mutations. Each is applied, durably published and only then reported as
  // accepted. A mutation that cannot be published is rolled back in memory and
  // reported as rejected, so the in-memory state never runs ahead of the
  // durable state.
  // -------------------------------------------------------------------------

  [[nodiscard]] Result<MutationReceipt> register_rack(const RegisterRackRequest& request);
  [[nodiscard]] Result<MutationReceipt> set_rack_structure(const SetRackStructureRequest& request);
  [[nodiscard]] Result<MutationReceipt> transition_lifecycle(
      const TransitionLifecycleRequest& request);
  [[nodiscard]] Result<MutationReceipt> insert_member(const InsertMemberRequest& request);
  [[nodiscard]] Result<MutationReceipt> remove_member(const RemoveMemberRequest& request);
  [[nodiscard]] Result<MutationReceipt> move_member(const MoveMemberRequest& request);
  [[nodiscard]] Result<MutationReceipt> replace_member(const ReplaceMemberRequest& request);

  // -------------------------------------------------------------------------
  // Queries. These read the in-memory image of the authoritative generation.
  // -------------------------------------------------------------------------

  [[nodiscard]] bool contains_rack(const RackId& rack_id) const;
  [[nodiscard]] std::size_t rack_count() const;
  [[nodiscard]] Result<RackView> rack(const RackId& rack_id) const;
  [[nodiscard]] std::vector<RackView> racks() const;
  [[nodiscard]] Result<std::vector<MemberRecord>> members(const RackId& rack_id,
                                                          MemberOrder order) const;
  [[nodiscard]] Result<std::optional<MemberRecord>> member(const RackId& rack_id,
                                                           const RackMemberId& member_id) const;
  [[nodiscard]] Result<std::vector<OccupancyRecord>> occupancy(const RackId& rack_id) const;
  [[nodiscard]] Result<std::vector<FreeSpan>> free_spans(const RackId& rack_id) const;
  [[nodiscard]] Result<std::vector<SharedSpanAvailability>> shared_availability(
      const RackId& rack_id) const;
  [[nodiscard]] Result<CompatibilityReport> evaluate_compatibility(
      const RackId& rack_id, const MemberRequirements& requirements) const;
  [[nodiscard]] Result<RackDiff> diff_generations(const RackId& rack_id,
                                                  RackGeneration from) const;
  [[nodiscard]] Result<RackDiff> diff_generations(const RackId& rack_id,
                                                  RackGeneration from,
                                                  RackGeneration to) const;
  [[nodiscard]] std::vector<RejectionRecord> rejections() const;
  [[nodiscard]] RegistryStats stats() const;
  [[nodiscard]] RackSnapshot snapshot() const;

  // Compares the currently authoritative generation with the retained previous
  // publication. Available only while the retained file is present and valid.
  [[nodiscard]] Result<SnapshotDiff> diff_with_previous() const;

  // -------------------------------------------------------------------------
  // Store state.
  // -------------------------------------------------------------------------

  [[nodiscard]] const RecoveryReport& recovery() const;
  [[nodiscard]] WriterLockInfo writer_lock() const;
  [[nodiscard]] bool holds_writer_authority() const;
  [[nodiscard]] bool is_read_only() const;
  [[nodiscard]] StoreEpoch epoch() const;
  [[nodiscard]] StoreSequence sequence() const;
  [[nodiscard]] const std::filesystem::path& path() const;

  // Releases writer authority, retires any temporary file this store still
  // owns and returns accounting to its baseline. Idempotent. A store that is
  // destroyed without close() performs the same work.
  [[nodiscard]] Status close();

 private:
  struct Impl;
  explicit RackStore(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace rackregistry
