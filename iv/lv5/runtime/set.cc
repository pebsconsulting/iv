#include <iv/lv5/error_check.h>
#include <iv/lv5/constructor_check.h>
#include <iv/lv5/arguments.h>
#include <iv/lv5/jsval.h>
#include <iv/lv5/context.h>
#include <iv/lv5/jsobject.h>
#include <iv/lv5/error.h>
#include <iv/lv5/jsset.h>
#include <iv/lv5/jsset_iterator.h>
#include <iv/lv5/jsiterator_result.h>
#include <iv/lv5/runtime/set.h>
namespace iv {
namespace lv5 {
namespace runtime {

// section 15.16.2.1 Set(iterable = undefined)
// section 15.16.3.1 new Set (iterable = undefined)
JSVal SetConstructor(const Arguments& args, Error* e) {
  Context* ctx = args.ctx();
  const JSVal first = args.At(0);
  const JSVal this_binding = args.this_binding();
  JSObject* set = nullptr;
  if (this_binding.IsUndefined() ||
      JSVal::StrictEqual(this_binding, ctx->global_data()->set_prototype())) {
    set = JSObject::New(
        ctx,
        ctx->global_data()->set_map());
  } else {
    set = this_binding.ToObject(ctx, IV_LV5_ERROR(e));
  }
  return JSSet::Initialize(ctx, set, first, IV_LV5_ERROR(e));
}

// 23.2.3.1 Set.prototype.add(value)
JSVal SetAdd(const Arguments& args, Error* e) {
  IV_LV5_CONSTRUCTOR_CHECK("Set.prototype.add", args, e);
  Context* ctx = args.ctx();
  JSObject* set = args.this_binding().ToObject(ctx, IV_LV5_ERROR(e));
  Slot slot;
  if (!set->GetOwnPropertySlot(ctx, JSSet::symbol(), &slot)) {
    e->Report(Error::Type, "Set.prototype.add is not generic function");
    return JSEmpty;
  }
  JSSet::Data* entries = static_cast<JSSet::Data*>(slot.value().cell());
  entries->Add(args.At(0));
  return JSUndefined;
}

// 23.2.3.2 Set.prototype.clear()
JSVal SetClear(const Arguments& args, Error* e) {
  IV_LV5_CONSTRUCTOR_CHECK("Set.prototype.clear", args, e);
  Context* ctx = args.ctx();
  JSObject* set = args.this_binding().ToObject(ctx, IV_LV5_ERROR(e));
  Slot slot;
  if (!set->GetOwnPropertySlot(ctx, JSSet::symbol(), &slot)) {
    e->Report(Error::Type, "Set.prototype.clear is not generic function");
    return JSEmpty;
  }
  JSSet::Data* entries = static_cast<JSSet::Data*>(slot.value().cell());
  entries->Clear();
  return JSUndefined;
}

// 23.2.3.4 Set.prototype.delete(value)
JSVal SetDelete(const Arguments& args, Error* e) {
  IV_LV5_CONSTRUCTOR_CHECK("Set.prototype.delete", args, e);
  Context* ctx = args.ctx();
  JSObject* set = args.this_binding().ToObject(ctx, IV_LV5_ERROR(e));
  Slot slot;
  if (!set->GetOwnPropertySlot(ctx, JSSet::symbol(), &slot)) {
    e->Report(Error::Type, "Set.prototype.delete is not generic function");
    return JSEmpty;
  }
  JSSet::Data* entries = static_cast<JSSet::Data*>(slot.value().cell());
  return JSVal::Bool(entries->Delete(args.At(0)));
}

// 23.2.3.5 Set.prototype.entries()
JSVal SetEntries(const Arguments& args, Error* e) {
  IV_LV5_CONSTRUCTOR_CHECK("Set.prototype.entries", args, e);
  Context* ctx = args.ctx();
  JSObject* set = args.this_binding().ToObject(ctx, IV_LV5_ERROR(e));
  Slot slot;
  if (!set->GetOwnPropertySlot(ctx, JSSet::symbol(), &slot)) {
    e->Report(Error::Type, "Set.prototype.entries is not generic function");
    return JSEmpty;
  }
  JSSet::Data* entries = static_cast<JSSet::Data*>(slot.value().cell());
  return JSSetIterator::New(ctx, entries, SetIterationKind::KEY_PLUS_VALUE);
}

// 23.2.3.6 Set.prototype.forEach(callbackfn, thisArg = undefined)
JSVal SetForEach(const Arguments& args, Error* e) {
  IV_LV5_CONSTRUCTOR_CHECK("Set.prototype.forEach", args, e);
  Context* ctx = args.ctx();
  JSObject* set = args.this_binding().ToObject(ctx, IV_LV5_ERROR(e));
  Slot slot;
  if (!set->GetOwnPropertySlot(ctx, JSSet::symbol(), &slot)) {
    e->Report(Error::Type, "Set.prototype.forEach is not generic function");
    return JSEmpty;
  }
  const JSVal callbackfn = args.At(0);
  if (!callbackfn.IsCallable()) {
    e->Report(Error::Type, "Set.prototype.forEach callbackfn is not callable");
    return JSEmpty;
  }
  JSFunction* callback = static_cast<JSFunction*>(callbackfn.object());
  JSVal this_arg = args.At(1);
  JSSet::Data* entries = static_cast<JSSet::Data*>(slot.value().cell());
  for (JSSet::Data::Set::const_iterator it = entries->set().begin(),
       last = entries->set().end(); it != last; ++it) {
    ScopedArguments arg_list(ctx, 3, IV_LV5_ERROR(e));
    arg_list[0] = *it;
    arg_list[2] = set;
    callback->Call(&arg_list, this_arg, IV_LV5_ERROR(e));
  }
  return JSUndefined;
}

// 23.2.3.7 Set.prototype.has(value)
JSVal SetHas(const Arguments& args, Error* e) {
  IV_LV5_CONSTRUCTOR_CHECK("Set.prototype.has", args, e);
  Context* ctx = args.ctx();
  JSObject* set = args.this_binding().ToObject(ctx, IV_LV5_ERROR(e));
  Slot slot;
  if (!set->GetOwnPropertySlot(ctx, JSSet::symbol(), &slot)) {
    e->Report(Error::Type, "Set.prototype.has is not generic function");
    return JSEmpty;
  }
  JSSet::Data* entries = static_cast<JSSet::Data*>(slot.value().cell());
  return JSVal::Bool(entries->Has(args.At(0)));
}

// 23.2.3.8 Set.prototype.keys()
JSVal SetKeys(const Arguments& args, Error* e) {
  IV_LV5_CONSTRUCTOR_CHECK("Set.prototype.keys", args, e);
  Context* ctx = args.ctx();
  JSObject* set = args.this_binding().ToObject(ctx, IV_LV5_ERROR(e));
  Slot slot;
  if (!set->GetOwnPropertySlot(ctx, JSSet::symbol(), &slot)) {
    e->Report(Error::Type, "Set.prototype.keys is not generic function");
    return JSEmpty;
  }
  JSSet::Data* entries = static_cast<JSSet::Data*>(slot.value().cell());
  return JSSetIterator::New(ctx, entries, SetIterationKind::KEY);
}

// 23.2.3.9 get Set.prototype.size
JSVal SetSize(const Arguments& args, Error* e) {
  IV_LV5_CONSTRUCTOR_CHECK("Set.prototype.size", args, e);
  Context* ctx = args.ctx();
  JSObject* set = args.this_binding().ToObject(ctx, IV_LV5_ERROR(e));
  Slot slot;
  if (!set->GetOwnPropertySlot(ctx, JSSet::symbol(), &slot)) {
    e->Report(Error::Type, "Set.prototype.size is not generic function");
    return JSEmpty;
  }
  JSSet::Data* entries = static_cast<JSSet::Data*>(slot.value().cell());
  return JSVal::UInt32(static_cast<uint32_t>(entries->set().size()));
}

// 23.2.3.10 Set.prototype.values()
JSVal SetValues(const Arguments& args, Error* e) {
  IV_LV5_CONSTRUCTOR_CHECK("Set.prototype.values", args, e);
  Context* ctx = args.ctx();
  JSObject* set = args.this_binding().ToObject(ctx, IV_LV5_ERROR(e));
  Slot slot;
  if (!set->GetOwnPropertySlot(ctx, JSSet::symbol(), &slot)) {
    e->Report(Error::Type, "Set.prototype.values is not generic function");
    return JSEmpty;
  }
  JSSet::Data* entries = static_cast<JSSet::Data*>(slot.value().cell());
  return JSSetIterator::New(ctx, entries, SetIterationKind::VALUE);
}

} } }  // namespace iv::lv5::runtime
