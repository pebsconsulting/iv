#ifndef _IV_LV5_TELEPORTER_JSSCRIPT_H_
#define _IV_LV5_TELEPORTER_JSSCRIPT_H_
#include "detail/memory.h"
#include "source_traits.h"
#include "lv5/jsscript.h"
#include "lv5/specialized_ast.h"
#include "lv5/eval_source.h"
namespace iv {
namespace lv5 {
namespace teleporter {

class Context;

class JSScript : public lv5::JSScript {
 public:
  typedef JSScript this_type;
  enum Type {
    kGlobal,
    kEval,
    kFunction
  };

  JSScript(const FunctionLiteral* function)
    : function_(function) {
  }

  virtual Type type() const = 0;

  virtual core::UStringPiece SubString(std::size_t start,
                                       std::size_t len) const = 0;

  inline const FunctionLiteral* function() const {
    return function_;
  }

 private:
  const FunctionLiteral* function_;
};

template<typename Source>
class JSEvalScript : public JSScript {
 public:
  typedef JSEvalScript<Source> this_type;
  JSEvalScript(const FunctionLiteral* function,
               AstFactory* factory,
               std::tr1::shared_ptr<Source> source)
    : JSScript(function),
      factory_(factory),
      source_(source) {
  }

  ~JSEvalScript() {
    // this container has ownership to factory
    delete factory_;
  }

  inline Type type() const {
    return kEval;
  }

  inline std::tr1::shared_ptr<Source> source() const {
    return source_;
  }

  core::UStringPiece SubString(std::size_t start,
                               std::size_t len) const {
    return source_->GetData().substr(start, len);
  }

  static this_type* New(Context* ctx,
                        const FunctionLiteral* function,
                        AstFactory* factory,
                        std::tr1::shared_ptr<Source> source) {
    return new JSEvalScript<Source>(function, factory, source);
  }

 private:
  AstFactory* factory_;
  std::tr1::shared_ptr<Source> source_;
};

class JSGlobalScript : public JSScript {
 public:
  typedef JSGlobalScript this_type;
  JSGlobalScript(const FunctionLiteral* function,
                 AstFactory* factory,
                 core::FileSource* source)
    : JSScript(function),
      source_(source) {
  }

  inline Type type() const {
    return kGlobal;
  }

  inline core::FileSource* source() const {
    return source_;
  }

  core::UStringPiece SubString(std::size_t start,
                               std::size_t len) const {
    return source_->GetData().substr(start, len);
  }

  static this_type* New(Context* ctx,
                        const FunctionLiteral* function,
                        AstFactory* factory,
                        core::FileSource* source) {
    return new JSGlobalScript(function, factory, source);
  }

 private:
  core::FileSource* source_;
};

} } }  // namespace iv::lv5::teleporter
#endif  // _IV_LV5_TELEPORTER_JSSCRIPT_H_
