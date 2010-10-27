#include <cmath>
#include "cstring"
#include "ustring.h"
#include "arguments.h"
#include "jsfunction.h"
#include "context.h"
#include "interpreter.h"
#include "property.h"
#include "class.h"
#include "runtime.h"
namespace iv {
namespace lv5 {
namespace {

const std::string length_string("length");
const std::string eval_string("eval");
const std::string arguments_string("arguments");
const std::string caller_string("caller");
const std::string callee_string("callee");
const std::string toString_string("toString");
const std::string valueOf_string("valueOf");
const std::string prototype_string("prototype");

}  // namespace

Context::Context()
  : global_obj_(),
    throw_type_error_(&Runtime_ThrowTypeError),
    lexical_env_(NULL),
    variable_env_(NULL),
    binding_(&global_obj_),
    table_(),
    interp_(),
    mode_(NORMAL),
    ret_(),
    target_(NULL),
    error_(),
    builtins_(),
    strict_(false),
    random_engine_(random_engine_type(),
                   random_distribution_type(0, 1)),
    length_symbol_(Intern(length_string)),
    eval_symbol_(Intern(eval_string)),
    arguments_symbol_(Intern(arguments_string)),
    caller_symbol_(Intern(caller_string)),
    callee_symbol_(Intern(callee_string)),
    toString_symbol_(Intern(toString_string)),
    valueOf_symbol_(Intern(valueOf_string)),
    prototype_symbol_(Intern(prototype_string)) {
  JSEnv* env = Interpreter::NewObjectEnvironment(this, &global_obj_, NULL);
  lexical_env_ = env;
  variable_env_ = env;
  interp_.set_context(this);
  // discard random
  for (std::size_t i = 0; i < 20; ++i) {
    Random();
  }
  Initialize();
}

Symbol Context::Intern(const core::StringPiece& str) {
  return table_.Lookup(str);
}

Symbol Context::Intern(const core::UStringPiece& str) {
  return table_.Lookup(str);
}

double Context::Random() {
  return random_engine_();
}

JSString* Context::ToString(Symbol sym) {
  return table_.ToString(this, sym);
}

const core::UString& Context::GetContent(Symbol sym) const {
  return table_.GetContent(sym);
}

bool Context::InCurrentLabelSet(
    const core::AnonymousBreakableStatement* stmt) const {
  // AnonymousBreakableStatement has empty label at first
  return !target_ || stmt == target_;
}

bool Context::InCurrentLabelSet(
    const core::NamedOnlyBreakableStatement* stmt) const {
  return stmt == target_;
}

void Context::Run(core::FunctionLiteral* global) {
  interp_.Run(global);
}

void Context::Initialize() {
  // Object and Function
  JSObject* const obj_proto = JSObject::NewPlain(this);

  // Function
  JSObject* const func_proto = JSObject::NewPlain(this);
  func_proto->set_prototype(obj_proto);
  struct Class func_cls = {
    JSString::NewAsciiString(this, "Function"),
    NULL,
    func_proto
  };
  func_proto->set_cls(func_cls.name);
  const Symbol func_name = Intern("Function");
  builtins_[func_name] = func_cls;

  JSNativeFunction* const obj_constructor =
      JSNativeFunction::New(this, &Runtime_ObjectConstructor);

  struct Class obj_cls = {
    JSString::NewAsciiString(this, "Object"),
    obj_constructor,
    obj_proto
  };
  obj_proto->set_cls(obj_cls.name);
  const Symbol obj_name = Intern("Object");
  builtins_[obj_name] = obj_cls;

  {
    JSNativeFunction* const func =
        JSNativeFunction::New(this, &Runtime_FunctionToString);
    func_proto->DefineOwnProperty(
        this, toString_symbol_,
        DataDescriptor(func,
                       PropertyDescriptor::WRITABLE),
        false, NULL);
  }

  {
    // Object Define
    {
      JSNativeFunction* const func =
          JSNativeFunction::New(this, &Runtime_ObjectHasOwnProperty);
      obj_proto->DefineOwnProperty(
          this, Intern("hasOwnProperty"),
          DataDescriptor(func,
                         PropertyDescriptor::WRITABLE),
          false, NULL);
    }
    {
      JSNativeFunction* const func =
          JSNativeFunction::New(this, &Runtime_ObjectToString);
      obj_proto->DefineOwnProperty(
          this, toString_symbol_,
          DataDescriptor(func,
                         PropertyDescriptor::WRITABLE),
          false, NULL);
    }

    obj_constructor->DefineOwnProperty(
        this, prototype_symbol_,
        DataDescriptor(obj_proto, PropertyDescriptor::NONE),
        false, NULL);

    variable_env_->CreateMutableBinding(this, obj_name, false);
    variable_env_->SetMutableBinding(this, obj_name,
                                     obj_constructor, strict_, NULL);
  }

  {
    // Array
    JSObject* const proto = JSObject::NewPlain(this);
    proto->set_prototype(obj_proto);
    struct Class cls = {
      JSString::NewAsciiString(this, "Array"),
      NULL,
      proto
    };
    proto->set_cls(cls.name);
    const Symbol name = Intern("Array");
    builtins_[name] = cls;
  }

  {
    // Error
    JSObject* const proto = JSObject::NewPlain(this);
    proto->set_prototype(obj_proto);
    struct Class cls = {
      JSString::NewAsciiString(this, "Error"),
      NULL,
      proto
    };
    proto->set_cls(cls.name);
    const Symbol name = Intern("Error");
    builtins_[name] = cls;
    {
      // section 15.11.4.4 Error.prototype.toString()
      JSNativeFunction* const func =
          JSNativeFunction::New(this, &Runtime_ErrorToString);
      proto->DefineOwnProperty(
          this, toString_symbol_,
          DataDescriptor(func,
                         PropertyDescriptor::WRITABLE),
          false, NULL);
    }

    // section 15.11.4.2 Error.prototype.name
    proto->DefineOwnProperty(
        this, Intern("name"),
        DataDescriptor(
            JSString::NewAsciiString(this, "Error"), PropertyDescriptor::NONE),
        false, NULL);

    // section 15.11.4.3 Error.prototype.message
    proto->DefineOwnProperty(
        this, Intern("message"),
        DataDescriptor(
            JSString::NewAsciiString(this, ""), PropertyDescriptor::NONE),
        false, NULL);
  }

  {
    // section 15.8 Math
    JSObject* const math = JSObject::NewPlain(this);
    math->set_prototype(obj_proto);
    math->set_cls(JSString::NewAsciiString(this, "Math"));
    global_obj_.DefineOwnProperty(
        this, Intern("Math"),
        DataDescriptor(math, PropertyDescriptor::WRITABLE),
        false, NULL);

    // section 15.8.1.1 E
    math->DefineOwnProperty(
        this, Intern("E"),
        DataDescriptor(std::exp(1.0), PropertyDescriptor::NONE),
        false, NULL);

    // section 15.8.1.2 LN10
    math->DefineOwnProperty(
        this, Intern("LN10"),
        DataDescriptor(std::exp(10.0), PropertyDescriptor::NONE),
        false, NULL);

    // section 15.8.1.3 LN2
    math->DefineOwnProperty(
        this, Intern("LN2"),
        DataDescriptor(std::exp(2.0), PropertyDescriptor::NONE),
        false, NULL);

    // section 15.8.1.4 LOG2E
    math->DefineOwnProperty(
        this, Intern("LOG2E"),
        DataDescriptor(1.0 / std::log(2.0), PropertyDescriptor::NONE),
        false, NULL);

    // section 15.8.1.5 LOG10E
    math->DefineOwnProperty(
        this, Intern("LOG10E"),
        DataDescriptor(1.0 / std::log(10.0), PropertyDescriptor::NONE),
        false, NULL);

    // section 15.8.1.6 PI
    math->DefineOwnProperty(
        this, Intern("PI"),
        DataDescriptor(std::acos(-1.0), PropertyDescriptor::NONE),
        false, NULL);

    // section 15.8.1.7 SQRT1_2
    math->DefineOwnProperty(
        this, Intern("SQRT1_2"),
        DataDescriptor(std::sqrt(0.5), PropertyDescriptor::NONE),
        false, NULL);

    // section 15.8.1.8 SQRT2
    math->DefineOwnProperty(
        this, Intern("SQRT2"),
        DataDescriptor(std::sqrt(2.0), PropertyDescriptor::NONE),
        false, NULL);

    // section 15.8.2.1 abs(x)
    math->DefineOwnProperty(
        this, Intern("abs"),
        DataDescriptor(JSNativeFunction::New(this, &Runtime_MathAbs),
                       PropertyDescriptor::WRITABLE),
        false, NULL);

    // section 15.8.2.2 acos(x)
    math->DefineOwnProperty(
        this, Intern("acos"),
        DataDescriptor(JSNativeFunction::New(this, &Runtime_MathAcos),
                       PropertyDescriptor::WRITABLE),
        false, NULL);

    // section 15.8.2.3 asin(x)
    math->DefineOwnProperty(
        this, Intern("acos"),
        DataDescriptor(JSNativeFunction::New(this, &Runtime_MathAsin),
                       PropertyDescriptor::WRITABLE),
        false, NULL);

    // section 15.8.2.4 atan(x)
    math->DefineOwnProperty(
        this, Intern("atan"),
        DataDescriptor(JSNativeFunction::New(this, &Runtime_MathAtan),
                       PropertyDescriptor::WRITABLE),
        false, NULL);

    // section 15.8.2.5 atan2(y, x)
    math->DefineOwnProperty(
        this, Intern("atan2"),
        DataDescriptor(JSNativeFunction::New(this, &Runtime_MathAtan2),
                       PropertyDescriptor::WRITABLE),
        false, NULL);

    // section 15.8.2.6 ceil(x)
    math->DefineOwnProperty(
        this, Intern("ceil"),
        DataDescriptor(JSNativeFunction::New(this, &Runtime_MathCeil),
                       PropertyDescriptor::WRITABLE),
        false, NULL);

    // section 15.8.2.7 cos(x)
    math->DefineOwnProperty(
        this, Intern("cos"),
        DataDescriptor(JSNativeFunction::New(this, &Runtime_MathCos),
                       PropertyDescriptor::WRITABLE),
        false, NULL);

    // section 15.8.2.8 exp(x)
    math->DefineOwnProperty(
        this, Intern("exp"),
        DataDescriptor(JSNativeFunction::New(this, &Runtime_MathExp),
                       PropertyDescriptor::WRITABLE),
        false, NULL);

    // section 15.8.2.9 floor(x)
    math->DefineOwnProperty(
        this, Intern("floor"),
        DataDescriptor(JSNativeFunction::New(this, &Runtime_MathFloor),
                       PropertyDescriptor::WRITABLE),
        false, NULL);

    // section 15.8.2.10 log(x)
    math->DefineOwnProperty(
        this, Intern("log"),
        DataDescriptor(JSNativeFunction::New(this, &Runtime_MathLog),
                       PropertyDescriptor::WRITABLE),
        false, NULL);

    // section 15.8.2.11 max([value1[, value2[, ... ]]])
    math->DefineOwnProperty(
        this, Intern("max"),
        DataDescriptor(JSNativeFunction::New(this, &Runtime_MathMax),
                       PropertyDescriptor::WRITABLE),
        false, NULL);

    // section 15.8.2.12 min([value1[, value2[, ... ]]])
    math->DefineOwnProperty(
        this, Intern("min"),
        DataDescriptor(JSNativeFunction::New(this, &Runtime_MathMin),
                       PropertyDescriptor::WRITABLE),
        false, NULL);

    // section 15.8.2.13 pow(x, y)
    math->DefineOwnProperty(
        this, Intern("pow"),
        DataDescriptor(JSNativeFunction::New(this, &Runtime_MathPow),
                       PropertyDescriptor::WRITABLE),
        false, NULL);

    // section 15.8.2.14 random()
    math->DefineOwnProperty(
        this, Intern("random"),
        DataDescriptor(JSNativeFunction::New(this, &Runtime_MathRandom),
                       PropertyDescriptor::WRITABLE),
        false, NULL);

    // section 15.8.2.15 round(x)
    math->DefineOwnProperty(
        this, Intern("round"),
        DataDescriptor(JSNativeFunction::New(this, &Runtime_MathRound),
                       PropertyDescriptor::WRITABLE),
        false, NULL);

    // section 15.8.2.16 sin(x)
    math->DefineOwnProperty(
        this, Intern("sin"),
        DataDescriptor(JSNativeFunction::New(this, &Runtime_MathSin),
                       PropertyDescriptor::WRITABLE),
        false, NULL);

    // section 15.8.2.17 sqrt(x)
    math->DefineOwnProperty(
        this, Intern("sqrt"),
        DataDescriptor(JSNativeFunction::New(this, &Runtime_MathSqrt),
                       PropertyDescriptor::WRITABLE),
        false, NULL);

    // section 15.8.2.18 tan(x)
    math->DefineOwnProperty(
        this, Intern("tan"),
        DataDescriptor(JSNativeFunction::New(this, &Runtime_MathTan),
                       PropertyDescriptor::WRITABLE),
        false, NULL);
  }

  {
    // Builtins
    // section 15.1.1.1 NaN
    global_obj_.DefineOwnProperty(
        this, Intern("NaN"),
        DataDescriptor(
            std::numeric_limits<double>::quiet_NaN(), PropertyDescriptor::NONE),
        false, NULL);
    // section 15.1.1.2 Infinity
    global_obj_.DefineOwnProperty(
        this, Intern("Infinity"),
        DataDescriptor(
            std::numeric_limits<double>::infinity(), PropertyDescriptor::NONE),
        false, NULL);
    // section 15.1.1.3 undefined
    global_obj_.DefineOwnProperty(
        this, Intern("undefined"),
        DataDescriptor(JSUndefined, PropertyDescriptor::NONE),
        false, NULL);
    global_obj_.set_cls(JSString::NewAsciiString(this, "global"));
    global_obj_.set_prototype(obj_proto);
  }

  {
    // Arguments
    struct Class cls = {
      JSString::NewAsciiString(this, "Arguments"),
      NULL,
      obj_proto
    };
    const Symbol name = Intern("Arguments");
    builtins_[name] = cls;
  }
  JSFunction::SetClass(this, &throw_type_error_);
}

const Class& Context::Cls(Symbol name) {
  assert(builtins_.find(name) != builtins_.end());
  return builtins_[name];
}

const Class& Context::Cls(const core::StringPiece& str) {
  assert(builtins_.find(Intern(str)) != builtins_.end());
  return builtins_[Intern(str)];
}

} }  // namespace iv::lv5
