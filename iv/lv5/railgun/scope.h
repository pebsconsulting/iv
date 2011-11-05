#ifndef IV_LV5_RAILGUN_SCOPE_H_
#define IV_LV5_RAILGUN_SCOPE_H_
#include <algorithm>
#include <iv/detail/memory.h>
#include <iv/detail/unordered_map.h>
#include <iv/lv5/specialized_ast.h>
#include <iv/lv5/symbol.h>
#include <iv/lv5/context_utils.h>
#include <iv/lv5/railgun/fwd.h>
#include <iv/lv5/railgun/code.h>
#include <iv/lv5/railgun/direct_threading.h>
namespace iv {
namespace lv5 {
namespace railgun {

class VariableScope : private core::Noncopyable<VariableScope> {
 public:
  enum Type {
    STACK = 0,
    HEAP,
    GLOBAL,
    LOOKUP
  };

  virtual ~VariableScope() { }

  std::shared_ptr<VariableScope> upper() const {
    return upper_;
  }

  void Lookup(Symbol sym, std::size_t target, Code* from) {
    // first, so this is stack
    LookupImpl(sym, target, (InWith())? LOOKUP : STACK, from);
  }

  virtual bool InWith() const {
    return false;
  }

  static Type TypeUpgrade(Type now, Type next) {
    return std::max<Type>(now, next);
  }

  virtual void LookupImpl(Symbol sym,
                          std::size_t target, Type type, Code* from) = 0;

  virtual void MakeUpperOfEval() { }

  void RecordEval() {
    MakeUpperOfEval();
    VariableScope* scope = GetRawUpper();
    while (scope) {
      scope->MakeUpperOfEval();
      scope = scope->GetRawUpper();
    }
  }

  uint32_t scope_nest_count() const {
    return scope_nest_count_;
  }

 protected:
  explicit VariableScope(std::shared_ptr<VariableScope> upper)
    : upper_(upper),
      scope_nest_count_(upper ? (upper->scope_nest_count() + 1) : 0) {
  }

  VariableScope* GetRawUpper() const {
    return upper_.get();
  }

  std::shared_ptr<VariableScope> upper_;
  uint32_t scope_nest_count_;
};

class WithScope : public VariableScope {
 public:
  explicit WithScope(std::shared_ptr<VariableScope> upper)
    : VariableScope(upper) {
    assert(upper);
  }

  bool InWith() const {
    return true;
  }

  void LookupImpl(Symbol sym,
                  std::size_t target, Type type, Code* from) {
    upper_->LookupImpl(sym, target, TypeUpgrade(type, LOOKUP), from);
  }
};

class CatchScope : public VariableScope {
 public:
  CatchScope(std::shared_ptr<VariableScope> upper, Symbol sym)
    : VariableScope(upper),
      target_(sym) {
    assert(upper);
  }

  void LookupImpl(Symbol sym,
                  std::size_t target, Type type, Code* from) {
    if (sym != target_) {
      upper_->LookupImpl(sym, target, TypeUpgrade(type, HEAP), from);
    }
  }

 private:
  Symbol target_;
};

class FunctionScope : public VariableScope {
 public:

  // Load sites vector (variable load sites)
  // symbol, point, type, from
  typedef std::vector<std::tuple<Symbol, std::size_t, Type, Code*> > Sites;
  // type, refcount, immutable
  typedef std::tuple<Type, std::size_t, bool> Variable;
  typedef std::unordered_map<Symbol, Variable> Variables;
  // Symbol -> used, location
  typedef std::unordered_map<Symbol, std::tuple<bool, uint32_t> > Locations;
  typedef std::unordered_map<Symbol, uint32_t> HeapOffsetMap;

  // this is dummy global environment constructor
  FunctionScope(std::shared_ptr<VariableScope> upper, Code::Data* data)
    : VariableScope(upper),
      map_(),
      sites_(),
      code_(NULL),
      data_(data),
      upper_of_eval_(false),
      eval_top_scope_(false),
      dynamic_target_scope_(false) {
  }

  FunctionScope(std::shared_ptr<VariableScope> upper,
                Context* ctx,
                Code* code,
                Code::Data* data,
                const Scope& scope)
    : VariableScope(upper),
      map_(),
      sites_(),
      code_(code),
      data_(data),
      upper_of_eval_(false),
      eval_top_scope_(code->code_type() == Code::EVAL),
      dynamic_target_scope_(scope.HasDirectCallToEval() && !code->strict()) {
    if (!IsTop()) {
      // is global or not
      map_[symbol::arguments()] = std::make_tuple(STACK, 0, code_->strict());

      // params
      for (Code::Names::const_iterator it = code->params().begin(),
           last = code->params().end(); it != last; ++it) {
        map_[*it] = std::make_tuple(STACK, 0, false);
      }

      // function declarations
      typedef Scope::FunctionLiterals Functions;
      const Functions& functions = scope.function_declarations();
      for (Functions::const_iterator it = functions.begin(),
           last = functions.end(); it != last; ++it) {
        const Symbol sym = (*it)->name().Address()->symbol();
        map_[sym] = std::make_tuple(STACK, 0, false);
      }

      // variables
      typedef Scope::Variables Variables;
      const Variables& vars = scope.variables();
      for (Variables::const_iterator it = vars.begin(),
           last = vars.end(); it != last; ++it) {
        const Symbol name = it->first->symbol();
        if (map_.find(name) != map_.end()) {
          map_[name] = std::make_tuple(
              TypeUpgrade(std::get<0>(map_[name]), STACK), 0, false);
        } else {
          map_[name] = std::make_tuple(STACK, 0, false);
        }
      }

      const FunctionLiteral::DeclType type = code->decl_type();
      if (type == FunctionLiteral::STATEMENT ||
          (type == FunctionLiteral::EXPRESSION && code_->HasName())) {
        const Symbol& name = code_->name();
        if (map_.find(name) == map_.end()) {
          map_[name] = std::make_tuple(STACK, 0, true);
        }
      }
    }
  }

  ~FunctionScope() {
    if (IsTop()) {
      // if direct call to eval top scope
      // all variable is {configurable : true}, so not STACK
      if (!eval_top_scope_) {
        for (Sites::const_iterator it = sites_.begin(),
             last = sites_.end(); it != last; ++it) {
          const Symbol sym = std::get<0>(*it);
          const std::size_t point = std::get<1>(*it);
          const Type type = TypeUpgrade(std::get<0>(map_[sym]),
                                        std::get<2>(*it));
          if (type == GLOBAL) {
            // emit global opt
            const uint32_t op = (*data_)[point].value;
            (*data_)[point] = OP::ToGlobal(op);
          }
        }
      }
    } else {
      assert(code_);
      Locations locations;
      uint32_t location = 0;

      // arguments heap conversion
      if (code_->IsShouldCreateHeapArguments()) {
        for (Code::Names::const_iterator it = code_->params().begin(),
             last = code_->params().end(); it != last; ++it) {
          std::get<0>(map_[*it]) = TypeUpgrade(std::get<0>(map_[*it]), HEAP);
        }
      }

      if (!upper_of_eval_) {
        // stack variable calculation
        for (Variables::const_iterator it = map_.begin(),
             last = map_.end(); it != last; ++it) {
          if (std::get<0>(it->second) == STACK) {
            if (std::get<1>(it->second) == 0) {  // ref count is 0
              locations.insert(
                  std::make_pair(it->first, std::make_tuple(false, 0u)));
            } else {
              locations.insert(
                  std::make_pair(it->first, std::make_tuple(true, location++)));
              code_->locals_.push_back(it->first);
            }
          }
        }
        code_->set_stack_depth(code_->stack_depth() + code_->locals().size());
      }

      code_->set_scope_nest_count(scope_nest_count());
      HeapOffsetMap offsets;
      InsertDecls(code_, locations, &offsets);

      // opcode optimization
      if (upper_of_eval_) {
        // only HEAP analyzer is allowed
        for (Sites::const_iterator it = sites_.begin(),
             last = sites_.end(); it != last; ++it) {
          const Symbol sym = std::get<0>(*it);
          const std::size_t point = std::get<1>(*it);
          const Type type = TypeUpgrade(std::get<0>(map_[sym]),
                                        std::get<2>(*it));
          if (type == HEAP) {  // not LOOKUP
            // emit heap opt
            const uint32_t op = (*data_)[point].value;
            (*data_)[point] = OP::ToHeap(op);
            (*data_)[point + 2] = scope_nest_count();
            (*data_)[point + 3] = offsets[sym];
          }
        }
      } else {
        for (Sites::const_iterator it = sites_.begin(),
             last = sites_.end(); it != last; ++it) {
          const Symbol sym = std::get<0>(*it);
          const std::size_t point = std::get<1>(*it);
          const Type type = TypeUpgrade(std::get<0>(map_[sym]),
                                        std::get<2>(*it));
          if (type == STACK) {
            const bool immutable = std::get<2>(map_[sym]);
            Code* from = std::get<3>(*it);
            const uint32_t op = (*data_)[point].value;
            (*data_)[point] = (immutable) ?
                OP::ToLocalImmutable(op, from->strict()) : OP::ToLocal(op);
            assert(locations.find(sym) != locations.end());
            assert(std::get<0>(locations[sym]));
            const uint32_t loc = std::get<1>(locations[sym]);
            (*data_)[point + 1] = loc;
          } else if (type == GLOBAL) {
            // emit global opt
            const uint32_t op = (*data_)[point].value;
            (*data_)[point] = OP::ToGlobal(op);
          } else if (type == HEAP) {  // Not Lookup
            // emit heap opt
            const uint32_t op = (*data_)[point].value;
            (*data_)[point] = OP::ToHeap(op);
            (*data_)[point + 2] = scope_nest_count();
            (*data_)[point + 3] = offsets[sym];
          }
        }
      }
    }
  }

  void LookupImpl(Symbol sym, std::size_t target, Type type, Code* from) {
    if (map_.find(sym) == map_.end()) {
      if (IsTop()) {
        // this is global
        map_[sym] =
            std::make_tuple((eval_top_scope_) ? LOOKUP : GLOBAL, 1, false);
        sites_.push_back(
            std::make_tuple(
                sym,
                target,
                TypeUpgrade((eval_top_scope_) ? LOOKUP : GLOBAL, type),
                from));
      } else {
        upper_->LookupImpl(
            sym,
            target,
            TypeUpgrade(type, (IsDynamicTargetScope()) ? LOOKUP : HEAP),
            from);
      }
    } else {
      if (IsTop()) {
        ++std::get<1>(map_[sym]);
        sites_.push_back(
            std::make_tuple(
                sym,
                target,
                TypeUpgrade((eval_top_scope_) ? LOOKUP : GLOBAL, type),
                from));
      } else {
        const Type stored = (type == LOOKUP || type == GLOBAL) ? HEAP : type;
        assert(stored == HEAP || stored == STACK);
        std::get<0>(map_[sym]) = TypeUpgrade(std::get<0>(map_[sym]), stored);
        ++std::get<1>(map_[sym]);
        sites_.push_back(std::make_tuple(sym, target, type, from));
      }
    }
  }

  virtual void MakeUpperOfEval() {
    upper_of_eval_ = true;
  }

  bool IsDynamicTargetScope() const {
    return dynamic_target_scope_;
  }

  bool IsTop() const {
    return !upper_;
  }

 private:
  class SearchDecl {
   public:
    explicit SearchDecl(Symbol sym) : sym_(sym) { }
    template<typename DeclTuple>
    bool operator()(const DeclTuple& decl) const {
      return std::get<0>(decl) == sym_;
    }
   private:
    Symbol sym_;
  };

  void InsertDecls(Code* code,
                   const Locations& locations,
                   HeapOffsetMap* offsets) {
    bool needs_env = false;
    std::unordered_set<Symbol> already_decled;
    {
      std::size_t param_count = 0;
      typedef std::unordered_map<Symbol, std::size_t> Symbol2Count;
      Symbol2Count sym2c;
      for (Code::Names::const_iterator it = code->params().begin(),
           last = code->params().end(); it != last; ++it, ++param_count) {
        sym2c[*it] = param_count;
      }
      for (Symbol2Count::const_iterator it = sym2c.begin(),
           last = sym2c.end(); it != last; ++it) {
        const Symbol sym = it->first;
        const Locations::const_iterator f = locations.find(sym);
        if (f == locations.end()) {
          needs_env = true;
          const uint32_t offset = offsets->size();
          offsets->insert(std::make_pair(sym, offset));
          code->decls_.push_back(
              std::make_tuple(
                  sym,
                  Code::PARAM,
                  it->second,
                  offset));
        } else {
          // PARAM on STACK
          if (std::get<0>(f->second)) {  // used
            code->decls_.push_back(
                std::make_tuple(
                    sym,
                    Code::PARAM_LOCAL,
                    it->second,
                    std::get<1>(f->second)));
          }
        }
        already_decled.insert(sym);
      }
    }

    for (Code::Codes::const_iterator it = code->codes().begin(),
         last = code->codes().end(); it != last; ++it) {
      if ((*it)->IsFunctionDeclaration()) {
        const Symbol& fn = (*it)->name();
        if (locations.find(fn) == locations.end() &&
            already_decled.find(fn) == already_decled.end()) {
          needs_env = true;
          const uint32_t offset = offsets->size();
          offsets->insert(std::make_pair(fn, offset));
          code->decls_.push_back(
              std::make_tuple(fn, Code::FDECL, 0, offset));
          already_decled.insert(fn);
        }
      }
    }

    if (code->IsShouldCreateArguments()) {
      const Locations::const_iterator f = locations.find(symbol::arguments());
      if (f == locations.end()) {
        needs_env = true;
        const uint32_t offset = offsets->size();
        offsets->insert(std::make_pair(symbol::arguments(), offset));
        code->decls_.push_back(
            std::make_tuple(symbol::arguments(), Code::ARGUMENTS, 0, offset));
      } else {
        assert(std::get<0>(f->second));  // used
        code->decls_.push_back(
            std::make_tuple(symbol::arguments(),
                            Code::ARGUMENTS_LOCAL,
                            0,
                            std::get<1>(f->second)));
      }
      already_decled.insert(symbol::arguments());
    }

    for (Code::Names::const_iterator it = code->varnames().begin(),
         last = code->varnames().end(); it != last; ++it) {
      const Symbol& dn = *it;
      if (locations.find(dn) == locations.end() &&
          already_decled.find(dn) == already_decled.end()) {
        needs_env = true;
        const uint32_t offset = offsets->size();
        offsets->insert(std::make_pair(dn, offset));
        code->decls_.push_back(std::make_tuple(dn, Code::VAR, 0, offset));
        already_decled.insert(dn);
      }
    }

    const FunctionLiteral::DeclType type = code->decl_type();
    if (type == FunctionLiteral::STATEMENT ||
        (type == FunctionLiteral::EXPRESSION && code->HasName())) {
      const Symbol& fn = code->name();
      if (already_decled.find(fn) == already_decled.end()) {
        const Locations::const_iterator f = locations.find(fn);
        if (f == locations.end()) {
          needs_env = true;
          const uint32_t offset = offsets->size();
          offsets->insert(std::make_pair(fn, offset));
          code->decls_.push_back(std::make_tuple(fn, Code::FEXPR, 0, offset));
        } else {
          if (std::get<0>(f->second)) {  // used
            code->decls_.push_back(
                std::make_tuple(fn,
                                Code::FEXPR_LOCAL,
                                0,
                                std::get<1>(f->second)));
          }
        }
        already_decled.insert(fn);
      }
    }
    if (!needs_env) {
      code->set_has_declarative_env(false);
    } else {
      code->set_reserved_record_size(offsets->size());
    }
  }

  Variables map_;
  Sites sites_;
  Code* code_;
  Code::Data* data_;
  bool upper_of_eval_;
  bool eval_top_scope_;
  bool dynamic_target_scope_;
};

} } }  // namespace iv::lv5::railgun
#endif  // IV_LV5_RAILGUN_SCOPE_H_