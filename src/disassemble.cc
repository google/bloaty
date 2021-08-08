// Copyright 2016 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <string>

#include "absl/strings/ascii.h"
#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/strings/substitute.h"
#include "bloaty.h"
#include "capstone/capstone.h"
#include "re.h"
#include "util.h"

using absl::string_view;

namespace bloaty {

namespace {

static std::string RightPad(const std::string& input, size_t size) {
  std::string ret = input;
  while (ret.size() < size) {
    ret += " ";
  }
  return ret;
}

}  // anonymous namespace

void DisassembleFindReferences(const DisassemblyInfo& info, RangeSink* sink) {
  if (info.arch != CS_ARCH_X86) {
    // x86 only for now.
    return;
  }

  csh handle;
  if (cs_open(info.arch, info.mode, &handle) != CS_ERR_OK ||
      cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK) {
    THROW("Couldn't initialize Capstone");
  }

  if (info.text.size() == 0) {
    cs_close(&handle);
    THROW("Tried to disassemble empty function.");
  }

  cs_insn *in = cs_malloc(handle);
  uint64_t address = info.start_address;
  const uint8_t* ptr = reinterpret_cast<const uint8_t*>(info.text.data());
  size_t size = info.text.size();

  while (size > 0) {
    if (!cs_disasm_iter(handle, &ptr, &size, &address, in)) {
      // Some symbols that end up in the .text section aren't really functions
      // but data.  Not sure why this happens.
      if (verbose_level > 1) {
        printf("Error disassembling function at address: %" PRIx64 "\n",
               address);
      }
      goto cleanup;
    }

    size_t count = in->detail->x86.op_count;
    for (size_t i = 0; i < count; i++) {
      cs_x86_op* op = &in->detail->x86.operands[i];
      if (op->type == X86_OP_MEM && op->mem.base == X86_REG_RIP &&
          op->mem.segment == X86_REG_INVALID &&
          op->mem.index == X86_REG_INVALID) {
        uint64_t to_address = in->address + in->size + op->mem.disp;
        if (to_address) {
          sink->AddVMRangeForVMAddr("x86_disassemble", in->address, to_address,
                                    RangeSink::kUnknownSize);
        }
      }
    }
  }

cleanup:
  cs_free(in, 1);
  cs_close(&handle);
}

bool TryGetJumpTarget(cs_arch arch, cs_insn *in, uint64_t* target) {
  switch (arch) {
    case CS_ARCH_X86:
      switch (in->id) {
        case X86_INS_JAE:
        case X86_INS_JA:
        case X86_INS_JBE:
        case X86_INS_JB:
        case X86_INS_JCXZ:
        case X86_INS_JECXZ:
        case X86_INS_JE:
        case X86_INS_JGE:
        case X86_INS_JG:
        case X86_INS_JLE:
        case X86_INS_JL:
        case X86_INS_JMP:
        case X86_INS_JNE:
        case X86_INS_JNO:
        case X86_INS_JNP:
        case X86_INS_JNS:
        case X86_INS_JO:
        case X86_INS_JP:
        case X86_INS_JS:
        case X86_INS_CALL: {
          auto op0 = in->detail->x86.operands[0];
          if (op0.type == X86_OP_IMM) {
            *target = op0.imm;
            return true;
          }
          return false;
        }
        default:
          return false;
      }
    default:
      return false;
  }
}

std::string DisassembleFunction(const DisassemblyInfo& info) {
  std::string ret;

  csh handle;
  if (cs_open(info.arch, info.mode, &handle) != CS_ERR_OK ||
      cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK) {
    THROW("Couldn't initialize Capstone");
  }

  if (info.text.size() == 0) {
    THROW("Tried to disassemble empty function.");
  }

  cs_insn *insn;
  size_t count =
      cs_disasm(handle, reinterpret_cast<const uint8_t *>(info.text.data()),
                info.text.size(), info.start_address, 0, &insn);

  if (count == 0) {
    THROW("Error disassembling function.");
  }

  std::map<uint64_t, int> local_labels;

  for (size_t i = 0; i < count; i++) {
    cs_insn *in = insn + i;
    uint64_t target;
    if (TryGetJumpTarget(info.arch, in, &target) &&
        target >= info.start_address &&
        target < info.start_address + info.text.size()) {
      local_labels[target] = 0;  // Fill in real value later.
    }
  }

  int label = 0;
  for (auto& pair : local_labels) {
    pair.second = label++;
  }

  for (size_t i = 0; i < count; i++) {
    cs_insn *in = insn + i;
    std::string bytes = absl::BytesToHexString(
        string_view(reinterpret_cast<const char*>(in->bytes), in->size));
    string_view mnemonic(in->mnemonic);
    std::string op_str(in->op_str);
    std::string match;
    std::string label;

    if (info.arch == CS_ARCH_X86) {
      if (in->id == X86_INS_LEA) {
        ReImpl::GlobalReplace(&op_str, "\\w?word ptr ", "");
      } else if (in->id == X86_INS_NOP) {
        op_str.clear();
      } else {
        // qword ptr => QWORD
        while (ReImpl::PartialMatch(op_str, "(\\w?word) ptr", &match)) {
          std::string upper_match = match;
          absl::AsciiStrToUpper(&upper_match);
          ReImpl::Replace(&op_str, match + " ptr", upper_match);
        }
      }
    }

    ReImpl::GlobalReplace(&op_str, " ", "");

    auto iter = local_labels.find(in->address);
    if (iter != local_labels.end()) {
      label = std::to_string(iter->second) + ":";
    }

    uint64_t target;
    if (TryGetJumpTarget(info.arch, in, &target)) {
      auto iter = local_labels.find(target);
      std::string label;
      if (iter != local_labels.end()) {
        if (target > in->address) {
          op_str = ">" + std::to_string(iter->second);
        } else {
          op_str = "<" + std::to_string(iter->second);
        }
      } else if (info.symbol_map.vm_map.TryGetLabel(target, &label)) {
        op_str = label;
      }
    }

    absl::StrAppend(&ret, " ", RightPad(label, 4),
                    RightPad(std::string(mnemonic), 8), " ", op_str, "\n");
  }

  cs_close(&handle);
  return ret;
}

}  // namespace bloaty
