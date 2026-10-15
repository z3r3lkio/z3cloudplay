#include "app/persistence_controller.h"

#include <utility>

namespace z3cloudplay::app {
namespace {

constexpr std::size_t kEventsPerTick = 16;
constexpr std::size_t kShutdownEvents = 64;

bool PreferencesLoadFailed(PreferencesLoadStatus status) {
  return status == PreferencesLoadStatus::kDefaultCorrupt ||
         status == PreferencesLoadStatus::kDefaultReadError;
}

PreferencesLoadResult PreferenceResult(const PersistenceEvent& event) {
  PreferencesLoadResult result;
  if (event.preferences) result.preferences = *event.preferences;
  result.status = event.preferences_status;
  return result;
}

}  // namespace

PersistenceController::PersistenceController(homebrew::Simulation& model,
                                             PersistenceService& service)
    : model_(model), service_(service) {
  BeginPreferencesLoad();
}

void PersistenceController::BeginPreferencesLoad() {
  preferences_load_token_ = model_.BeginPreferencesLoad();
  preferences_load_request_ = service_.LoadPreferences();
  preferences_load_failed_ = false;
  if (preferences_load_request_) return;

  PreferencesLoadResult failure;
  failure.status = PreferencesLoadStatus::kDefaultReadError;
  failure.diagnostic = "Preferences are busy and could not be loaded.";
  model_.ApplyPreferences(preferences_load_token_, failure);
  preferences_load_failed_ = true;
}

void PersistenceController::BeginOrganizationLoad() {
  if (!account_id_) return;
  organization_load_token_ = model_.BeginOrganizationLoad(account_id_);
  organization_load_request_ =
      service_.LoadOrganization(account_id_, account_generation_);
  organization_load_failed_ = false;
  if (organization_load_request_) return;

  model_.FailOrganizationLoad(account_id_, organization_load_token_,
                              StorageError::kReadFailed);
  organization_load_failed_ = true;
}

void PersistenceController::Activate(std::uint64_t account) {
  if (!account || shutting_down_ || clear_request_) return;
  if (account == account_id_) return;

  if (account_id_) {
    ObserveAndSave();
    service_.InvalidateOrganization(++account_generation_);
    model_.OrganizationCleared(account_id_);
  } else {
    ++account_generation_;
  }
  account_id_ = account;
  organization_load_request_ = 0;
  organization_load_failed_ = false;
  submitted_organization_revision_ = 0;
  BeginOrganizationLoad();
}

void PersistenceController::Reset() {
  if (shutting_down_) return;
  ObserveAndSave();
  service_.InvalidateOrganization(++account_generation_);
  if (account_id_) model_.OrganizationCleared(account_id_);
  account_id_ = 0;
  organization_load_request_ = 0;
  organization_load_failed_ = false;
  submitted_organization_revision_ = 0;
  clear_request_ = 0;
  clear_result_.reset();
}

void PersistenceController::Tick() {
  if (shutting_down_) return;
  Pump(kEventsPerTick);
  if (model_.TakePersistenceRetryRequest()) Retry();
  ObserveAndSave();
}

void PersistenceController::Retry() {
  if (shutting_down_ || clear_request_) return;
  if (preferences_load_failed_) BeginPreferencesLoad();
  else submitted_preferences_revision_ = 0;

  if (!account_id_) return;
  if (organization_load_failed_) BeginOrganizationLoad();
  else submitted_organization_revision_ = 0;
}

void PersistenceController::Clear() {
  if (shutting_down_ || clear_request_ || clear_result_) return;
  if (!account_id_) {
    clear_result_ = StorageError::kNone;
    return;
  }

  ObserveAndSave();
  const auto generation = account_generation_ + 1;
  const auto request = service_.ClearOrganization(account_id_, generation);
  if (!request) {
    clear_result_ = StorageError::kWriteFailed;
    return;
  }
  account_generation_ = generation;
  clear_request_ = request;
  clear_had_pending_load_ = organization_load_request_ != 0;
  organization_load_request_ = 0;
}

std::optional<StorageError> PersistenceController::TakeClearResult() {
  auto result = clear_result_;
  clear_result_.reset();
  return result;
}

void PersistenceController::Pump(std::size_t maximum) {
  for (auto& event : service_.Pump(maximum)) Apply(std::move(event));
}

void PersistenceController::Apply(PersistenceEvent event) {
  if (event.kind == PersistenceEventKind::kPreferencesLoaded) {
    if (event.request_id != preferences_load_request_) return;
    preferences_load_request_ = 0;
    const auto loaded = PreferenceResult(event);
    preferences_load_failed_ = PreferencesLoadFailed(loaded.status);
    model_.ApplyPreferences(preferences_load_token_, loaded);
    return;
  }
  if (event.kind == PersistenceEventKind::kPreferencesSaved) {
    model_.PreferencesSaveResult(event.revision, event.error);
    return;
  }
  if (event.kind == PersistenceEventKind::kOrganizationCleared) {
    if (event.request_id != clear_request_ || event.account_id != account_id_)
      return;
    clear_request_ = 0;
    clear_result_ = event.error;
    if (event.error == StorageError::kNone) {
      organization_load_failed_ = false;
      submitted_organization_revision_ = model_.OrganizationRevision();
      model_.OrganizationCleared(account_id_);
    } else if (clear_had_pending_load_) {
      organization_load_failed_ = true;
      model_.FailOrganizationLoad(account_id_, organization_load_token_,
                                  event.error);
    } else {
      organization_load_failed_ = false;
      // Root presents the destructive-operation error. The already loaded
      // model remains authoritative; do not mislabel deletion as a save
      // failure or leave a clean model with an unresolvable save notice.
      submitted_organization_revision_ = 0;
    }
    clear_had_pending_load_ = false;
    return;
  }
  if (event.account_id != account_id_ ||
      event.account_generation != account_generation_) return;
  if (event.kind == PersistenceEventKind::kOrganizationLoaded) {
    if (event.request_id != organization_load_request_) return;
    organization_load_request_ = 0;
    if (event.error == StorageError::kNone) {
      organization_load_failed_ = !model_.ApplyOrganization(
          account_id_, organization_load_token_, std::move(event.organization));
    } else {
      organization_load_failed_ = true;
      model_.FailOrganizationLoad(account_id_, organization_load_token_,
                                  event.error);
    }
    return;
  }
  if (event.kind == PersistenceEventKind::kOrganizationSaved)
    model_.OrganizationSaveResult(event.account_id, event.revision, event.error);
}

void PersistenceController::ObserveAndSave() {
  if (!preferences_load_request_ && !preferences_load_failed_ &&
      model_.PreferencesDirty()) {
    const auto revision = model_.PreferencesRevision();
    if (revision != submitted_preferences_revision_) {
      const auto request =
          service_.SavePreferences(model_.ExportPreferences(), revision);
      submitted_preferences_revision_ = revision;
      if (!request)
        model_.PreferencesSaveResult(revision, StorageError::kWriteFailed);
    }
  }

  if (!account_id_ || clear_request_ || organization_load_request_ ||
      organization_load_failed_ || !model_.OrganizationReadyForSave() ||
      !model_.OrganizationDirty()) return;
  const auto revision = model_.OrganizationRevision();
  if (revision == submitted_organization_revision_) return;
  const auto request = service_.SaveOrganization(
      account_id_, account_generation_, revision, model_.ExportOrganization());
  submitted_organization_revision_ = revision;
  if (!request)
    model_.OrganizationSaveResult(account_id_, revision,
                                  StorageError::kWriteFailed);
}

void PersistenceController::FlushAndShutdown() {
  if (shutting_down_) return;

  // Complete required loads before stopping: their results supply untouched
  // stored fields which the model merges with edits made while IO was pending.
  ObserveAndSave();
  service_.WaitIdle();
  Pump(kShutdownEvents);
  ObserveAndSave();
  service_.WaitIdle();
  Pump(kShutdownEvents);
  service_.Shutdown();
  Pump(kShutdownEvents);
  shutting_down_ = true;
}

}  // namespace z3cloudplay::app
