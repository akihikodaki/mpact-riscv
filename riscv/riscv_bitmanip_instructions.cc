// Copyright 2024 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "riscv/riscv_bitmanip_instructions.h"

#include <algorithm>
#include <cstdint>

#include "absl/numeric/bits.h"
#include "mpact/sim/generic/instruction.h"
#include "riscv/riscv_instruction_helpers.h"
#include "riscv/riscv_register.h"
#include "riscv/riscv_state.h"

// This file contains the semantic function definitions for the bit manipulation
// instructions in RiscV.

namespace mpact::sim::riscv {

using ::mpact::sim::generic::Instruction;

namespace RV32 {

using RegisterType = RV32Register;
using UIntReg = uint32_t;
using IntReg = int32_t;
constexpr int kXlen = sizeof(UIntReg) * 8;

// Add with shift.
void RiscVShAdd(const Instruction *instruction) {
  auto shift = generic::GetInstructionSource<UIntReg>(instruction, 2);
  RiscVBinaryOp<RegisterType, UIntReg, UIntReg>(
      instruction, [shift](UIntReg a, UIntReg b) { return b + (a << shift); });
}

// And not.
void RiscVAndn(const Instruction *instruction) {
  RiscVBinaryOp<RegisterType, UIntReg, UIntReg>(
      instruction, [](UIntReg a, UIntReg b) { return a & ~b; });
}

// Or not.
void RiscVOrn(const Instruction *instruction) {
  RiscVBinaryOp<RegisterType, UIntReg, UIntReg>(
      instruction, [](UIntReg a, UIntReg b) { return a | ~b; });
}

// Xnor.
void RiscVXnor(const Instruction *instruction) {
  RiscVBinaryOp<RegisterType, UIntReg, UIntReg>(
      instruction, [](UIntReg a, UIntReg b) { return ~(a ^ b); });
}

// Count leading zeros.
void RiscVClz(const Instruction *instruction) {
  RiscVUnaryOp<RegisterType, UIntReg, UIntReg>(
      instruction, [](UIntReg a) -> UIntReg { return absl::countl_zero(a); });
}

// Count trailing zeros.
void RiscVCtz(const Instruction *instruction) {
  RiscVUnaryOp<RegisterType, UIntReg, UIntReg>(
      instruction, [](UIntReg a) -> UIntReg { return absl::countr_zero(a); });
}

// Bit population count.
void RiscVCpop(const Instruction *instruction) {
  RiscVUnaryOp<RegisterType, UIntReg, UIntReg>(
      instruction, [](UIntReg a) -> UIntReg { return absl::popcount(a); });
}

// Signed max.
void RiscVMax(const Instruction *instruction) {
  RiscVBinaryOp<RegisterType, IntReg, IntReg>(
      instruction, [](IntReg a, IntReg b) { return std::max(a, b); });
}

// Unsigned max.
void RiscVMaxu(const Instruction *instruction) {
  RiscVBinaryOp<RegisterType, UIntReg, UIntReg>(
      instruction, [](UIntReg a, UIntReg b) { return std::max(a, b); });
}

// Signed min.
void RiscVMin(const Instruction *instruction) {
  RiscVBinaryOp<RegisterType, IntReg, IntReg>(
      instruction, [](IntReg a, IntReg b) { return std::min(a, b); });
}

// Unsigned min.
void RiscVMinu(const Instruction *instruction) {
  RiscVBinaryOp<RegisterType, UIntReg, UIntReg>(
      instruction, [](UIntReg a, UIntReg b) { return std::min(a, b); });
}

// Sign extend byte.
void RiscVSextB(const Instruction *instruction) {
  RiscVUnaryOp<RegisterType, IntReg, int8_t>(
      instruction, [](int8_t a) -> IntReg { return static_cast<IntReg>(a); });
}

// Sign extend half word.
void RiscVSextH(const Instruction *instruction) {
  RiscVUnaryOp<RegisterType, IntReg, int16_t>(
      instruction, [](int16_t a) -> IntReg { return static_cast<IntReg>(a); });
}

// Zero extend half word.
void RiscVZextH(const Instruction *instruction) {
  RiscVUnaryOp<RegisterType, UIntReg, uint16_t>(
      instruction,
      [](uint16_t a) -> UIntReg { return static_cast<UIntReg>(a); });
}

// Rotate left.
void RiscVrol(const Instruction *instruction) {
  RiscVBinaryOp<RegisterType, UIntReg, UIntReg>(
      instruction,
      [](UIntReg a, UIntReg b) { return (a << b) | (a >> (32 - b)); });
}

// Rotate right.
void RiscVror(const Instruction *instruction) {
  RiscVBinaryOp<RegisterType, UIntReg, UIntReg>(
      instruction,
      [](UIntReg a, UIntReg b) { return (a >> b) | (a << (32 - b)); });
}

// Or combine (byte wise).
void RiscVOrcb(const Instruction *instruction) {
  RiscVUnaryOp<RegisterType, UIntReg, UIntReg>(instruction, [](UIntReg a) {
    UIntReg mask = 0xff;
    UIntReg result = 0;
    for (int i = 0; i < 4; i++) {
      if (a & mask) {
        result |= mask;
      }
      mask <<= 8;
    }
    return result;
  });
}

// Byte reverse.
void RiscVRev8(const Instruction *instruction) {
  RiscVUnaryOp<RegisterType, UIntReg, UIntReg>(instruction, [](UIntReg a) {
    UIntReg result = 0;
    for (int i = 0; i < sizeof(UIntReg); i++) {
      result <<= 8;
      result |= a & 0xff;
      a >>= 8;
    }
    return result;
  });
}

// Carry-less multiplication (using xor) - low 32 bits.
void RiscVClmul(const Instruction *instruction) {
  RiscVBinaryOp<RegisterType, UIntReg, UIntReg>(
      instruction, [](UIntReg a, UIntReg b) {
        UIntReg result = 0;
        for (int i = 0; i < kXlen; i++) {
          result = ((b >> i) & 1) ? result ^ (a << i) : result;
        }
        return result;
      });
}

// Carry-less multiplication (using xor) - high 32 bits.
void RiscVClmulh(const Instruction *instruction) {
  RiscVBinaryOp<RegisterType, UIntReg, UIntReg>(
      instruction, [](UIntReg a, UIntReg b) {
        UIntReg result = 0;
        for (int i = 1; i < kXlen; i++) {
          result = ((b >> i) & 1) ? result ^ (a >> (kXlen - i)) : result;
        }
        return result;
      });
}

// Reverse carry-less multiplication (using xor).
void RiscVClmulr(const Instruction *instruction) {
  RiscVBinaryOp<RegisterType, UIntReg, UIntReg>(
      instruction, [](UIntReg a, UIntReg b) {
        UIntReg result = 0;
        for (int i = 0; i < kXlen - 1; i++) {
          result = ((b >> i) & 1) ? result ^ (a >> (kXlen - i - 1)) : result;
        }
        return result;
      });
}

// Clear bit.
void RiscVBclr(const Instruction *instruction) {
  RiscVBinaryOp<RegisterType, UIntReg, UIntReg>(
      instruction,
      [](UIntReg a, UIntReg b) { return a & ~(1U << (b & 0x1f)); });
}

// Extract bit.
void RiscVBext(const Instruction *instruction) {
  RiscVBinaryOp<RegisterType, UIntReg, UIntReg>(
      instruction,
      [](UIntReg a, UIntReg b) { return (a >> (b & 0x1f)) & 0x1; });
}

// Invert bit.
void RiscVBinv(const Instruction *instruction) {
  RiscVBinaryOp<RegisterType, UIntReg, UIntReg>(
      instruction, [](UIntReg a, UIntReg b) { return a ^ (1U << (b & 0x1f)); });
}

// Set bit.
void RiscVBset(const Instruction *instruction) {
  RiscVBinaryOp<RegisterType, UIntReg, UIntReg>(
      instruction, [](UIntReg a, UIntReg b) { return a | (1U << (b & 0x1f)); });
}

}  // namespace RV32

}  // namespace mpact::sim::riscv