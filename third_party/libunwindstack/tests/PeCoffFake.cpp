/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "PeCoffFake.h"

#include <memory>
#include <vector>

#include <unwindstack/PeCoffInterface.h>

namespace unwindstack {

// Needed for static_asserts that depend on the template parameter. Just a
// plain `false` in the static_assert will cause compilation to fail.
template <class>
inline constexpr bool kDependentFalse = false;

template <typename PeCoffInterfaceType>
void PeCoffFake<PeCoffInterfaceType>::Init() {
  memory_->Clear();
  uint64_t offset = SetDosHeader(0x1000);
  offset = SetNewHeaderAtOffset(offset);
  offset = SetCoffHeaderAtOffset(offset);
  offset = SetOptionalHeaderAtOffset(offset);
  offset = SetSectionHeadersAtOffset(offset);

  SetDebugFrameSectionAtOffset(kDebugFrameSectionFileOffset);
}

template <typename PeCoffInterfaceType>
uint64_t PeCoffFake<PeCoffInterfaceType>::InitNoSectionHeaders() {
  CHECK(memory_);
  memory_->Clear();
  uint64_t offset = SetDosHeader(0x1000);
  offset = SetNewHeaderAtOffset(offset);
  offset = SetCoffHeaderAtOffset(offset);
  offset = SetOptionalHeaderAtOffset(offset);
  return offset;
}

template <typename PeCoffInterfaceType>
uint64_t PeCoffFake<PeCoffInterfaceType>::SetData8(uint64_t offset, uint8_t value) {
  CHECK(memory_);
  memory_->SetData8(offset, value);
  return offset + sizeof(uint8_t);
}

template <typename PeCoffInterfaceType>
uint64_t PeCoffFake<PeCoffInterfaceType>::SetData16(uint64_t offset, uint16_t value) {
  CHECK(memory_);
  memory_->SetData16(offset, value);
  return offset + sizeof(uint16_t);
}

template <typename PeCoffInterfaceType>
uint64_t PeCoffFake<PeCoffInterfaceType>::SetData32(uint64_t offset, uint32_t value) {
  CHECK(memory_);
  memory_->SetData32(offset, value);
  return offset + sizeof(uint32_t);
}

template <typename PeCoffInterfaceType>
uint64_t PeCoffFake<PeCoffInterfaceType>::SetData64(uint64_t offset, uint64_t value) {
  CHECK(memory_);
  memory_->SetData64(offset, value);
  return offset + sizeof(uint64_t);
}

template <typename PeCoffInterfaceType>
uint64_t PeCoffFake<PeCoffInterfaceType>::SetMax64(uint64_t offset, uint64_t value, uint64_t size) {
  switch (size) {
    case 1:
      return SetData8(offset, static_cast<uint8_t>(value));
    case 2:
      return SetData16(offset, static_cast<uint16_t>(value));
    case 4:
      return SetData32(offset, static_cast<uint32_t>(value));
    case 8:
      return SetData64(offset, value);
    default:
      return offset;
  }
  return offset;
}

template <typename PeCoffInterfaceType>
void PeCoffFake<PeCoffInterfaceType>::SetDosHeaderMagicValue() {
  // This must be at offset 0.
  SetData16(0x0, 0x5a4d);
}

template <typename PeCoffInterfaceType>
void PeCoffFake<PeCoffInterfaceType>::SetDosHeaderOffsetToNewHeader(uint32_t offset_value) {
  // This must be at offset 0x3c.
  CHECK(memory_);
  memory_->SetMemory(0x3c, &offset_value, sizeof(uint32_t));
}

template <typename PeCoffInterfaceType>
uint64_t PeCoffFake<PeCoffInterfaceType>::SetDosHeader(uint32_t new_header_offset_value) {
  CHECK(memory_);
  std::vector<uint8_t> zero_data(kDosHeaderSizeInBytes, 0);
  memory_->SetMemory(0, zero_data);

  SetDosHeaderMagicValue();
  SetDosHeaderOffsetToNewHeader(new_header_offset_value);
  return new_header_offset_value;
}

template <typename PeCoffInterfaceType>
uint64_t PeCoffFake<PeCoffInterfaceType>::SetNewHeaderAtOffset(uint64_t offset) {
  return SetData32(offset, 0x00004550);
}

template <typename PeCoffInterfaceType>
uint64_t PeCoffFake<PeCoffInterfaceType>::SetCoffHeaderAtOffset(uint64_t offset) {
  offset = SetData16(offset, 0);  // machine

  // We remember the location of the number of sections here, so we can set it correctly later
  // when we initialize the sections.
  coff_header_nsects_offset_ = offset;
  offset = SetData16(offset, 0);  // nsects

  offset = SetData32(offset, 0);  // modtime

  coff_header_symoff_offset_ = offset;
  offset = SetData32(offset, 0);  // symoff

  offset = SetData32(offset, 0);  // nsyms

  // We remember the location of the header size (which is actually the size of the optional
  // header) here so that we can set it correctly later when we know the size of the optional
  // header (which depends on target address size and the number of directory entries).
  optional_header_size_offset_ = offset;
  offset = SetData16(offset, 0);  // hdrsize, to be set correctly later

  offset = SetData16(offset, 0);  // flags
  return offset;
}

template <typename PeCoffInterfaceType>
uint64_t PeCoffFake<PeCoffInterfaceType>::SetOptionalHeaderMagicPE32AtOffset(uint64_t offset) {
  constexpr uint16_t kOptionalHeaderMagicPE32 = 0x010b;
  return SetData16(offset, kOptionalHeaderMagicPE32);
}

template <typename PeCoffInterfaceType>
uint64_t PeCoffFake<PeCoffInterfaceType>::SetOptionalHeaderMagicPE32PlusAtOffset(uint64_t offset) {
  constexpr uint16_t kOptionalHeaderMagicPE32Plus = 0x020b;
  return SetData16(offset, kOptionalHeaderMagicPE32Plus);
}

template <typename PeCoffInterfaceType>
uint64_t PeCoffFake<PeCoffInterfaceType>::SetOptionalHeaderAtOffset(uint64_t offset) {
  optional_header_start_offset_ = offset;

  if constexpr (sizeof(typename PeCoffInterfaceType::AddressType) == 4) {
    offset = SetOptionalHeaderMagicPE32AtOffset(offset);
  } else if constexpr (sizeof(typename PeCoffInterfaceType::AddressType) == 8) {
    offset = SetOptionalHeaderMagicPE32PlusAtOffset(offset);
  } else {
    static_assert(kDependentFalse<PeCoffInterfaceType>, "AddressType size must be 4 or 8 bytes");
  }
  offset = SetData8(offset, 0);   // major_linker_version
  offset = SetData8(offset, 0);   // minor_linker_version
  offset = SetData32(offset, 0);  // code_size
  offset = SetData32(offset, 0);  // data_size
  offset = SetData32(offset, 0);  // bss_size
  offset = SetData32(offset, 0);  // entry
  offset = SetData32(offset, 0);  // code_offset

  if constexpr (sizeof(typename PeCoffInterfaceType::AddressType) == 4) {
    // Data offset only exists in 32-bit PE/COFF.
    offset = SetData32(offset, 0);
  }

  // image_base
  offset = SetMax64(offset, kLoadBiasFake, sizeof(typename PeCoffInterfaceType::AddressType));

  offset = SetData32(offset, 0x1000);  // sect_alignment
  offset = SetData32(offset, 0x200);   // file_alignment
  offset = SetData16(offset, 0);       // major_os_system_version
  offset = SetData16(offset, 0);       // minor_os_system_version
  offset = SetData16(offset, 0);       // major_image_version
  offset = SetData16(offset, 0);       // minor_image_version
  offset = SetData16(offset, 0);       // major_subsystem_version
  offset = SetData16(offset, 0);       // minor_subsystem_version
  offset = SetData32(offset, 0);       // reserved1
  offset = SetData32(offset, 0);       // image_size
  offset = SetData32(offset, 0);       // header_size
  offset = SetData32(offset, 0);       // checksum
  offset = SetData16(offset, 0);       // subsystem
  offset = SetData16(offset, 0);       // dll_flags

  // stack_reserve_size
  offset = SetMax64(offset, 0, sizeof(typename PeCoffInterfaceType::AddressType));
  // stack_commit_size
  offset = SetMax64(offset, 0, sizeof(typename PeCoffInterfaceType::AddressType));
  // heap_reserve_size
  offset = SetMax64(offset, 0, sizeof(typename PeCoffInterfaceType::AddressType));
  // heap_commit_size
  offset = SetMax64(offset, 0, sizeof(typename PeCoffInterfaceType::AddressType));

  offset = SetData32(offset, 0);  // loader_flags

  optional_header_num_data_dirs_offset_ = offset;
  constexpr uint32_t kNumDirDataEntries = 10;
  offset = SetData32(offset, kNumDirDataEntries);  // num_dir_data_entries

  for (uint32_t i = 0; i < kNumDirDataEntries; ++i) {
    offset = SetData32(offset, 0);
    offset = SetData32(offset, 0);
  }

  SetData16(optional_header_size_offset_, offset - optional_header_start_offset_);

  return offset;
}

template <typename PeCoffInterfaceType>
uint64_t PeCoffFake<PeCoffInterfaceType>::SetSectionHeaderAtOffset(uint64_t offset,
                                                                   std::string section_name,
                                                                   uint64_t vmsize, uint64_t vmaddr,
                                                                   uint64_t size,
                                                                   uint64_t file_offset) {
  CHECK(memory_);
  std::string name_in_header = section_name;
  if (section_name.size() > kSectionNameInHeaderSize) {
    const uint64_t previous_offset =
        section_names_in_string_table_.empty() ? 0 : section_names_in_string_table_.back().first;
    const uint64_t previous_size = section_names_in_string_table_.empty()
                                       ? 0
                                       // The +1 is for null-termination of the string when written
                                       // to the string table in the fake file.
                                       : (section_names_in_string_table_.back().second.size() + 1);
    const uint64_t current_offset = previous_offset + previous_size;
    name_in_header = std::string("/") + std::to_string(current_offset);
    section_names_in_string_table_.emplace_back(current_offset, section_name);
  }

  std::vector<uint8_t> zeros(kSectionNameInHeaderSize, 0);
  memory_->SetMemory(offset, zeros.data(), zeros.size());
  memory_->SetMemory(offset, name_in_header);
  offset += kSectionNameInHeaderSize;
  offset = SetData32(offset, vmsize);
  offset = SetData32(offset, vmaddr);
  offset = SetData32(offset, size);
  offset = SetData32(offset, file_offset);
  offset = SetData32(offset, 0);  // reloff
  offset = SetData32(offset, 0);  // lineoff
  offset = SetData16(offset, 0);  // nrel
  offset = SetData16(offset, 0);  // nline
  offset = SetData32(offset, 0);  // flagsd
  return offset;
};

template <typename PeCoffInterfaceType>
uint64_t PeCoffFake<PeCoffInterfaceType>::SetSectionStringsAtOffset(uint64_t offset) {
  CHECK(memory_);
  const uint64_t string_table_base_offset = offset;
  for (const auto& [string_table_offset, name] : section_names_in_string_table_) {
    memory_->SetMemory(string_table_base_offset + string_table_offset, name);

    // Strings written to memory are null-terminated, so we need to add "1" to the size.
    offset += name.size() + 1;
  }
  return offset;
}

template <typename PeCoffInterfaceType>
uint64_t PeCoffFake<PeCoffInterfaceType>::SetSectionHeadersAtOffset(uint64_t offset) {
  // Shorter than kSectionNameInHeaderSize (== 8) characters
  offset = SetSectionHeaderAtOffset(offset, ".text", 0, kTextSectionOffsetFake, 0, 0);
  // Longer than kSectionNameInHeaderSize (== 8) characters
  offset = SetSectionHeaderAtOffset(offset, ".debug_frame", kDebugFrameSectionSize,
                                    kDebugFrameSectionFileOffset, kDebugFrameSectionSize,
                                    kDebugFrameSectionFileOffset);
  SetData16(coff_header_nsects_offset_, 2);

  CHECK(offset <= std::numeric_limits<uint32_t>::max());
  const uint32_t actual_symoff = static_cast<uint32_t>(offset);
  SetData32(coff_header_symoff_offset_, actual_symoff);

  offset = SetSectionStringsAtOffset(offset);

  return offset;
}

template <typename PeCoffInterfaceType>
uint64_t PeCoffFake<PeCoffInterfaceType>::SetDebugFrameSectionAtOffset(uint64_t offset) {
  CHECK(memory_);
  uint64_t initial_offset = offset;
  // CIE 32
  offset = SetData32(offset, 0xfc);        // length
  offset = SetData32(offset, 0xffffffff);  // CIE_id
  memory_->SetMemory(offset, std::vector<uint8_t>{1, '\0', 0, 0, 1});

  // FDE 32
  offset = initial_offset + 0x100;
  offset = SetData32(offset, 0xfc);
  offset = SetData32(offset, 0);
  offset = SetData32(offset, 0x2100);
  offset = SetData32(offset, 0x400);
  return offset;
}

// Instantiate all of the needed template functions.
template class PeCoffFake<PeCoffInterface32>;
template class PeCoffFake<PeCoffInterface64>;

}  // namespace unwindstack