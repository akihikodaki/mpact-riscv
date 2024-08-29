// Copyright 2023 Google LLC
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

#include <signal.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <ios>
#include <iostream>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "mpact/sim/generic/core_debug_interface.h"
#include "mpact/sim/generic/counters.h"
#include "mpact/sim/generic/instruction.h"
#include "mpact/sim/proto/component_data.pb.h"
#include "mpact/sim/util/memory/atomic_memory.h"
#include "mpact/sim/util/memory/flat_demand_memory.h"
#include "mpact/sim/util/memory/memory_watcher.h"
#include "mpact/sim/util/program_loader/elf_program_loader.h"
#include "re2/re2.h"
#include "riscv/debug_command_shell.h"
#include "riscv/riscv32_htif_semihost.h"
#include "riscv/riscv32g_vec_decoder.h"
#include "riscv/riscv_arm_semihost.h"
#include "riscv/riscv_fp_state.h"
#include "riscv/riscv_register.h"
#include "riscv/riscv_register_aliases.h"
#include "riscv/riscv_state.h"
#include "riscv/riscv_top.h"
#include "riscv/riscv_vector_state.h"
#include "src/google/protobuf/text_format.h"

using ::mpact::sim::generic::Instruction;
using ::mpact::sim::proto::ComponentData;
using ::mpact::sim::riscv::RiscV32GVecDecoder;
using ::mpact::sim::riscv::RiscV32HtifSemiHost;
using ::mpact::sim::riscv::RiscVArmSemihost;
using ::mpact::sim::riscv::RiscVFPState;
using ::mpact::sim::riscv::RiscVState;
using ::mpact::sim::riscv::RiscVVectorState;
using ::mpact::sim::riscv::RiscVXlen;
using ::mpact::sim::riscv::RV32Register;
using ::mpact::sim::riscv::RVFpRegister;

using AddressRange = mpact::sim::util::MemoryWatcher::AddressRange;

// Flags for specifying interactive mode.
ABSL_FLAG(bool, i, false, "Interactive mode");
ABSL_FLAG(bool, interactive, false, "Interactive mode");
// Flag for destination directory of proto file.
ABSL_FLAG(std::string, output_dir, "", "Output directory");
ABSL_FLAG(bool, semihost_htif, false, "HTIF semihosting");
ABSL_FLAG(bool, semihost_arm, false, "ARM semihosting");
// The RiscV gcc compiler bare metal library does not initialize the stack
// pointer before the program starts executing. It assumes that there is some
// other mechanism by which the stack pointer is initialized. For this simulator
// the stack pointer start and the stack size can be initialized in a couple of
// ways, including command line arguments, symbols defined in the executable,
// or a special program header entry in the executable.
//
// The following defines the optional flag for setting the stack size. If the
// stack size is not set using the flag, then the simulator will look in the
// executable to see if the GNU_STACK segment exists (assuming gcc RiscV
// compiler), and use that size. If not, it will use the value of the symbol
// __stack_size in the executable. If no such symbol exists, the stack size will
// be 32KB.
//
// A symbol may be defined in a C/C++ source file using asm, such as:
// asm(".global __stack_size\n"
//     ".equ __stack_size, 32 * 1024\n");
// The asm statement need not be inside a function body.
//
// The program header entry may be generated by adding the following to the
// gcc/g++ command line: -Wl,z,stack-size=N
//
ABSL_FLAG(std::optional<uint64_t>, stack_size, 32 * 1024,
          "Size of software stack");
// Optional flag for setting the location of the end of the stack (bottom). The
// beginning stack pointer is the value stack_end + stack_size. If this option
// is not set, it will use the value of the symbol __stack_end in the
// executable. If no such symbol exists, stack pointer initialization will not
// be performed by the simulator, and an appropriate crt0 library has to be
// used.
//
// A symbol may be defined in a C/C++ source file using asm, such as:
// asm(".global __stack_end\n"
//     ".equ __stack_end, 0x200000\n");
// The asm statement need not be inside a function body.
ABSL_FLAG(std::optional<uint64_t>, stack_end, 0,
          "Lowest valid address of software stack. "
          "Top of stack is stack_end + stack_size.");

// The following macro can be used in source code to define both the stack size
// and location:
//
// #define __STACK(addr, size) \
//  asm(".global __stack_size\n.equ __stack_size, " #size "\n"); \
//  asm(".global __stack_end\n.equ __stack_end, " #addr "\n");
//
// E.g.
//
// #include <stdio>
//
// __STACK(0x20000, 32 * 1024);
//
// int main(int, char **) {
//   printf("Hello World\n");
//   return 0;
// }
//

// Exit on execution of ecall instruction, default false.
ABSL_FLAG(bool, exit_on_ecall, false, "Exit on ecall - false by default");

constexpr char kStackEndSymbolName[] = "__stack_end";
constexpr char kStackSizeSymbolName[] = "__stack_size";

// Static pointer to the top instance. Used by the control-C handler.
static mpact::sim::riscv::RiscVTop *top = nullptr;

// Control-c handler to interrupt any running simulation.
static void sim_sigint_handler(int arg) {
  if (top != nullptr) {
    (void)top->Halt();
    return;
  } else {
    exit(-1);
  }
}

using ::mpact::sim::riscv::RiscVTop;

// Helper function to get the magic semihosting addresses from the loader.
static bool GetMagicAddresses(mpact::sim::util::ElfProgramLoader *loader,
                              RiscV32HtifSemiHost::SemiHostAddresses *magic) {
  auto result = loader->GetSymbol("tohost_ready");
  if (!result.ok()) return false;
  magic->tohost_ready = result.value().first;

  result = loader->GetSymbol("tohost");
  if (!result.ok()) return false;
  magic->tohost = result.value().first;

  result = loader->GetSymbol("fromhost_ready");
  if (!result.ok()) return false;
  magic->fromhost_ready = result.value().first;

  result = loader->GetSymbol("fromhost");
  if (!result.ok()) return false;
  magic->fromhost = result.value().first;

  return true;
}

// This is an example custom command that is added to the interactive
// debug command shell.
static bool PrintRegisters(
    absl::string_view input,
    const mpact::sim::riscv::DebugCommandShell::CoreAccess &core_access,
    std::string &output) {
  static const LazyRE2 reg_info_re{R"(\s*xyzreg\s+info\s*)"};
  if (!RE2::FullMatch(input, *reg_info_re)) {
    return false;
  }
  std::string output_str;
  for (int i = 0; i < 32; i++) {
    std::string reg_name = absl::StrCat("x", i);
    auto result = core_access.debug_interface->ReadRegister(reg_name);
    if (!result.ok()) {
      output = absl::StrCat("Failed to read register '", reg_name, "'");
      return true;
    }
    output_str +=
        absl::StrCat("x", absl::Dec(i, absl::kZeroPad2), " = [",
                     absl::Hex(result.value(), absl::kZeroPad8), "]\n");
  }
  output = output_str;
  return true;
}

int main(int argc, char **argv) {
  auto arg_vec = absl::ParseCommandLine(argc, argv);

  if (absl::GetFlag(FLAGS_semihost_htif) && absl::GetFlag(FLAGS_semihost_arm)) {
    LOG(ERROR) << "Cannot specify both htif and arm semihosting";
    std::cerr << "Only one semihosting mechanism can be specified" << std::endl;
  }

  if (arg_vec.size() > 2) {
    std::cerr << "Only a single input file allowed" << std::endl;
    return -1;
  }
  std::string full_file_name = arg_vec[1];
  std::string file_name =
      full_file_name.substr(full_file_name.find_last_of('/') + 1);
  std::string file_basename = file_name.substr(0, file_name.find_first_of('.'));

  auto *memory = new mpact::sim::util::FlatDemandMemory();
  auto *atomic_memory = new mpact::sim::util::AtomicMemory(memory);
  // Load the elf segments into memory.
  mpact::sim::util::ElfProgramLoader elf_loader(memory);
  auto load_result = elf_loader.LoadProgram(full_file_name);
  if (!load_result.ok()) {
    std::cerr << "Error while loading '" << full_file_name
              << "': " << load_result.status().message();
    return -1;
  }

  // Set up architectural state and decoder.
  RiscVState rv_state("RiscV32GV", RiscVXlen::RV32, memory, atomic_memory);
  // For floating point support add the fp state.
  RiscVFPState rv_fp_state(rv_state.csr_set(), &rv_state);
  RiscVVectorState rvv_state(&rv_state, 16 /*vector byte length*/);
  rv_state.set_rv_fp(&rv_fp_state);
  // Create the instruction decoder.
  RiscV32GVecDecoder rv_decoder(&rv_state, memory);

  // Make sure the architectural and abi register aliases are added.
  std::string reg_name;
  for (int i = 0; i < 32; i++) {
    reg_name = absl::StrCat(RiscVState::kXregPrefix, i);
    (void)rv_state.AddRegister<RV32Register>(reg_name);
    (void)rv_state.AddRegisterAlias<RV32Register>(
        reg_name, ::mpact::sim::riscv::kXRegisterAliases[i]);
  }
  for (int i = 0; i < 32; i++) {
    reg_name = absl::StrCat(RiscVState::kFregPrefix, i);
    (void)rv_state.AddRegister<RVFpRegister>(reg_name);
    (void)rv_state.AddRegisterAlias<RVFpRegister>(
        reg_name, ::mpact::sim::riscv::kFRegisterAliases[i]);
  }

  RiscVTop riscv_top("RiscV32GVSim", &rv_state, &rv_decoder);

  if (absl::GetFlag(FLAGS_exit_on_ecall)) {
    rv_state.set_on_ecall([&riscv_top](const Instruction *inst) -> bool {
      riscv_top.RequestHalt(RiscVTop::HaltReason::kProgramDone, inst);
      return true;
    });
  }

  // Initialize the PC to the entry point.
  uint32_t entry_point = load_result.value();
  auto pc_write = riscv_top.WriteRegister("pc", entry_point);
  if (!pc_write.ok()) {
    std::cerr << "Error writing to pc: " << pc_write.message();
    return -1;
  }

  // Initializing the stack pointer.

  // First see if there is a stack location defined, if not, do not initialize
  // the stack pointer.
  bool initialize_stack = false;
  uint64_t stack_end = 0;

  // Is the __stack_end symbol defined?
  auto res = elf_loader.GetSymbol(kStackEndSymbolName);
  if (res.ok()) {
    stack_end = res.value().first;
    initialize_stack = true;
  }

  // The stack_end flag overrides the __stack_end symbol.
  if (absl::GetFlag(FLAGS_stack_end).has_value()) {
    stack_end = absl::GetFlag(FLAGS_stack_end).value();
    initialize_stack = true;
  }

  // If there is a stack location, get the stack size, and write the sp.
  if (initialize_stack) {
    // Default size is 32KB.
    uint64_t stack_size = 32 * 1024;

    // Does the executable have a valid GNU_STACK segment? If so, override the
    // default
    auto loader_res = elf_loader.GetStackSize();
    if (loader_res.ok()) {
      stack_end = loader_res.value();
    }

    // If the __stack_size symbol is defined then override.
    auto res = elf_loader.GetSymbol(kStackSizeSymbolName);
    if (res.ok()) {
      stack_size = res.value().first;
    }

    // If the flag is set, then override.
    if (absl::GetFlag(FLAGS_stack_size).has_value()) {
      stack_size = absl::GetFlag(FLAGS_stack_size).value();
    }

    auto sp_write = riscv_top.WriteRegister("sp", stack_end + stack_size);
    if (!sp_write.ok()) {
      std::cerr << "Error writing to sp: " << sp_write.message();
      return -1;
    }
  }

  mpact::sim::util::MemoryWatcher *watcher = nullptr;
  RiscV32HtifSemiHost *htif_semihost = nullptr;
  if (absl::GetFlag(FLAGS_semihost_htif)) {
    // Add htif semihosting.
    RiscV32HtifSemiHost::SemiHostAddresses magic_addresses;
    if (GetMagicAddresses(&elf_loader, &magic_addresses)) {
      watcher = new mpact::sim::util::MemoryWatcher(memory);
      htif_semihost = new RiscV32HtifSemiHost(
          watcher, memory, magic_addresses,
          [&riscv_top]() {
            riscv_top.RequestHalt(RiscVTop::HaltReason::kSemihostHaltRequest,
                                  nullptr);
          },
          [&riscv_top](std::string) {
            riscv_top.RequestHalt(RiscVTop::HaltReason::kSemihostHaltRequest,
                                  nullptr);
          });
      riscv_top.state()->set_memory(watcher);
    }
  }

  RiscVArmSemihost *arm_semihost = nullptr;
  if (absl::GetFlag(FLAGS_semihost_arm)) {
    // Add ARM semihosting.
    arm_semihost = new RiscVArmSemihost(RiscVArmSemihost::BitWidth::kWord32,
                                        memory, memory);
    riscv_top.state()->AddEbreakHandler(
        [arm_semihost](const Instruction *inst) -> bool {
          if (arm_semihost->IsSemihostingCall(inst)) {
            arm_semihost->OnEBreak(inst);
            return true;
          }
          return false;
        });
    arm_semihost->set_exit_callback([&riscv_top]() {
      riscv_top.RequestHalt(RiscVTop::HaltReason::kSemihostHaltRequest,
                            nullptr);
    });
  }

  mpact::sim::generic::SimpleCounter<double> counter_sec("simulation_time_sec",
                                                         0.0);

  CHECK_OK(riscv_top.AddCounter(&counter_sec));
  // Set up control-c handling.
  top = &riscv_top;
  struct sigaction sa;
  sa.sa_flags = 0;
  sigemptyset(&sa.sa_mask);
  sigaddset(&sa.sa_mask, SIGINT);
  sa.sa_handler = &sim_sigint_handler;
  sigaction(SIGINT, &sa, nullptr);

  // Determine if this is being run interactively or as a batch job.
  bool interactive = absl::GetFlag(FLAGS_i) || absl::GetFlag(FLAGS_interactive);
  if (interactive) {
    mpact::sim::riscv::DebugCommandShell cmd_shell;
    cmd_shell.AddCore({&riscv_top, [&elf_loader]() { return &elf_loader; }});
    // Add custom command to interactive debug command shell.
    cmd_shell.AddCommand(
        "    reg info                       - print all scalar regs",
        PrintRegisters);
    cmd_shell.Run(std::cin, std::cout);
  } else {
    std::cerr << "Starting simulation\n";

    auto t0 = absl::Now();

    auto run_status = riscv_top.Run();
    if (!run_status.ok()) {
      std::cerr << run_status.message() << std::endl;
    }

    auto wait_status = riscv_top.Wait();
    if (!wait_status.ok()) {
      std::cerr << wait_status.message() << std::endl;
    }

    auto t1 = absl::Now();
    absl::Duration duration = t1 - t0;
    double sec = static_cast<double>(duration / absl::Milliseconds(100)) / 10;
    counter_sec.SetValue(sec);

    std::cerr << absl::StrFormat("Simulation done: %0.1f sec\n", sec)
              << std::endl;
  }

  // Export counters.
  auto component_proto = std::make_unique<ComponentData>();
  CHECK_OK(riscv_top.Export(component_proto.get())) << "Failed to export proto";
  std::string proto_file_name;
  if (FLAGS_output_dir.CurrentValue().empty()) {
    proto_file_name = "./" + file_basename + ".proto";
  } else {
    proto_file_name =
        FLAGS_output_dir.CurrentValue() + "/" + file_basename + ".proto";
  }
  std::fstream proto_file(proto_file_name.c_str(), std::ios_base::out);
  std::string serialized;
  if (!proto_file.good() || !google::protobuf::TextFormat::PrintToString(
                                *component_proto.get(), &serialized)) {
    LOG(ERROR) << "Failed to write proto to file";
  } else {
    proto_file << serialized;
    proto_file.close();
  }

  // Cleanup.
  auto status = riscv_top.ClearAllSwBreakpoints();
  if (!status.ok()) {
    LOG(ERROR) << "Error in ClearAllSwBreakpoints: " << status.message();
  }
  delete atomic_memory;
  delete memory;
  delete watcher;
  delete arm_semihost;
  delete htif_semihost;
}