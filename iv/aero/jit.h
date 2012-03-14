// AeroHand RegExp Runtime JIT Compiler
// only works on x64 and gcc or clang calling convensions
//
// special thanks
//   MITSUNARI Shigeo
//
#ifndef IV_AERO_JIT_H_
#define IV_AERO_JIT_H_
#include <vector>
#include <iv/detail/cstdint.h>
#include <iv/detail/type_traits.h>
#include <iv/no_operator_names_guard.h>
#include <iv/static_assert.h>
#include <iv/conversions.h>
#include <iv/assoc_vector.h>
#include <iv/utils.h>
#include <iv/aero/op.h>
#include <iv/aero/code.h>
#include <iv/aero/utility.h>
#include <iv/aero/jit_fwd.h>
#if defined(IV_ENABLE_JIT)
#include <iv/third_party/xbyak/xbyak.h>

namespace iv {
namespace aero {
namespace jit_detail {

static const char* kQuickCheckNextLabel = "QUICK_CHECK_NEXT";
static const char* kBackTrackLabel = "BACKTRACK";
static const char* kReturnLabel = "RETURN";
static const char* kFailureLabel = "FAILURE";
static const char* kErrorLabel = "ERROR";
static const char* kSuccessLabel = "SUCCESS";
static const char* kStartLabel = "START";

template<typename CharT = char>
struct Reg {
  typedef Xbyak::Reg8 type;
  static const type& GetR10(Xbyak::CodeGenerator* gen) {
    return gen->r10b;
  }
  static const type& GetR11(Xbyak::CodeGenerator* gen) {
    return gen->r11b;
  }
};

template<>
struct Reg<uint16_t> {
  typedef Xbyak::Reg16 type;
  static const type& GetR10(Xbyak::CodeGenerator* gen) {
    return gen->r10w;
  }
  static const type& GetR11(Xbyak::CodeGenerator* gen) {
    return gen->r11w;
  }
};

}  // namespace jit_detail

template<typename CharT>
class JIT : public Xbyak::CodeGenerator {
 public:
  // x64 JIT
  //
  // r10 : tmp
  // r11 : tmp
  //
  // callee-save
  // r12 : const char* subject
  // r13 : uint64_t size
  // r14 : int* captures
  // r15 : current position
  // rbx : stack position

  typedef std::unordered_map<int, uintptr_t> BackTrackMap;
  typedef typename jit_detail::Reg<CharT>::type RegC;
  typedef typename JITExecutable<CharT>::Executable Executable;

  IV_STATIC_ASSERT((std::is_same<CharT, char>::value ||
                    std::is_same<CharT, uint16_t>::value));

  static const int kIntSize = core::Size::kIntSize;
  static const int kPtrSize = core::Size::kPointerSize;
  static const int kCharSize = sizeof(CharT);  // NOLINT
  static const int k8Size = sizeof(uint8_t);  // NOLINT
  static const int k16Size = sizeof(uint16_t);  // NOLINT
  static const int k32Size = sizeof(uint32_t);  // NOLINT
  static const int k64Size = sizeof(uint64_t);  // NOLINT

  static const int kASCII = kCharSize == 1;

  explicit JIT(const Code& code)
    : Xbyak::CodeGenerator(8192 * 4),
      code_(code),
      first_instr_(code.bytes().data()),
      targets_(),
      backtracks_(),
      tracked_(),
      character(kASCII ? byte : word),
      subject_(r12),
      size_(r13),
      captures_(r14),
      cp_(r15),
      cpd_(r15d),
      sp_(rbx),
      spd_(ebx),
      ch10_(jit_detail::Reg<CharT>::GetR10(this)),
      ch11_(jit_detail::Reg<CharT>::GetR11(this)) {
  }

  Executable Compile() {
    // start
    ScanJumpReference();
    EmitPrologue();
    Main();
    EmitEpilogue();
    return Get();
  }

  Executable Get() {
    return core::BitCast<Executable>(getCode());
  }

 private:
  void LoadVM(const Xbyak::Reg64& dst) {
    mov(dst, ptr[rbp - 8]);
  }

  void LoadResult(const Xbyak::Reg64& dst) {
    mov(dst, ptr[rbp - 16]);
  }

  void LoadStack(const Xbyak::Reg64& dst) {
    LoadVM(dst);
    mov(dst, ptr[dst]);
  }

  void LoadStateSize(const Xbyak::Reg64& dst) {
    LoadVM(dst);
    mov(dst, qword[dst + kPtrSize * 2 + k64Size]);
  }

  static int StaticPrint(const char* format, int64_t ch) {
    printf("%s: %lld\n", format, ch);
    return 0;
  }

  // debug
  template<typename REG>
  void Put(const char* format, const REG& reg) {
    push(r10);
    push(r11);
    push(rdi);
    push(rsi);
    push(rax);
    push(rdx);
    push(rcx);
    push(r8);

    mov(rdi, core::BitCast<uintptr_t>(format));
    mov(rsi, reg);
    mov(rax, core::BitCast<uintptr_t>(&StaticPrint));
    call(rax);

    pop(r8);
    pop(rcx);
    pop(rdx);
    pop(rax);
    pop(rsi);
    pop(rdi);
    pop(r11);
    pop(r10);
  }

  void ScanJumpReference() {
    // scan jump referenced opcode, and record it to jump table
    Code::Data::const_pointer instr = first_instr_;
    const Code::Data::const_pointer last =
        code_.bytes().data() + code_.bytes().size();
    while (instr != last) {
      const uint8_t opcode = *instr;
      uint32_t length = kOPLength[opcode];
      if (opcode == OP::CHECK_RANGE || opcode == OP::CHECK_RANGE_INVERTED) {
        length += Load4Bytes(instr + 1);
      }
      switch (opcode) {
        case OP::PUSH_BACKTRACK: {
          const int dis = Load4Bytes(instr + 1);
          if (backtracks_.find(dis) == backtracks_.end()) {
            backtracks_.insert(std::make_pair(dis, backtracks_.size()));
          }
          break;
        }
        case OP::COUNTER_NEXT: {
          targets_.insert(Load4Bytes(instr + 9));
          break;
        }
        case OP::ASSERTION_SUCCESS: {
          targets_.insert(Load4Bytes(instr + 5));
          break;
        }
        case OP::JUMP: {
          targets_.insert(Load4Bytes(instr + 1));
          break;
        }
      }
      std::advance(instr, length);
    }
    tracked_.resize(backtracks_.size(), 0);
  }

  void Main() {
    // main pass
    Code::Data::const_pointer instr = first_instr_;
    const Code::Data::const_pointer last =
        code_.bytes().data() + code_.bytes().size();
    while (instr != last) {
      const uint8_t opcode = *instr;
      uint32_t length = kOPLength[opcode];
      if (opcode == OP::CHECK_RANGE || opcode == OP::CHECK_RANGE_INVERTED) {
        length += Load4Bytes(instr + 1);
      }

#define INTERCEPT()\
  do {\
    mov(r10, offset);\
    Put("OPCODE", r10);\
  } while (0)

#define V(op, N)\
  case OP::op: {\
    const uint32_t offset = instr - first_instr_;\
    if (std::binary_search(targets_.begin(), targets_.end(), offset)) {\
      DefineLabel(offset);\
    }\
    const BackTrackMap::const_iterator it = backtracks_.find(offset);\
    if (it != backtracks_.end()) {\
      tracked_[it->second] = core::BitCast<uintptr_t>(getCurr());\
    }\
    /* INTERCEPT(); */\
    Emit##op(instr, length);\
    break;\
  }
      switch (opcode) {
IV_AERO_OPCODES(V)
      }
#undef V
#undef INTERCEPT
      std::advance(instr, length);
    }
  }

  void EmitQuickCheck() {
    const uint16_t filter = code_.filter();
    inLocalLabel();
    if (!filter) {
      // normal path
      L(".QUICK_CHECK_NORMAL_START");
      jmp(jit_detail::kStartLabel, T_NEAR);
      L(jit_detail::kQuickCheckNextLabel);
      inc(cp_);
      cmp(cp_, size_);
      jle(".QUICK_CHECK_NORMAL_START");
      jmp(jit_detail::kFailureLabel, T_NEAR);
    } else {
      // one char check and bloom filter path
      if (kASCII && !core::character::IsASCII(filter)) {
        jmp(jit_detail::kFailureLabel, T_NEAR);
        return;
      }
      L(".QUICK_CHECK_SPECIAL_START");
      cmp(cp_, size_);
      jge(jit_detail::kFailureLabel, T_NEAR);
      movzx(r10, character[subject_ + cp_ * kCharSize]);
      if (code_.IsQuickCheckOneChar()) {
        cmp(r10, filter);
      } else {
        mov(ch11_, ch10_);
        and(ch10_, filter);
        cmp(ch10_, ch11_);
      }
      jne(jit_detail::kQuickCheckNextLabel);
      jmp(jit_detail::kStartLabel, T_NEAR);
      L(jit_detail::kQuickCheckNextLabel);
      inc(cp_);
      jmp(".QUICK_CHECK_SPECIAL_START");
    }
    outLocalLabel();
  }

  void EmitPrologue() {
    // initialize capture buffer and put arguments registers
    push(rbp);
    mov(rbp, rsp);
    sub(rsp, 8 * 8);
    mov(qword[rbp - 8 * 1], rdi);  // push vm to stack
    mov(qword[rbp - 8 * 2], rcx);  // push captures to stack
    // 8 * 3 is reserved for cp store
    mov(qword[rbp - 8 * 4], subject_);
    mov(qword[rbp - 8 * 5], captures_);
    mov(qword[rbp - 8 * 6], size_);
    mov(qword[rbp - 8 * 7], cp_);
    mov(qword[rbp - 8 * 8], sp_);

    // calling convension is
    // Execute(VM* vm, const char* subject,
    //         uint32_t size, int* captures, uint32_t cp)
    mov(subject_, rsi);
    mov(size_, rdx);
    mov(cp_, r8);

    const std::size_t size = code_.captures() * 2 + code_.counters() + 1;
    {
      // allocate state space
      // rdi is already VM*
      inLocalLabel();
      mov(captures_, ptr[rdi + kPtrSize]);  // load state
      cmp(qword[rdi + kPtrSize * 2 + k64Size], size);
      jge(".EXIT", T_NEAR);
      xchg(rdi, captures_);
      mov(rax, core::BitCast<uintptr_t>(&std::free));
      call(rax);
      // state size is too small
      const std::size_t allocated = IV_ROUNDUP(size, VM::kInitialStateSize);
      mov(rdi, allocated * kIntSize);
      mov(rax, core::BitCast<uintptr_t>(&std::malloc));
      call(rax);
      mov(qword[captures_ + kPtrSize], rax);
      mov(qword[captures_ + kPtrSize * 2 + k64Size], allocated);
      mov(captures_, rax);
      L(".EXIT");
      outLocalLabel();
    }

    // generate quick check path
    EmitQuickCheck();

    L(jit_detail::kStartLabel);
    {
      inLocalLabel();
      mov(qword[rbp - 8 * 3], cp_);
      // initialize sp_ to 0
      mov(sp_, 0);

      // initialize captures
      mov(dword[captures_], cpd_);
      lea(r10, ptr[captures_ + kIntSize]);
      mov(r11, size - 1);
      test(r11, r11);
      jz(".LOOP_END");
      L(".LOOP_START");
      mov(dword[r10], kUndefined);
      add(r10, kIntSize);
      sub(r11, 1);
      jnz(".LOOP_START");
      L(".LOOP_END");
      outLocalLabel();
    }
  }

  void EmitEpilogue() {
    // generate success path
    L(jit_detail::kSuccessLabel);
    {
      inLocalLabel();
      mov(dword[captures_ + kIntSize], cpd_);
      // copy to result
      LoadResult(r10);
      mov(r11, code_.captures() * 2);
      mov(rax, captures_);

      test(r11, r11);
      jz(".LOOP_END");

      L(".LOOP_START");
      mov(ecx, dword[rax]);
      add(rax, kIntSize);
      mov(dword[r10], ecx);
      add(r10, kIntSize);
      sub(r11, 1);
      jnz(".LOOP_START");
      L(".LOOP_END");
      mov(rax, AERO_SUCCESS);
      outLocalLabel();
      // fall through
    }

    // generate last return path
    L(jit_detail::kReturnLabel);
    {
      mov(subject_, qword[rbp - 8 * 4]);
      mov(captures_, qword[rbp - 8 * 5]);
      mov(size_, qword[rbp - 8 * 6]);
      mov(cp_, qword[rbp - 8 * 7]);
      mov(sp_, qword[rbp - 8 * 8]);
      mov(rsp, rbp);
      pop(rbp);
      ret();
      align(16);
    }

    // generate last failure path
    L(jit_detail::kFailureLabel);
    {
      mov(rax, AERO_FAILURE);
      jmp(jit_detail::kReturnLabel);
    }

    // generate backtrack path
    {
      inLocalLabel();
      L(jit_detail::kBackTrackLabel);
      test(sp_, sp_);
      jz(".FAILURE", T_NEAR);
      const int size = code_.captures() * 2 + code_.counters() + 1;
      sub(sp_, size);

      // copy to captures
      mov(r10, captures_);
      mov(r11, size);
      LoadStack(rax);
      lea(rax, ptr[rax + sp_ * kIntSize]);

      test(r11, r11);
      jz(".LOOP_END");

      L(".LOOP_START");
      mov(ecx, dword[rax]);
      add(rax, kIntSize);
      mov(dword[r10], ecx);
      add(r10, kIntSize);
      sub(r11, 1);
      jnz(".LOOP_START");
      L(".LOOP_END");

      movsxd(cp_, dword[captures_ + kIntSize]);
      movsxd(rcx, dword[captures_ + kIntSize * (size - 1)]);
      mov(rax, core::BitCast<uintptr_t>(tracked_.data()));
      jmp(ptr[rax + rcx * kPtrSize]);

      L(".FAILURE");
      mov(cp_, qword[rbp - 8 * 3]);
      jmp(jit_detail::kQuickCheckNextLabel, T_NEAR);
      outLocalLabel();
    }

    // generate error path
    L(jit_detail::kErrorLabel);
    {
      mov(rax, AERO_ERROR);
      jmp(jit_detail::kReturnLabel);
    }
  }

  void EmitSizeGuard() {
    cmp(cp_, size_);
    jge(jit_detail::kBackTrackLabel, T_NEAR);
  }

  void EmitSTORE_SP(const uint8_t* instr, uint32_t len) {
    const uint32_t offset = code_.captures() * 2 + Load4Bytes(instr + 1);
    mov(dword[captures_ + kIntSize * offset], spd_);
  }

  void EmitSTORE_POSITION(const uint8_t* instr, uint32_t len) {
    const uint32_t offset = code_.captures() * 2 + Load4Bytes(instr + 1);
    mov(dword[captures_ + kIntSize * offset], cpd_);
  }

  void EmitPOSITION_TEST(const uint8_t* instr, uint32_t len) {
    const uint32_t offset = code_.captures() * 2 + Load4Bytes(instr + 1);
    cmp(cpd_, dword[captures_ + kIntSize * offset]);
    je(jit_detail::kBackTrackLabel, T_NEAR);
  }

  void EmitASSERTION_SUCCESS(const uint8_t* instr, uint32_t len) {
    const uint32_t offset = code_.captures() * 2 + Load4Bytes(instr + 1);
    movsxd(sp_, dword[captures_ + kIntSize * offset]);
    LoadStack(r10);
    movsxd(cp_, dword[r10 + sp_ * kIntSize + kIntSize]);
    Jump(Load4Bytes(instr + 5));
  }

  void EmitASSERTION_FAILURE(const uint8_t* instr, uint32_t len) {
    const uint32_t offset = code_.captures() * 2 + Load4Bytes(instr + 1);
    movsxd(sp_, dword[captures_ + kIntSize * offset]);
    jmp(jit_detail::kBackTrackLabel, T_NEAR);
  }

  void InlineIsLineTerminator(const RegC& reg, const char* ok) {
    cmp(reg, core::character::code::CR);
    je(ok);
    cmp(reg, core::character::code::LF);
    je(ok);
    if (!kASCII) {
      // not ASCII => 16bit
      // (c & ~1) == 0x2028;  // 0x2028 or 0x2029
      and(Xbyak::Reg32(reg.getIdx()), 0xFFFD);
      cmp(reg, 0x2028);
      je(ok);
    }
  }

  void EmitASSERTION_BOL(const uint8_t* instr, uint32_t len) {
    inLocalLabel();
    test(cp_, cp_);
    jz(".SUCCESS");
    mov(ch10_, character[subject_ + (cp_ * kCharSize) - kCharSize]);
    InlineIsLineTerminator(ch10_, ".SUCCESS");
    jmp(jit_detail::kBackTrackLabel, T_NEAR);
    L(".SUCCESS");
    outLocalLabel();
  }

  void EmitASSERTION_BOB(const uint8_t* instr, uint32_t len) {
    test(cp_, cp_);
    jnz(jit_detail::kBackTrackLabel, T_NEAR);
  }

  void EmitASSERTION_EOL(const uint8_t* instr, uint32_t len) {
    inLocalLabel();
    cmp(cp_, size_);
    je(".SUCCESS");
    mov(ch10_, character[subject_ + (cp_ * kCharSize)]);
    InlineIsLineTerminator(ch10_, ".SUCCESS");
    jmp(jit_detail::kBackTrackLabel, T_NEAR);
    L(".SUCCESS");
    outLocalLabel();
  }

  void EmitASSERTION_EOB(const uint8_t* instr, uint32_t len) {
    cmp(cp_, size_);
    jne(jit_detail::kBackTrackLabel, T_NEAR);
  }

  void SetRegIfIsWord(const Xbyak::Reg64& out, const Xbyak::Reg64& reg) {
    inLocalLabel();
    if (!kASCII) {
      // insert ASCII check
      test(reg, static_cast<uint16_t>(65408));  // (1111111110000000)2
      jnz(".exit");
    }
    static const char tbl[128] = {
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0,
      0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1,
      0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0
    };
    xor(out, out);
    mov(out, reinterpret_cast<uintptr_t>(tbl));  // get tbl address
    movzx(out, byte[out + reg]);
    L(".exit");
    outLocalLabel();
  }

  void EmitASSERTION_WORD_BOUNDARY(const uint8_t* instr, uint32_t len) {
    inLocalLabel();

    test(cpd_, cpd_);
    jz(".FIRST_FALSE");
    cmp(cp_, size_);
    jg(".FIRST_FALSE");
    movzx(r10, character[subject_ + (cp_ * kCharSize) - kCharSize]);
    SetRegIfIsWord(rax, r10);
    jmp(".SECOND");
    L(".FIRST_FALSE");
    mov(rax, 0);

    L(".SECOND");
    cmp(cp_, size_);
    jge(".SECOND_FALSE");
    movzx(r11, character[subject_ + cp_ * kCharSize]);
    SetRegIfIsWord(rcx, r11);
    jmp(".CHECK");
    L(".SECOND_FALSE");
    mov(rcx, 0);

    L(".CHECK");
    xor(rax, rcx);
    jz(jit_detail::kBackTrackLabel, T_NEAR);
    outLocalLabel();
  }

  void EmitASSERTION_WORD_BOUNDARY_INVERTED(const uint8_t* instr, uint32_t len) {  // NOLINT
    inLocalLabel();

    test(cpd_, cpd_);
    jz(".FIRST_FALSE");
    cmp(cp_, size_);
    jg(".FIRST_FALSE");
    movzx(r10, character[subject_ + (cp_ * kCharSize) - kCharSize]);
    SetRegIfIsWord(rax, r10);
    jmp(".SECOND");
    L(".FIRST_FALSE");
    mov(rax, 0);

    L(".SECOND");
    cmp(cp_, size_);
    jge(".SECOND_FALSE");
    movzx(r11, character[subject_ + cp_ * kCharSize]);
    SetRegIfIsWord(rcx, r11);
    jmp(".CHECK");
    L(".SECOND_FALSE");
    mov(rcx, 0);

    L(".CHECK");
    xor(rax, rcx);
    jnz(jit_detail::kBackTrackLabel, T_NEAR);
    outLocalLabel();
  }

  void EmitSTART_CAPTURE(const uint8_t* instr, uint32_t len) {
    const uint32_t target = Load4Bytes(instr + 1);
    mov(dword[captures_ + kIntSize * (target * 2)], cpd_);
    mov(dword[captures_ + kIntSize * (target * 2 + 1)], kUndefined);
  }

  void EmitEND_CAPTURE(const uint8_t* instr, uint32_t len) {
    const uint32_t target = Load4Bytes(instr + 1);
    mov(dword[captures_ + kIntSize * (target * 2 + 1)], cpd_);
  }

  void EmitCLEAR_CAPTURES(const uint8_t* instr, uint32_t len) {
    const uint32_t from = Load4Bytes(instr + 1) * 2;
    const uint32_t to = Load4Bytes(instr + 5) * 2;
    if ((to - from) <= 10) {
      // loop unrolling
      if ((to - from) > 0) {
        lea(r10, ptr[captures_ + kIntSize * from]);
        uint32_t i = from;
        while (true) {
          mov(dword[r10], kUndefined);
          ++i;
          if (i == to) {
            break;
          }
          add(r10, kIntSize);
        }
      }
    } else {
      assert(to - from);  // not 0
      inLocalLabel();
      mov(r10, (to - from));
      lea(r11, ptr[captures_ + kIntSize * from]);
      L(".LOOP_START");
      mov(dword[r11], kUndefined);
      add(r11, kIntSize);
      sub(r10, 1);
      jnz(".LOOP_START");
      outLocalLabel();
    }
  }

  void EmitCOUNTER_ZERO(const uint8_t* instr, uint32_t len) {
    const uint32_t counter = code_.captures() * 2 + Load4Bytes(instr + 1);
    mov(dword[captures_ + kIntSize * counter], 0);
  }

  void EmitCOUNTER_NEXT(const uint8_t* instr, uint32_t len) {
    const int max = static_cast<int>(Load4Bytes(instr + 5));
    const uint32_t counter = code_.captures() * 2 + Load4Bytes(instr + 1);
    mov(r10d, dword[captures_ + kIntSize * counter]);
    inc(r10d);
    mov(dword[captures_ + kIntSize * counter], r10d);
    cmp(r10d, max);
    jl(MakeLabel(Load4Bytes(instr + 9)).c_str(), T_NEAR);
  }

  void EmitPUSH_BACKTRACK(const uint8_t* instr, uint32_t len) {
    const int size = code_.captures() * 2 + code_.counters() + 1;
    inLocalLabel();
    LoadVM(rdi);
    mov(rsi, sp_);
    mov(rdx, size);
    mov(rax, core::BitCast<uintptr_t>(&VM::NewStateForJIT));
    call(rax);
    test(rax, rax);
    jz(jit_detail::kErrorLabel, T_NEAR);

    add(sp_, size);
    sub(rax, kIntSize * (size));

    // copy
    mov(r10, captures_);
    mov(r11, size - 1);
    mov(rsi, rax);

    test(r11, r11);
    jz(".LOOP_END");

    L(".LOOP_START");
    mov(ecx, dword[r10]);
    add(r10, kIntSize);
    mov(dword[rax], ecx);
    add(rax, kIntSize);
    sub(r11, 1);
    jnz(".LOOP_START");
    L(".LOOP_END");
    mov(dword[rsi + kIntSize], cpd_);
    const int val = static_cast<int>(Load4Bytes(instr + 1));
    const BackTrackMap::const_iterator it = backtracks_.find(val);
    assert(it != backtracks_.end());
    // we use rsi as counter in AERO_PUSH_BACKTRACK
    mov(dword[rsi + kIntSize * (size - 1)], static_cast<uint32_t>(it->second));
    outLocalLabel();
  }

  void EmitBACK_REFERENCE(const uint8_t* instr, uint32_t len) {
    const uint16_t ref = Load2Bytes(instr + 1);
    assert(ref != 0);  // limited by parser
    if (ref >= code_.captures()) {
      return;
    }

    inLocalLabel();
    movsxd(rax, dword[captures_ + kIntSize * (ref * 2 + 1)]);
    cmp(rax, -1);
    je(".SUCCESS", T_NEAR);
    movsxd(r10, dword[captures_ + kIntSize * (ref * 2)]);
    sub(rax, r10);
    lea(rcx, ptr[rax + cp_]);

    cmp(rcx, size_);
    jg(jit_detail::kBackTrackLabel, T_NEAR);

    // back reference check
    lea(rdx, ptr[subject_ + r10 * kCharSize]);
    lea(rcx, ptr[subject_ + cp_ * kCharSize]);
    mov(r8, rax);

    test(r8, r8);
    jz(".LOOP_END");

    L(".LOOP_START");
    mov(ch10_, character[rdx]);
    cmp(ch10_, character[rcx]);
    jne(jit_detail::kBackTrackLabel, T_NEAR);
    add(rdx, kCharSize);
    add(rcx, kCharSize);
    sub(r8, 1);
    jnz(".LOOP_START");
    L(".LOOP_END");

    add(cp_, rax);

    L(".SUCCESS");
    outLocalLabel();
  }

  void EmitBACK_REFERENCE_IGNORE_CASE(const uint8_t* instr, uint32_t len) {
    const uint16_t ref = Load2Bytes(instr + 1);
    assert(ref != 0);  // limited by parser
    if (ref >= code_.captures()) {
      return;
    }

    inLocalLabel();
    movsxd(rax, dword[captures_ + kIntSize * (ref * 2 + 1)]);
    cmp(rax, -1);
    je(".SUCCESS", T_NEAR);
    movsxd(r10, dword[captures_ + kIntSize * (ref * 2)]);
    sub(rax, r10);
    lea(rcx, ptr[rax + cp_]);

    cmp(rcx, size_);
    jg(jit_detail::kBackTrackLabel, T_NEAR);

    // back reference check
    lea(rdx, ptr[subject_ + r10 * kCharSize]);
    lea(rcx, ptr[subject_ + cp_ * kCharSize]);
    mov(r8, rax);

    test(r8, r8);
    jz(".LOOP_END");

    L(".LOOP_START");
    mov(ch10_, character[rdx]);
    cmp(ch10_, character[rcx]);
    je(".COND_OK", T_NEAR);

    // used callar-save registers
    // r8 r10 r11 rdx rcx
    push(r8);
    push(r8);

    push(rdx);
    push(rcx);
    movzx(rdi, ch10_);
    mov(r10, core::BitCast<uintptr_t>(&core::character::ToUpperCase));
    call(r10);
    pop(rcx);
    cmp(eax, character[rcx]);
    je(".CALL_COND_OK");

    pop(rdx);
    movzx(rdi, character[rdx]);
    push(rdx);
    push(rcx);
    movzx(rdi, ch10_);
    mov(r10, core::BitCast<uintptr_t>(&core::character::ToLowerCase));
    call(r10);
    pop(rcx);
    cmp(eax, character[rcx]);
    je(".CALL_COND_OK");

    pop(rdx);
    pop(r8);
    pop(r8);
    jmp(jit_detail::kBackTrackLabel, T_NEAR);

    L(".CALL_COND_OK");
    pop(rdx);
    pop(r8);
    pop(r8);

    L(".COND_OK");
    add(rdx, kCharSize);
    add(rcx, kCharSize);
    sub(r8, 1);
    jnz(".LOOP_START");

    L(".LOOP_END");
    add(cp_, rax);

    L(".SUCCESS");
    outLocalLabel();
  }

  void EmitCHECK_1BYTE_CHAR(const uint8_t* instr, uint32_t len) {
    EmitSizeGuard();
    const CharT ch = Load1Bytes(instr + 1);
    cmp(character[subject_ + cp_ * kCharSize], ch);
    jne(jit_detail::kBackTrackLabel, T_NEAR);
    inc(cp_);
  }

  void EmitCHECK_2BYTE_CHAR(const uint8_t* instr, uint32_t len) {
    if (kASCII) {
      jmp(jit_detail::kBackTrackLabel, T_NEAR);
      return;
    }
    EmitSizeGuard();
    const uint16_t ch = Load2Bytes(instr + 1);
    if (ch <= 0x7FFF) {  // INT16_MAX
      cmp(character[subject_ + cp_ * kCharSize], ch);
    } else {
      movzx(r10, character[subject_ + cp_ * kCharSize]);
      cmp(r10, ch);
    }
    jne(jit_detail::kBackTrackLabel, T_NEAR);
    inc(cp_);
  }

  void EmitCHECK_2CHAR_OR(const uint8_t* instr, uint32_t len) {
    inLocalLabel();
    EmitSizeGuard();
    movzx(r10, character[subject_ + cp_ * kCharSize]);
    const uint16_t first = Load2Bytes(instr + 1);
    if (!(kASCII && !core::character::IsASCII(first))) {
      cmp(r10, first);
      je(".SUCCESS");
    }
    const uint16_t second = Load2Bytes(instr + 3);
    if (!(kASCII && !core::character::IsASCII(second))) {
      cmp(r10, second);
      je(".SUCCESS");
    }
    jmp(jit_detail::kBackTrackLabel, T_NEAR);
    L(".SUCCESS");
    inc(cp_);
    outLocalLabel();
  }

  void EmitCHECK_3CHAR_OR(const uint8_t* instr, uint32_t len) {
    inLocalLabel();
    EmitSizeGuard();
    movzx(r10, character[subject_ + cp_ * kCharSize]);
    const uint16_t first = Load2Bytes(instr + 1);
    if (!(kASCII && !core::character::IsASCII(first))) {
      cmp(r10, first);
      je(".SUCCESS");
    }
    const uint16_t second = Load2Bytes(instr + 3);
    if (!(kASCII && !core::character::IsASCII(second))) {
      cmp(r10, second);
      je(".SUCCESS");
    }
    const uint16_t third = Load2Bytes(instr + 5);
    if (!(kASCII && !core::character::IsASCII(third))) {
      cmp(r10, third);
      je(".SUCCESS");
    }
    jmp(jit_detail::kBackTrackLabel, T_NEAR);
    L(".SUCCESS");
    inc(cp_);
    outLocalLabel();
  }

  void EmitCHECK_RANGE(const uint8_t* instr, uint32_t len) {
    inLocalLabel();
    EmitSizeGuard();
    movzx(r10, character[subject_ + cp_ * kCharSize]);
    const uint32_t length = Load4Bytes(instr + 1);
    for (std::size_t i = 0; i < length; i += 4) {
      const uint16_t start = Load2Bytes(instr + 1 + 4 + i);
      const uint16_t finish = Load2Bytes(instr + 1 + 4 + i + 2);
      if (kASCII && (!core::character::IsASCII(start))) {
        jmp(jit_detail::kBackTrackLabel, T_NEAR);
        break;
      }
      cmp(r10, start);
      jl(jit_detail::kBackTrackLabel, T_NEAR);

      if (kASCII && (!core::character::IsASCII(finish))) {
        jmp(".SUCCESS", T_NEAR);
        break;
      }
      cmp(r10, finish);
      jle(".SUCCESS", T_NEAR);
    }
    jmp(jit_detail::kBackTrackLabel, T_NEAR);
    L(".SUCCESS");
    inc(cp_);
    outLocalLabel();
  }

  void EmitCHECK_RANGE_INVERTED(const uint8_t* instr, uint32_t len) {
    inLocalLabel();
    EmitSizeGuard();
    movzx(r10, character[subject_ + cp_ * kCharSize]);
    const uint32_t length = Load4Bytes(instr + 1);
    for (std::size_t i = 0; i < length; i += 4) {
      const uint16_t start = Load2Bytes(instr + 1 + 4 + i);
      const uint16_t finish = Load2Bytes(instr + 1 + 4 + i + 2);
      if (kASCII && (!core::character::IsASCII(start))) {
        jmp(".SUCCESS", T_NEAR);
        break;
      }
      cmp(r10, start);
      jl(".SUCCESS", T_NEAR);

      if (kASCII && (!core::character::IsASCII(finish))) {
        jmp(jit_detail::kBackTrackLabel, T_NEAR);
        break;
      }
      cmp(r10, finish);
      jle(jit_detail::kBackTrackLabel, T_NEAR);
    }
    L(".SUCCESS");
    inc(cp_);
    outLocalLabel();
  }

  void EmitJUMP(const uint8_t* instr, uint32_t len) {
    Jump(Load4Bytes(instr + 1));
  }

  void EmitFAILURE(const uint8_t* instr, uint32_t len) {
    jmp(jit_detail::kBackTrackLabel, T_NEAR);
  }

  void EmitSUCCESS(const uint8_t* instr, uint32_t len) {
    jmp(jit_detail::kSuccessLabel, T_NEAR);
  }

  std::string MakeLabel(uint32_t num, const std::string& prefix = "AERO_") {
    std::string str(prefix);
    core::UInt32ToString(num, std::back_inserter(str));
    return str;
  }

  void DefineLabel(uint32_t num) {
    L(MakeLabel(num).c_str());
  }

  void Jump(uint32_t num) {
    jmp(MakeLabel(num).c_str(), T_NEAR);
  }

  const Code& code_;
  const Code::Data::const_pointer first_instr_;
  core::SortedVector<uint32_t> targets_;
  BackTrackMap backtracks_;
  std::vector<uintptr_t> tracked_;

  const Xbyak::AddressFrame character;
  const Xbyak::Reg64& subject_;
  const Xbyak::Reg64& size_;
  const Xbyak::Reg64& captures_;
  const Xbyak::Reg64& cp_;
  const Xbyak::Reg32& cpd_;
  const Xbyak::Reg64& sp_;
  const Xbyak::Reg32& spd_;
  const RegC& ch10_;
  const RegC& ch11_;
};

} }  // namespace iv::aero
#endif
#endif  // IV_AERO_JIT_H_
