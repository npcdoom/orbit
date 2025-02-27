/*
 * Copyright (C) 2018 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <pthread.h>
#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include <unwindstack/LocalUnwinder.h>
#include <unwindstack/MapInfo.h>
#include <unwindstack/Maps.h>
#include <unwindstack/Memory.h>
#include <unwindstack/Object.h>
#include <unwindstack/Regs.h>
#include <unwindstack/RegsGetLocal.h>

namespace unwindstack {

bool LocalUnwinder::Init() {
  pthread_rwlock_init(&maps_rwlock_, nullptr);

  // Create the maps.
  maps_.reset(new unwindstack::LocalUpdatableMaps());
  if (!maps_->Parse()) {
    maps_.reset();
    return false;
  }

  process_memory_ = unwindstack::Memory::CreateProcessMemoryThreadCached(getpid());

  return true;
}

bool LocalUnwinder::ShouldSkipLibrary(const std::string& map_name) {
  for (const std::string& skip_library : skip_libraries_) {
    if (skip_library == map_name) {
      return true;
    }
  }
  return false;
}

bool LocalUnwinder::Unwind(std::vector<LocalFrameData>* frame_info, size_t max_frames) {
  std::unique_ptr<unwindstack::Regs> regs(unwindstack::Regs::CreateFromLocal());
  unwindstack::RegsGetLocal(regs.get());
  ArchEnum arch = regs->Arch();

  // Clear any cached data from previous unwinds.
  process_memory_->Clear();

  size_t num_frames = 0;
  bool adjust_pc = false;
  while (true) {
    uint64_t cur_pc = regs->pc();
    uint64_t cur_sp = regs->sp();

    MapInfo* map_info = maps_->Find(cur_pc);
    if (map_info == nullptr) {
      break;
    }

    Object* object = map_info->GetObject(process_memory_, arch);
    uint64_t rel_pc = object->GetRelPc(cur_pc, map_info);
    uint64_t step_pc = rel_pc;
    uint64_t pc_adjustment;
    if (adjust_pc) {
      pc_adjustment = GetPcAdjustment(rel_pc, object, arch);
    } else {
      pc_adjustment = 0;
    }
    step_pc -= pc_adjustment;

    bool finished = false;
    bool is_signal_frame = false;
    if (object->StepIfSignalHandler(rel_pc, regs.get(), process_memory_.get())) {
      step_pc = rel_pc;
    } else if (!object->Step(step_pc, regs.get(), process_memory_.get(), &finished,
                             &is_signal_frame)) {
      finished = true;
    }

    // Skip any locations that are within this library.
    if (num_frames != 0 || !ShouldSkipLibrary(map_info->name())) {
      // Add frame information.
      SharedString func_name;
      uint64_t func_offset;
      if (object->GetFunctionName(rel_pc, &func_name, &func_offset)) {
        frame_info->emplace_back(map_info, cur_pc - pc_adjustment, rel_pc - pc_adjustment,
                                 func_name, func_offset);
      } else {
        frame_info->emplace_back(map_info, cur_pc - pc_adjustment, rel_pc - pc_adjustment, "", 0);
      }
      num_frames++;
    }

    if (finished || frame_info->size() == max_frames ||
        (cur_pc == regs->pc() && cur_sp == regs->sp())) {
      break;
    }
    adjust_pc = true;
  }
  return num_frames != 0;
}

}  // namespace unwindstack
