// Copyright 2011 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/codegen/safepoint-table.h"

#include <iomanip>

#include "src/codegen/assembler-inl.h"
#include "src/codegen/macro-assembler.h"
#include "src/deoptimizer/deoptimizer.h"
#include "src/diagnostics/disasm.h"
#include "src/execution/frames-inl.h"
#include "src/utils/ostreams.h"

#if V8_ENABLE_WEBASSEMBLY
#include "src/wasm/wasm-code-manager.h"
#endif  // V8_ENABLE_WEBASSEMBLY

namespace v8 {
namespace internal {

SafepointTable::SafepointTable(Isolate* isolate, Address pc, Code code)
    : SafepointTable(code.InstructionStart(isolate, pc),
                     code.SafepointTableAddress()) {}

#if V8_ENABLE_WEBASSEMBLY
SafepointTable::SafepointTable(const wasm::WasmCode* code)
    : SafepointTable(
          code->instruction_start(),
          code->instruction_start() + code->safepoint_table_offset()) {}
#endif  // V8_ENABLE_WEBASSEMBLY

SafepointTable::SafepointTable(Address instruction_start,
                               Address safepoint_table_address)
    : instruction_start_(instruction_start),
      safepoint_table_address_(safepoint_table_address),
      length_(base::Memory<int>(safepoint_table_address + kLengthOffset)),
      entry_configuration_(base::Memory<uint32_t>(safepoint_table_address +
                                                  kEntryConfigurationOffset)) {}

int SafepointTable::find_return_pc(int pc_offset) {
  for (int i = 0; i < length(); i++) {
    SafepointEntry entry = GetEntry(i);
    if (entry.trampoline_pc() == pc_offset || entry.pc() == pc_offset) {
      return entry.pc();
    }
  }
  UNREACHABLE();
}

SafepointEntry SafepointTable::FindEntry(Address pc) const {
  int pc_offset = static_cast<int>(pc - instruction_start_);

  // Check if the PC is pointing at a trampoline.
  if (has_deopt_data()) {
    int candidate = -1;
    for (int i = 0; i < length_; ++i) {
      int trampoline_pc = GetEntry(i).trampoline_pc();
      if (trampoline_pc != -1 && trampoline_pc <= pc_offset) candidate = i;
      if (trampoline_pc > pc_offset) break;
    }
    if (candidate != -1) return GetEntry(candidate);
  }

  for (int i = 0; i < length_; ++i) {
    SafepointEntry entry = GetEntry(i);
    if (i == length_ - 1 || GetEntry(i + 1).pc() > pc_offset) {
      DCHECK_LE(entry.pc(), pc_offset);
      return entry;
    }
  }
  UNREACHABLE();
}

void SafepointTable::Print(std::ostream& os) const {
  os << "Safepoints (entries = " << length_ << ", byte size = " << byte_size()
     << ")\n";

  for (int index = 0; index < length_; index++) {
    SafepointEntry entry = GetEntry(index);
    os << reinterpret_cast<const void*>(instruction_start_ + entry.pc()) << " "
       << std::setw(6) << std::hex << entry.pc() << std::dec;

    if (!entry.tagged_slots().empty()) {
      os << "  slots (sp->fp): ";
      for (uint8_t bits : entry.tagged_slots()) {
        for (int bit = 0; bit < kBitsPerByte; ++bit) {
          os << ((bits >> bit) & 1);
        }
      }
    }

    if (entry.tagged_register_indexes() != 0) {
      os << "  registers: ";
      uint32_t register_bits = entry.tagged_register_indexes();
      int bits = 32 - base::bits::CountLeadingZeros32(register_bits);
      for (int j = bits - 1; j >= 0; --j) {
        os << ((register_bits >> j) & 1);
      }
    }

    if (entry.has_deoptimization_index()) {
      os << "  deopt " << std::setw(6) << entry.deoptimization_index()
         << " trampoline: " << std::setw(6) << std::hex
         << entry.trampoline_pc();
    }
    os << "\n";
  }
}

Safepoint SafepointTableBuilder::DefineSafepoint(Assembler* assembler) {
  entries_.push_back(EntryBuilder(zone_, assembler->pc_offset_for_safepoint()));
  EntryBuilder& new_entry = entries_.back();
  return Safepoint(new_entry.stack_indexes, &new_entry.register_indexes);
}

int SafepointTableBuilder::UpdateDeoptimizationInfo(int pc, int trampoline,
                                                    int start,
                                                    int deopt_index) {
  DCHECK_NE(SafepointEntry::kNoTrampolinePC, trampoline);
  DCHECK_NE(SafepointEntry::kNoDeoptIndex, deopt_index);
  auto it = entries_.Find(start);
  DCHECK(std::any_of(it, entries_.end(),
                     [pc](auto& entry) { return entry.pc == pc; }));
  int index = start;
  while (it->pc != pc) ++it, ++index;
  it->trampoline = trampoline;
  it->deopt_index = deopt_index;
  return index;
}

void SafepointTableBuilder::Emit(Assembler* assembler, int tagged_slots_size) {
#ifdef DEBUG
  int last_pc = -1;
  int last_trampoline = -1;
  for (const EntryBuilder& entry : entries_) {
    // Entries are ordered by PC.
    DCHECK_LT(last_pc, entry.pc);
    last_pc = entry.pc;
    // Trampoline PCs are increasing, and larger than regular PCs.
    if (entry.trampoline != SafepointEntry::kNoTrampolinePC) {
      DCHECK_LT(last_trampoline, entry.trampoline);
      DCHECK_LT(entries_.back().pc, entry.trampoline);
      last_trampoline = entry.trampoline;
    }
    // An entry either has trampoline and deopt index, or none of the two.
    DCHECK_EQ(entry.trampoline == SafepointEntry::kNoTrampolinePC,
              entry.deopt_index == SafepointEntry::kNoDeoptIndex);
  }
#endif  // DEBUG

  RemoveDuplicates();
  TrimEntries(&tagged_slots_size);

#if V8_TARGET_ARCH_ARM || V8_TARGET_ARCH_ARM64
  // We cannot emit a const pool within the safepoint table.
  Assembler::BlockConstPoolScope block_const_pool(assembler);
#endif

  // Make sure the safepoint table is properly aligned. Pad with nops.
  assembler->Align(Code::kMetadataAlignment);
  assembler->RecordComment(";;; Safepoint table.");
  offset_ = assembler->pc_offset();

  // Compute the required sizes of the fields.
  int used_register_indexes = 0;
  STATIC_ASSERT(SafepointEntry::kNoTrampolinePC == -1);
  int max_pc = -1;
  STATIC_ASSERT(SafepointEntry::kNoDeoptIndex == -1);
  int max_deopt_index = -1;
  for (const EntryBuilder& entry : entries_) {
    used_register_indexes |= entry.register_indexes;
    max_pc = std::max(max_pc, std::max(entry.pc, entry.trampoline));
    max_deopt_index = std::max(max_deopt_index, entry.deopt_index);
  }

  // Derive the bytes and bools for the entry configuration from the values.
  auto value_to_bytes = [](int value) {
    DCHECK_LE(0, value);
    if (value == 0) return 0;
    if (value <= 0xff) return 1;
    if (value <= 0xffff) return 2;
    if (value <= 0xffffff) return 3;
    return 4;
  };
  bool has_deopt_data = max_deopt_index != -1;
  int register_indexes_size = value_to_bytes(used_register_indexes);
  // Add 1 so all values are non-negative.
  int pc_size = value_to_bytes(max_pc + 1);
  int deopt_index_size = value_to_bytes(max_deopt_index + 1);
  int tagged_slots_bytes =
      (tagged_slots_size + kBitsPerByte - 1) / kBitsPerByte;

  // Add a CHECK to ensure we never overflow the space in the bitfield, even for
  // huge functions which might not be covered by tests.
  CHECK(SafepointTable::RegisterIndexesSizeField::is_valid(
            register_indexes_size) &&
        SafepointTable::PcSizeField::is_valid(pc_size) &&
        SafepointTable::DeoptIndexSizeField::is_valid(deopt_index_size) &&
        SafepointTable::TaggedSlotsBytesField::is_valid(tagged_slots_bytes));

  uint32_t entry_configuration =
      SafepointTable::HasDeoptDataField::encode(has_deopt_data) |
      SafepointTable::RegisterIndexesSizeField::encode(register_indexes_size) |
      SafepointTable::PcSizeField::encode(pc_size) |
      SafepointTable::DeoptIndexSizeField::encode(deopt_index_size) |
      SafepointTable::TaggedSlotsBytesField::encode(tagged_slots_bytes);

  // Emit the table header.
  STATIC_ASSERT(SafepointTable::kLengthOffset == 0 * kIntSize);
  STATIC_ASSERT(SafepointTable::kEntryConfigurationOffset == 1 * kIntSize);
  STATIC_ASSERT(SafepointTable::kHeaderSize == 2 * kIntSize);
  int length = static_cast<int>(entries_.size());
  assembler->dd(length);
  assembler->dd(entry_configuration);

  auto emit_bytes = [assembler](int value, int bytes) {
    DCHECK_LE(0, value);
    for (; bytes > 0; --bytes, value >>= 8) assembler->db(value);
    DCHECK_EQ(0, value);
  };
  // Emit entries, sorted by pc offsets.
  for (const EntryBuilder& entry : entries_) {
    emit_bytes(entry.pc, pc_size);
    if (has_deopt_data) {
      // Add 1 so all values are non-negative.
      emit_bytes(entry.deopt_index + 1, deopt_index_size);
      emit_bytes(entry.trampoline + 1, pc_size);
    }
    emit_bytes(entry.register_indexes, register_indexes_size);
  }

  // Emit bitmaps of tagged stack slots.
  ZoneVector<uint8_t> bits(tagged_slots_bytes, 0, zone_);
  for (const EntryBuilder& entry : entries_) {
    std::fill(bits.begin(), bits.end(), 0);

    // Run through the indexes and build a bitmap.
    for (int idx : *entry.stack_indexes) {
      DCHECK_GT(tagged_slots_size, idx);
      int index = tagged_slots_size - 1 - idx;
      int byte_index = index >> kBitsPerByteLog2;
      int bit_index = index & (kBitsPerByte - 1);
      bits[byte_index] |= (1u << bit_index);
    }

    // Emit the bitmap for the current entry.
    for (uint8_t byte : bits) assembler->db(byte);
  }
}

void SafepointTableBuilder::RemoveDuplicates() {
  // Remove any duplicate entries, i.e. succeeding entries that are identical
  // except for the PC. During lookup, we will find the first entry whose PC is
  // not larger than the PC at hand, and find the first non-duplicate.

  if (entries_.size() < 2) return;

  auto is_identical_except_for_pc = [](const EntryBuilder& entry1,
                                       const EntryBuilder& entry2) {
    if (entry1.deopt_index != entry2.deopt_index) return false;
    DCHECK_EQ(entry1.trampoline, entry2.trampoline);

    ZoneChunkList<int>* indexes1 = entry1.stack_indexes;
    ZoneChunkList<int>* indexes2 = entry2.stack_indexes;
    if (indexes1->size() != indexes2->size()) return false;
    if (!std::equal(indexes1->begin(), indexes1->end(), indexes2->begin())) {
      return false;
    }

    if (entry1.register_indexes != entry2.register_indexes) return false;

    return true;
  };

  auto remaining_it = entries_.begin();
  size_t remaining = 0;

  for (auto it = entries_.begin(), end = entries_.end(); it != end;
       ++remaining_it, ++remaining) {
    if (remaining_it != it) *remaining_it = *it;
    // Merge identical entries.
    do {
      ++it;
    } while (it != end && is_identical_except_for_pc(*it, *remaining_it));
  }

  entries_.Rewind(remaining);
}

void SafepointTableBuilder::TrimEntries(int* tagged_slots_size) {
  int min_index = *tagged_slots_size;
  if (min_index == 0) return;  // Early exit: nothing to trim.

  for (auto& entry : entries_) {
    for (int idx : *entry.stack_indexes) {
      DCHECK_GT(*tagged_slots_size, idx);  // Validity check.
      if (idx >= min_index) continue;
      if (idx == 0) return;  // Early exit: nothing to trim.
      min_index = idx;
    }
  }

  DCHECK_LT(0, min_index);
  *tagged_slots_size -= min_index;
  for (auto& entry : entries_) {
    for (int& idx : *entry.stack_indexes) {
      idx -= min_index;
    }
  }
}

}  // namespace internal
}  // namespace v8
