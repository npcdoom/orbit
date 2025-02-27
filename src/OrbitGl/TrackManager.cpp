// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "TrackManager.h"

#include <GteVector.h>
#include <absl/container/flat_hash_map.h>
#include <absl/flags/declare.h>
#include <absl/flags/flag.h>
#include <absl/strings/ascii.h>
#include <absl/strings/match.h>
#include <absl/strings/str_format.h>
#include <absl/strings/str_split.h>
#include <stdlib.h>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <tuple>
#include <utility>

#include "App.h"
#include "ClientData/CallstackData.h"
#include "ClientData/CaptureData.h"
#include "ClientFlags/ClientFlags.h"
#include "GlCanvas.h"
#include "OrbitBase/Append.h"
#include "OrbitBase/Logging.h"
#include "OrbitBase/ThreadConstants.h"
#include "TimeGraph.h"
#include "TimeGraphLayout.h"
#include "Viewport.h"

using orbit_client_data::CallstackData;
using orbit_client_protos::TimerInfo;
using orbit_gl::CGroupAndProcessMemoryTrack;
using orbit_gl::PageFaultsTrack;
using orbit_gl::SystemMemoryTrack;
using orbit_gl::VariableTrack;

TrackManager::TrackManager(TimeGraph* time_graph, orbit_gl::Viewport* viewport,
                           TimeGraphLayout* layout, OrbitApp* app,
                           orbit_client_data::CaptureData* capture_data)
    : time_graph_(time_graph),
      viewport_(viewport),
      layout_(layout),
      capture_data_{capture_data},
      app_{app} {
  CHECK(capture_data != nullptr);
  for (Track::Type type : Track::kAllTrackTypes) {
    track_type_visibility_[type] = true;
  }
}

std::vector<Track*> TrackManager::GetAllTracks() const {
  std::vector<Track*> tracks;
  for (const auto& track : all_tracks_) {
    tracks.push_back(track.get());
  }

  for (const auto track : GetFrameTracks()) {
    tracks.push_back(track);
  }
  return tracks;
}

std::vector<ThreadTrack*> TrackManager::GetThreadTracks() const {
  std::vector<ThreadTrack*> tracks;
  for (const auto& [unused_key, track] : thread_tracks_) {
    tracks.push_back(track.get());
  }
  return tracks;
}

std::vector<FrameTrack*> TrackManager::GetFrameTracks() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::vector<FrameTrack*> tracks;
  for (const auto& [unused_id, track] : frame_tracks_) {
    tracks.push_back(track.get());
  }
  return tracks;
}

void TrackManager::SortTracks() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  // Gather all tracks regardless of the process in sorted order
  std::vector<Track*> all_processes_sorted_tracks;

  // Frame tracks.
  for (const auto& name_and_track : frame_tracks_) {
    all_processes_sorted_tracks.push_back(name_and_track.second.get());
  }

  // Gpu tracks.
  for (const auto& timeline_and_track : gpu_tracks_) {
    all_processes_sorted_tracks.push_back(timeline_and_track.second.get());
  }

  // Variable tracks.
  for (const auto& variable_track : variable_tracks_) {
    all_processes_sorted_tracks.push_back(variable_track.second.get());
  }

  // Memory tracks.
  if (system_memory_track_ != nullptr && !system_memory_track_->IsEmpty()) {
    all_processes_sorted_tracks.push_back(system_memory_track_.get());
  }
  if (cgroup_and_process_memory_track_ != nullptr && !cgroup_and_process_memory_track_->IsEmpty()) {
    all_processes_sorted_tracks.push_back(cgroup_and_process_memory_track_.get());
  }

  // PageFaults track.
  if (page_faults_track_ != nullptr && !page_faults_track_->IsEmpty()) {
    all_processes_sorted_tracks.push_back(page_faults_track_.get());
  }

  // Async tracks.
  for (const auto& async_track : async_tracks_) {
    all_processes_sorted_tracks.push_back(async_track.second.get());
  }

  // Tracepoint tracks.
  if (absl::GetFlag(FLAGS_enable_tracepoint_feature)) {
    if (app_ != nullptr && app_->HasCaptureData()) {
      ThreadTrack* tracepoints_track =
          GetOrCreateThreadTrack(orbit_base::kAllThreadsOfAllProcessesTid);
      if (!tracepoints_track->IsEmpty()) {
        all_processes_sorted_tracks.push_back(tracepoints_track);
      }
    }
  }

  // Process track.
  if (app_ != nullptr && app_->HasCaptureData()) {
    ThreadTrack* process_track = GetOrCreateThreadTrack(orbit_base::kAllProcessThreadsTid);
    if (!process_track->IsEmpty()) {
      all_processes_sorted_tracks.push_back(process_track);
    }
  }

  // Thread tracks.
  std::vector<ThreadTrack*> sorted_thread_tracks = GetSortedThreadTracks();
  for (ThreadTrack* thread_track : sorted_thread_tracks) {
    if (!thread_track->IsEmpty()) {
      all_processes_sorted_tracks.push_back(thread_track);
    }
  }

  // Separate "capture_pid" tracks from tracks that originate from other processes.
  uint32_t capture_pid = capture_data_->process_id();
  std::vector<Track*> capture_pid_tracks;
  std::vector<Track*> external_pid_tracks;
  for (auto& track : all_processes_sorted_tracks) {
    uint32_t pid = track->GetProcessId();
    if (pid != orbit_base::kInvalidProcessId && pid != capture_pid) {
      external_pid_tracks.push_back(track);
    } else {
      capture_pid_tracks.push_back(track);
    }
  }

  // Clear before repopulating.
  sorted_tracks_.clear();

  // Scheduler track.
  if (scheduler_track_ != nullptr && !scheduler_track_->IsEmpty()) {
    sorted_tracks_.push_back(scheduler_track_.get());
  }

  // For now, "external_pid_tracks" should only contain introspection tracks. Display them on top.
  orbit_base::Append(sorted_tracks_, external_pid_tracks);
  orbit_base::Append(sorted_tracks_, capture_pid_tracks);

  last_thread_reorder_.Restart();

  sorting_invalidated_ = false;
  visible_track_list_needs_update_ = true;
}

void TrackManager::SetFilter(const std::string& filter) {
  filter_ = absl::AsciiStrToLower(filter);
  visible_track_list_needs_update_ = true;
}

void TrackManager::UpdateVisibleTrackList() {
  CHECK(visible_track_list_needs_update_);

  visible_track_list_needs_update_ = false;
  visible_tracks_.clear();

  auto track_should_be_shown = [this](const Track* track) {
    return track->ShouldBeRendered() && track_type_visibility_[track->GetType()];
  };

  if (filter_.empty()) {
    std::copy_if(sorted_tracks_.begin(), sorted_tracks_.end(), std::back_inserter(visible_tracks_),
                 track_should_be_shown);
  } else {
    std::vector<std::string> filters = absl::StrSplit(filter_, ' ', absl::SkipWhitespace());
    for (const auto& track : sorted_tracks_) {
      if (!track_should_be_shown(track)) {
        continue;
      }

      std::string lower_case_label = absl::AsciiStrToLower(track->GetLabel());
      for (auto& filter : filters) {
        if (absl::StrContains(lower_case_label, filter)) {
          visible_tracks_.push_back(track);
          break;
        }
      }
    }
  }
  if (time_graph_ != nullptr) {
    time_graph_->RequestUpdate();
  }
}

std::vector<ThreadTrack*> TrackManager::GetSortedThreadTracks() {
  std::vector<ThreadTrack*> sorted_tracks;
  absl::flat_hash_map<ThreadTrack*, uint32_t> num_events_by_track;
  const CallstackData& callstack_data = capture_data_->GetCallstackData();

  for (auto& [tid, track] : thread_tracks_) {
    if (tid == orbit_base::kAllProcessThreadsTid) {
      continue;  // "kAllProcessThreadsTid" is handled separately.
    }
    sorted_tracks.push_back(track.get());
    num_events_by_track[track.get()] = callstack_data.GetCallstackEventsOfTidCount(tid);
  }

  // Tracks with instrumented timers appear first, ordered by descending order of timers.
  // The remaining tracks appear after, ordered by descending order of callstack events.
  std::sort(sorted_tracks.begin(), sorted_tracks.end(),
            [&num_events_by_track](ThreadTrack* a, ThreadTrack* b) {
              return std::make_tuple(a->GetNumberOfTimers(), num_events_by_track[a]) >
                     std::make_tuple(b->GetNumberOfTimers(), num_events_by_track[b]);
            });

  return sorted_tracks;
}

void TrackManager::UpdateMovingTrackPositionInVisibleTracks() {
  // This updates the position of the currently moving track in both the sorted_tracks_
  // and the visible_tracks_ array. The moving track is inserted after the first track
  // with a value of top + height smaller than the current mouse position.
  // Only drawn (i.e. not filtered out) tracks are taken into account to determine the
  // insertion position, but both arrays are updated accordingly.
  //
  // Note: We do an O(n) search for the correct position in the sorted_tracks_ array which
  // could be optimized, but this is not worth the effort for the limited number of tracks.

  int moving_track_previous_position = FindMovingTrackIndex();

  if (moving_track_previous_position != -1) {
    Track* moving_track = visible_tracks_[moving_track_previous_position];
    time_graph_->VerticallyMoveIntoView(*moving_track);

    visible_tracks_.erase(visible_tracks_.begin() + moving_track_previous_position);

    int moving_track_current_position = -1;
    for (auto track_it = visible_tracks_.begin(); track_it != visible_tracks_.end(); ++track_it) {
      if (moving_track->GetPos()[1] <= (*track_it)->GetPos()[1]) {
        auto inserted_it = visible_tracks_.insert(track_it, moving_track);
        moving_track_current_position = inserted_it - visible_tracks_.begin();
        break;
      }
    }

    if (moving_track_current_position == -1) {
      visible_tracks_.push_back(moving_track);
      moving_track_current_position = static_cast<int>(visible_tracks_.size()) - 1;
    }

    // Now we have to change the position of the moving_track in the non-filtered array
    if (moving_track_current_position == moving_track_previous_position) {
      return;
    }
    sorted_tracks_.erase(std::find(sorted_tracks_.begin(), sorted_tracks_.end(), moving_track));
    if (moving_track_current_position > moving_track_previous_position) {
      // In this case we will insert the moving_track right after the one who is before in the
      // filtered array
      Track* previous_filtered_track = visible_tracks_[moving_track_current_position - 1];
      sorted_tracks_.insert(
          ++std::find(sorted_tracks_.begin(), sorted_tracks_.end(), previous_filtered_track),
          moving_track);
    } else {
      // In this case we will insert the moving_track right before the one who is after in the
      // filtered array
      Track* next_filtered_track = visible_tracks_[moving_track_current_position + 1];
      sorted_tracks_.insert(
          std::find(sorted_tracks_.begin(), sorted_tracks_.end(), next_filtered_track),
          moving_track);
    }
  }
}

int TrackManager::FindMovingTrackIndex() {
  // Returns the position of the moving track, or -1 if there is none.
  for (auto track_it = visible_tracks_.begin(); track_it != visible_tracks_.end(); ++track_it) {
    if ((*track_it)->IsMoving()) {
      return track_it - visible_tracks_.begin();
    }
  }
  return -1;
}

void TrackManager::UpdateTrackListForRendering() {
  // Reorder threads if sorting isn't valid or once per second when capturing.
  if (sorting_invalidated_ ||
      (app_ != nullptr && app_->IsCapturing() && last_thread_reorder_.ElapsedMillis() > 1000.0)) {
    SortTracks();
  }

  // Update visible track list from the sorted one based on the filter.
  if (visible_track_list_needs_update_) {
    UpdateVisibleTrackList();
  }

  UpdateMovingTrackPositionInVisibleTracks();
}

void TrackManager::AddTrack(const std::shared_ptr<Track>& track) {
  all_tracks_.push_back(track);
  sorting_invalidated_ = true;
  visible_track_list_needs_update_ = true;
}

void TrackManager::AddFrameTrack(const std::shared_ptr<FrameTrack>& frame_track) {
  // We are inserting the new frame track just after the last Frame Track with lower function_id
  // (and also after the Scheduler one).
  auto last_frame_or_scheduler_track_pos =
      find_if(sorted_tracks_.rbegin(), sorted_tracks_.rend(), [frame_track](Track* track) {
        return (track->GetType() == Track::Type::kFrameTrack &&
                frame_track->GetFunctionId() > static_cast<FrameTrack*>(track)->GetFunctionId()) ||
               track->GetType() == Track::Type::kSchedulerTrack;
      });
  if (last_frame_or_scheduler_track_pos != sorted_tracks_.rend()) {
    sorted_tracks_.insert(last_frame_or_scheduler_track_pos.base(), frame_track.get());
  } else {
    sorted_tracks_.insert(sorted_tracks_.begin(), frame_track.get());
  }
  visible_track_list_needs_update_ = true;
}

void TrackManager::RemoveFrameTrack(uint64_t function_id) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  sorted_tracks_.erase(
      std::remove(sorted_tracks_.begin(), sorted_tracks_.end(), frame_tracks_[function_id].get()),
      sorted_tracks_.end());
  frame_tracks_.erase(function_id);

  visible_track_list_needs_update_ = true;
}

void TrackManager::SetTrackTypeVisibility(Track::Type type, bool value) {
  track_type_visibility_[type] = value;
  if (time_graph_ != nullptr) {
    time_graph_->RequestUpdate();
  }
  visible_track_list_needs_update_ = true;
}

bool TrackManager::GetTrackTypeVisibility(Track::Type type) const {
  return track_type_visibility_.at(type);
}

const absl::flat_hash_map<Track::Type, bool> TrackManager::GetAllTrackTypesVisibility() const {
  return track_type_visibility_;
}

void TrackManager::RestoreAllTrackTypesVisibility(
    const absl::flat_hash_map<Track::Type, bool>& values) {
  track_type_visibility_ = values;
  if (time_graph_ != nullptr) {
    time_graph_->RequestUpdate();
  }
  visible_track_list_needs_update_ = true;
}

bool TrackManager::IteratableType(orbit_client_protos::TimerInfo_Type type) {
  switch (type) {
    case TimerInfo::kNone:
    case TimerInfo::kApiScope:
    case TimerInfo::kGpuActivity:
    case TimerInfo::kGpuCommandBuffer:
    case TimerInfo::kGpuDebugMarker:
      return true;
    default:
      return false;
  }
}

bool TrackManager::FunctionIteratableType(orbit_client_protos::TimerInfo_Type type) {
  switch (type) {
    case TimerInfo::kNone:
    case TimerInfo::kApiScope:
      return true;
    default:
      return false;
  }
}

Track* TrackManager::GetOrCreateTrackFromTimerInfo(const TimerInfo& timer_info) {
  switch (timer_info.type()) {
    case TimerInfo::kNone:
    case TimerInfo::kApiScope:
    case TimerInfo::kApiEvent:
      return GetOrCreateThreadTrack(timer_info.thread_id());
    case TimerInfo::kCoreActivity:
      return GetOrCreateSchedulerTrack();
    case TimerInfo::kFrame:
      return GetOrCreateFrameTrack(
          capture_data_->instrumented_functions().at(timer_info.function_id()));
    case TimerInfo::kGpuActivity:
    case TimerInfo::kGpuCommandBuffer:
    case TimerInfo::kGpuDebugMarker:
      return GetOrCreateGpuTrack(timer_info.timeline_hash());
    case TimerInfo::kApiScopeAsync:
      return GetOrCreateAsyncTrack(timer_info.api_scope_name());
    case TimerInfo::kSystemMemoryUsage:
      return GetSystemMemoryTrack();
    case TimerInfo::kCGroupAndProcessMemoryUsage:
      return GetCGroupAndProcessMemoryTrack();
    case TimerInfo::kPageFaults:
      return GetPageFaultsTrack();
    case orbit_client_protos::TimerInfo_Type_TimerInfo_Type_INT_MIN_SENTINEL_DO_NOT_USE_:
    case orbit_client_protos::TimerInfo_Type_TimerInfo_Type_INT_MAX_SENTINEL_DO_NOT_USE_:
      UNREACHABLE();
  }
  return nullptr;
}

SchedulerTrack* TrackManager::GetOrCreateSchedulerTrack() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (scheduler_track_ == nullptr) {
    auto [unused, timer_data] = capture_data_->CreateTimerData();
    scheduler_track_ = std::make_shared<SchedulerTrack>(time_graph_, time_graph_, viewport_,
                                                        layout_, app_, capture_data_, timer_data);
    AddTrack(scheduler_track_);
  }
  return scheduler_track_.get();
}

ThreadTrack* TrackManager::GetOrCreateThreadTrack(uint32_t tid) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::shared_ptr<ThreadTrack> track = thread_tracks_[tid];
  if (track == nullptr) {
    auto thread_track_data_provider = capture_data_->GetThreadTrackDataProvider();
    thread_track_data_provider->CreateScopeTreeTimerData(tid);
    track = std::make_shared<ThreadTrack>(time_graph_, time_graph_, viewport_, layout_, tid, app_,
                                          capture_data_, thread_track_data_provider);
    thread_tracks_[tid] = track;
    AddTrack(track);
  }
  return track.get();
}

GpuTrack* TrackManager::GetOrCreateGpuTrack(uint64_t timeline_hash) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::string timeline =
      app_->GetStringManager()->Get(timeline_hash).value_or(std::to_string(timeline_hash));
  std::shared_ptr<GpuTrack> track = gpu_tracks_[timeline];
  if (track == nullptr) {
    auto [unused1, submission_timer_data] = capture_data_->CreateTimerData();
    auto [unused2, marker_timer_data] = capture_data_->CreateTimerData();
    track =
        std::make_shared<GpuTrack>(time_graph_, time_graph_, viewport_, layout_, timeline_hash,
                                   app_, capture_data_, submission_timer_data, marker_timer_data);
    gpu_tracks_[timeline] = track;
    AddTrack(track);
  }
  return track.get();
}

VariableTrack* TrackManager::GetOrCreateVariableTrack(const std::string& name) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::shared_ptr<VariableTrack> track = variable_tracks_[name];
  if (track == nullptr) {
    track = std::make_shared<VariableTrack>(time_graph_, time_graph_, viewport_, layout_, name,
                                            capture_data_);
    variable_tracks_[name] = track;
    AddTrack(track);
  }
  return track.get();
}

AsyncTrack* TrackManager::GetOrCreateAsyncTrack(const std::string& name) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::shared_ptr<AsyncTrack> track = async_tracks_[name];
  if (track == nullptr) {
    auto [unused, timer_data] = capture_data_->CreateTimerData();
    track = std::make_shared<AsyncTrack>(time_graph_, time_graph_, viewport_, layout_, name, app_,
                                         capture_data_, timer_data);
    async_tracks_[name] = track;
    AddTrack(track);
  }

  return track.get();
}

FrameTrack* TrackManager::GetOrCreateFrameTrack(
    const orbit_grpc_protos::InstrumentedFunction& function) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto track_it = frame_tracks_.find(function.function_id());
  if (track_it != frame_tracks_.end()) {
    return track_it->second.get();
  }

  auto [unused, timer_data] = capture_data_->CreateTimerData();
  auto track = std::make_shared<FrameTrack>(time_graph_, time_graph_, viewport_, layout_, function,
                                            app_, capture_data_, timer_data);

  // Normally we would call AddTrack(track) here, but frame tracks are removable by users
  // and therefore cannot be simply thrown into the flat vector of tracks. Also, we don't want to
  // trigger a sorting in all the tracks.
  frame_tracks_[function.function_id()] = track;
  AddFrameTrack(track);

  return track.get();
}

SystemMemoryTrack* TrackManager::CreateAndGetSystemMemoryTrack() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (system_memory_track_ == nullptr) {
    system_memory_track_ = std::make_shared<SystemMemoryTrack>(time_graph_, time_graph_, viewport_,
                                                               layout_, capture_data_);
    AddTrack(system_memory_track_);
  }

  return GetSystemMemoryTrack();
}

CGroupAndProcessMemoryTrack* TrackManager::CreateAndGetCGroupAndProcessMemoryTrack(
    const std::string& cgroup_name) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (cgroup_and_process_memory_track_ == nullptr) {
    cgroup_and_process_memory_track_ = std::make_shared<CGroupAndProcessMemoryTrack>(
        time_graph_, time_graph_, viewport_, layout_, cgroup_name, capture_data_);
    AddTrack(cgroup_and_process_memory_track_);
  }

  return GetCGroupAndProcessMemoryTrack();
}

PageFaultsTrack* TrackManager::CreateAndGetPageFaultsTrack(const std::string& cgroup_name,
                                                           uint64_t memory_sampling_period_ms) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (page_faults_track_ == nullptr) {
    page_faults_track_ =
        std::make_shared<PageFaultsTrack>(time_graph_, time_graph_, viewport_, layout_, cgroup_name,
                                          memory_sampling_period_ms, capture_data_);
    AddTrack(page_faults_track_);
  }
  return GetPageFaultsTrack();
}

std::pair<uint64_t, uint64_t> TrackManager::GetTracksMinMaxTimestamps() const {
  uint64_t min_time = std::numeric_limits<uint64_t>::max();
  uint64_t max_time = std::numeric_limits<uint64_t>::min();

  std::lock_guard<std::recursive_mutex> lock(mutex_);
  for (auto& track : GetAllTracks()) {
    if (!track->IsEmpty()) {
      min_time = std::min(min_time, track->GetMinTime());
      max_time = std::max(max_time, track->GetMaxTime());
    }
  }
  return std::make_pair(min_time, max_time);
}