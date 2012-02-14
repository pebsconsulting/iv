#ifndef IV_LV5_RAILGUN_INSTRUCTION_FWD_H_
#define IV_LV5_RAILGUN_INSTRUCTION_FWD_H_
#include <iv/debug.h>
#include <iv/lv5/symbol.h>
#include <iv/lv5/railgun/op.h>
#include <iv/lv5/railgun/register_id.h>
namespace iv {
namespace lv5 {

class Map;
class Chain;

namespace railgun {

struct Instruction {
  Instruction(uint32_t arg) : u32(arg) { }  // NOLINT

  Instruction(RegisterID reg) {  // NOLINT
    assert(reg);
    i32 = reg->register_offset();
  }

  union {
    const void* label;  // use for direct threading
    uint32_t u32;
    int32_t i32;
    ptrdiff_t diff;
    int16_t reg[2];
    Map* map;
    Chain* chain;
  };

  static Instruction Diff(ptrdiff_t to, ptrdiff_t from) {
    Instruction instr(0u);
    instr.diff = to - from;
    return instr;
  }

  inline OP::Type GetOP() const;

  static inline Instruction GetOPInstruction(OP::Type op);

  std::size_t GetLength() const {
    return kOPLength[GetOP()];
  }
};

} } }  // namespace iv::lv5::railgun

// global scope
GC_DECLARE_PTRFREE(iv::lv5::railgun::Instruction);

#endif  // IV_LV5_RAILGUN_INSTRUCTION_FWD_H_
