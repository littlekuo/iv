#ifndef IV_LV5_RAILGUN_OPERATION_H_
#define IV_LV5_RAILGUN_OPERATION_H_
#include <iv/arith.h>
#include <iv/platform_math.h>
#include <iv/lv5/arguments.h>
#include <iv/lv5/jsval.h>
#include <iv/lv5/chain.h>
#include <iv/lv5/railgun/fwd.h>
namespace iv {
namespace lv5 {
namespace railgun {
namespace detail {

template<int Target>
inline bool IsIncrementOverflowSafe(int32_t val);

template<>
inline bool IsIncrementOverflowSafe<-1>(int32_t val) {
  return val > INT32_MIN;
}

template<>
inline bool IsIncrementOverflowSafe<1>(int32_t val) {
  return val < INT32_MAX;
}

class VMArguments : public lv5::Arguments {
 public:
  VMArguments(Context* ctx, pointer ptr, std::size_t n)
    : lv5::Arguments(ctx, ptr, n) {
  }
};

}  // namespace detail

class Operation {
 public:
  explicit Operation(Context* ctx) : ctx_(ctx) { }

  JSVal Invoke(JSFunction* func, JSVal* arg, int argc_with_this, Error* e) {
    detail::VMArguments args(ctx_,
                             arg + (argc_with_this - 1),
                             argc_with_this - 1);
    return func->Call(&args, args.this_binding(), e);
  }

  JSVal InvokeMaybeEval(JSFunction* func,
                        JSVal* arg, int argc_with_this,
                        Frame* prev, Error* e) {
    detail::VMArguments args(ctx_,
                             arg + (argc_with_this - 1),
                             argc_with_this - 1);
    const JSAPI native = func->NativeFunction();
    if (native && native == &GlobalEval) {
      // direct call to eval point
      args.set_this_binding(args.this_binding());
      return DirectCallToEval(args, prev, e);
    }
    return func->Call(&args, args.this_binding(), e);
  }

  JSVal Construct(JSFunction* func, JSVal* arg, int argc_with_this, Error* e) {
    detail::VMArguments args(ctx_,
                             arg + (argc_with_this - 1),
                             argc_with_this - 1);
    args.set_constructor_call(true);
    return func->Construct(&args, e);
  }

  void RaiseReferenceError(Symbol name, Error* e) const {
    core::UStringBuilder builder;
    builder.Append('"');
    builder.Append(symbol::GetSymbolString(name));
    builder.Append("\" not defined");
    e->Report(Error::Reference, builder.BuildPiece());
  }

  void RaiseReferenceError(Error* e) const {
    core::UStringBuilder builder;
    builder.Append("Invalid left-hand side expression");
    e->Report(Error::Reference, builder.BuildPiece());
  }

  void RaiseImmutable(Symbol name, Error* e) const {
    core::UStringBuilder builder;
    builder.Append("mutating immutable binding \"");
    builder.Append(symbol::GetSymbolString(name));
    builder.Append("\" not allowed in strict mode");
    e->Report(Error::Type, builder.BuildPiece());
  }

  JSEnv* GetEnv(JSEnv* env, Symbol name) const {
    JSEnv* current = env;
    while (current) {
      if (current->HasBinding(ctx_, name)) {
        return current;
      } else {
        current = current->outer();
      }
    }
    return NULL;
  }

  JSDeclEnv* GetHeapEnv(JSEnv* env, uint32_t scope_nest_count) const {
    for (uint32_t i = 0; i < scope_nest_count; ++i) {
      env = env->outer();
    }
    assert(env->AsJSDeclEnv());
    return static_cast<JSDeclEnv*>(env);
  }

#define CHECK IV_LV5_ERROR_WITH(e, JSEmpty)

  JSVal LoadName(JSEnv* env, Symbol name, bool strict, Error* e) {
    if (JSEnv* current = GetEnv(env, name)) {
      return current->GetBindingValue(ctx_, name, strict, e);
    }
    RaiseReferenceError(name, e);
    return JSEmpty;
  }

  JSVal LoadHeap(JSEnv* env, Symbol name,
                 bool strict, uint32_t offset,
                 uint32_t scope_nest_count, Error* e) {
    return GetHeapEnv(env, scope_nest_count)->GetByOffset(offset, strict, e);
  }

  JSVal LoadProp(JSVal base, Symbol s, bool strict, Error* e) {
    base.CheckObjectCoercible(CHECK);
    return LoadPropImpl(base, s, strict, e);
  }

  template<OP::Type own, OP::Type proto, OP::Type chain, OP::Type generic>
  JSVal LoadProp(Instruction* instr,
                 JSVal base, Symbol s, bool strict, Error* e) {
    // opcode | (dst | base | name) | nop | nop | nop
    base.CheckObjectCoercible(CHECK);
    JSObject* obj = NULL;
    if (base.IsPrimitive()) {
      JSVal res;
      if (GetPrimitiveOwnProperty(base, s, &res)) {
        return res;
      } else {
        // if base is primitive, property not found in "this" object
        // so, lookup from proto
        obj = base.GetPrimitiveProto(ctx_);
      }
    } else {
      obj = base.object();
    }
    Slot slot;
    if (obj->GetPropertySlot(ctx_, s, &slot)) {
      // property found
      if (!slot.IsLoadCacheable()) {
        // bailout to generic
        instr[0] = Instruction::GetOPInstruction(generic);
        return slot.Get(ctx_, base, e);
      }

      // cache phase
      // own property / proto property / chain lookup property
      if (slot.base() == obj) {
        // own property
        instr[0] = Instruction::GetOPInstruction(own);
        instr[2].map = obj->map();
        instr[3].u32[0] = slot.offset();
        return slot.value();
      }

      if (slot.base() == obj->prototype()) {
        // proto property
        obj->FlattenMap();
        instr[0] = Instruction::GetOPInstruction(proto);
        instr[2].map = obj->map();
        instr[3].map = slot.base()->map();
        instr[4].u32[0] = slot.offset();
        return slot.value();
      }

      // chain property
      instr[0] = Instruction::GetOPInstruction(chain);
      instr[2].chain = Chain::New(obj, slot.base());
      instr[3].map = slot.base()->map();
      instr[4].u32[0] = slot.offset();
      return slot.value();
    } else {
      return JSUndefined;
    }
  }

  JSVal LoadElement(JSVal base,
                    JSVal element, bool strict, Error* e) {
    base.CheckObjectCoercible(CHECK);
    // array fast path
    uint32_t index;
    if (element.GetUInt32(&index)) {
      if (base.IsObject() && base.object()->IsClass<Class::Array>()) {
        JSArray* ary = static_cast<JSArray*>(base.object());
        if (ary->CanGetIndexDirect(index)) {
          return ary->GetIndexDirect(index);
        } else {
          return ary->JSArray::Get(ctx_, symbol::MakeSymbolFromIndex(index), e);
        }
      }
    }
    const Symbol s = element.ToSymbol(ctx_, CHECK);
    return LoadPropImpl(base, s, strict, e);
  }

  JSVal LoadPropImpl(JSVal base,
                     Symbol s, bool strict, Error* e) {
    if (base.IsPrimitive()) {
      return LoadPropPrimitive(base, s, strict, e);
    } else {
      return base.object()->Get(ctx_, s, e);
    }
  }

  JSVal LoadPropPrimitive(JSVal base,
                          Symbol s, bool strict, Error* e) {
    JSVal res;
    if (GetPrimitiveOwnProperty(base, s, &res)) {
      return res;
    }
    // if base is primitive, property not found in "this" object
    // so, lookup from proto
    Slot slot;
    JSObject* const proto = base.GetPrimitiveProto(ctx_);
    if (proto->GetPropertySlot(ctx_, s, &slot)) {
      return slot.Get(ctx_, base, e);
    } else {
      return JSUndefined;
    }
  }

#undef CHECK

#define CHECK IV_LV5_ERROR_VOID(e)

  void StoreName(JSEnv* env, Symbol name,
                 JSVal stored, bool strict, Error* e) {
    if (JSEnv* current = GetEnv(env, name)) {
      current->SetMutableBinding(ctx_, name, stored, strict, e);
    } else {
      if (strict) {
        e->Report(Error::Reference,
                  "putting to unresolvable reference "
                  "not allowed in strict reference");
      } else {
        ctx_->global_obj()->Put(ctx_, name, stored, strict, e);
      }
    }
  }

  void StoreHeap(JSEnv* env, Symbol name,
                 JSVal stored, bool strict,
                 uint32_t offset, uint32_t scope_nest_count, Error* e) {
    GetHeapEnv(env, scope_nest_count)->SetByOffset(offset, stored, strict, e);
  }

  void StoreElement(JSVal base, JSVal element,
                    JSVal stored, bool strict, Error* e) {
    base.CheckObjectCoercible(CHECK);
    // array fast path
    uint32_t index;
    if (element.GetUInt32(&index)) {
      if (base.IsObject() && base.object()->IsClass<Class::Array>()) {
        JSArray* ary = static_cast<JSArray*>(base.object());
        if (ary->CanSetIndexDirect(index)) {
          ary->SetIndexDirect(index, stored);
        } else {
          ary->JSArray::Put(
              ctx_,
              symbol::MakeSymbolFromIndex(index),
              stored, strict, e);
        }
        return;
      }
    }
    const Symbol s = element.ToSymbol(ctx_, CHECK);
    StorePropImpl(base, s, stored, strict, e);
  }

  void StoreProp(JSVal base, Symbol s,
                 JSVal stored, bool strict, Error* e) {
    base.CheckObjectCoercible(CHECK);
    StorePropImpl(base, s, stored, strict, e);
  }

  void StoreProp(JSVal base,
                 Instruction* instr,
                 OP::Type generic,
                 Symbol s, JSVal stored, bool strict, Error* e) {
    // opcode | (base | src | index) | nop | nop
    base.CheckObjectCoercible(CHECK);
    if (base.IsPrimitive()) {
      StorePropPrimitive(base, s, stored, strict, e);
    } else {
      // cache patten
      JSObject* obj = base.object();
      if (instr[2].map == obj->map()) {
        // map is cached, so use previous index code
        obj->GetSlot(instr[3].u32[0]) = stored;
        return;
      } else {
        Slot slot;
        if (obj->GetOwnPropertySlot(ctx_, s, &slot)) {
          // only data property
          if (slot.IsStoreCacheable()) {
            instr[2].map = obj->map();
            instr[3].u32[0] = slot.offset();
            obj->GetSlot(slot.offset()) = stored;
          } else {
            // dispatch generic path
            obj->Put(ctx_, s, stored, strict, e);
            instr[0] = Instruction::GetOPInstruction(generic);
          }
          return;
        } else {
          instr[2].map = NULL;
          obj->Put(ctx_, s, stored, strict, e);
          return;
        }
      }
    }
  }

  void StorePropImpl(JSVal base, Symbol s,
                     JSVal stored, bool strict, Error* e) {
    if (base.IsPrimitive()) {
      StorePropPrimitive(base, s, stored, strict, e);
    } else {
      base.object()->Put(ctx_, s, stored, strict, e);
    }
  }

  void StorePropPrimitive(JSVal base, Symbol s,
                          JSVal stored, bool strict, Error* e) {
    assert(base.IsPrimitive());
    JSObject* const o = base.ToObject(ctx_, CHECK);
    if (!o->CanPut(ctx_, s)) {
      if (strict) {
        e->Report(Error::Type, "cannot put value to object");
      }
      return;
    }
    const PropertyDescriptor own_desc = o->GetOwnProperty(ctx_, s);
    if (!own_desc.IsEmpty() && own_desc.IsData()) {
      if (strict) {
        e->Report(Error::Type,
                  "value to symbol defined and not data descriptor");
      }
      return;
    }
    const PropertyDescriptor desc = o->GetProperty(ctx_, s);
    if (!desc.IsEmpty() && desc.IsAccessor()) {
      ScopedArguments a(ctx_, 1, CHECK);
      a[0] = stored;
      const AccessorDescriptor* const ac = desc.AsAccessorDescriptor();
      assert(ac->set());
      ac->set()->AsCallable()->Call(&a, base, e);
      return;
    } else {
      if (strict) {
        e->Report(Error::Type, "value to symbol in transient object");
      }
    }
  }
#undef CHECK

#define CHECK IV_LV5_ERROR_WITH(e, JSEmpty)

  JSVal BinaryAdd(JSVal lhs, JSVal rhs, Error* e) const {
    if (lhs.IsNumber() && rhs.IsNumber()) {
      return lhs.number() + rhs.number();
    }
    if (lhs.IsString()) {
      if (rhs.IsString()) {
        return JSString::New(ctx_, lhs.string(), rhs.string());
      } else {
        const JSVal rp = rhs.ToPrimitive(ctx_, Hint::NONE, CHECK);
        JSString* const rs = rp.ToString(ctx_, CHECK);
        return JSString::New(ctx_, lhs.string(), rs);
      }
    }

    const JSVal lprim = lhs.ToPrimitive(ctx_, Hint::NONE, CHECK);
    const JSVal rprim = rhs.ToPrimitive(ctx_, Hint::NONE, CHECK);
    if (lprim.IsString() || rprim.IsString()) {
      JSString* const lstr = lprim.ToString(ctx_, CHECK);
      JSString* const rstr = rprim.ToString(ctx_, CHECK);
      return JSString::New(ctx_, lstr, rstr);
    }

    const double left = lprim.ToNumber(ctx_, CHECK);
    return left + rprim.ToNumber(ctx_, e);
  }

  JSVal BinarySub(JSVal lhs, JSVal rhs, Error* e) const {
    const double left = lhs.ToNumber(ctx_, CHECK);
    return left -  rhs.ToNumber(ctx_, e);
  }

  JSVal BinaryMultiply(JSVal lhs, JSVal rhs, Error* e) const {
    const double left = lhs.ToNumber(ctx_, CHECK);
    return left * rhs.ToNumber(ctx_, e);
  }

  JSVal BinaryDivide(JSVal lhs, JSVal rhs, Error* e) const {
    const double left = lhs.ToNumber(ctx_, CHECK);
    return left / rhs.ToNumber(ctx_, e);
  }

  JSVal BinaryModulo(JSVal lhs, JSVal rhs, Error* e) const {
    const double left = lhs.ToNumber(ctx_, CHECK);
    return core::math::Modulo(left, rhs.ToNumber(ctx_, e));
  }

  JSVal BinaryLShift(JSVal lhs, JSVal rhs, Error* e) const {
    const int32_t left = lhs.ToInt32(ctx_, CHECK);
    return JSVal::Int32(left << (rhs.ToInt32(ctx_, e) & 0x1f));
  }

  JSVal BinaryRShift(JSVal lhs, JSVal rhs, Error* e) const {
    const int32_t left = lhs.ToInt32(ctx_, CHECK);
    return JSVal::Int32(left >> (rhs.ToInt32(ctx_, e) & 0x1f));
  }

  JSVal BinaryRShiftLogical(JSVal lhs,
                            JSVal rhs, Error* e) const {
    const uint32_t left = lhs.ToUInt32(ctx_, CHECK);
    return JSVal::UInt32(left >> (rhs.ToInt32(ctx_, e) & 0x1f));
  }

  bool BinaryCompareLT(JSVal lhs,
                       JSVal rhs, Error* e) const {
    return JSVal::Compare<true>(ctx_, lhs, rhs, e) == CMP_TRUE;
  }

  bool BinaryCompareLTE(JSVal lhs,
                        JSVal rhs, Error* e) const {
    return JSVal::Compare<false>(ctx_, rhs, lhs, e) == CMP_FALSE;
  }

  bool BinaryCompareGT(JSVal lhs,
                       JSVal rhs, Error* e) const {
    return JSVal::Compare<false>(ctx_, rhs, lhs, e) == CMP_TRUE;
  }

  bool BinaryCompareGTE(JSVal lhs,
                        JSVal rhs, Error* e) const {
    return JSVal::Compare<true>(ctx_, lhs, rhs, e) == CMP_FALSE;
  }

  bool BinaryInstanceof(JSVal lhs,
                        JSVal rhs, Error* e) const {
    if (!rhs.IsObject()) {
      e->Report(Error::Type, "instanceof requires object");
      return false;
    }
    JSObject* const robj = rhs.object();
    if (!robj->IsCallable()) {
      e->Report(Error::Type, "instanceof requires constructor");
      return false;
    }
    return robj->AsCallable()->HasInstance(ctx_, lhs, e);
  }

  bool BinaryIn(JSVal lhs,
                JSVal rhs, Error* e) const {
    if (!rhs.IsObject()) {
      e->Report(Error::Type, "in requires object");
      return false;
    }
    const Symbol s = lhs.ToSymbol(ctx_, IV_LV5_ERROR_WITH(e, false));
    return rhs.object()->HasProperty(ctx_, s);
  }

  bool BinaryEqual(JSVal lhs,
                   JSVal rhs, Error* e) const {
    return JSVal::AbstractEqual(ctx_, lhs, rhs, e);
  }

  bool BinaryStrictEqual(JSVal lhs,
                         JSVal rhs) const {
    return JSVal::StrictEqual(lhs, rhs);
  }

  bool BinaryNotEqual(JSVal lhs,
                      JSVal rhs, Error* e) const {
    return !JSVal::AbstractEqual(ctx_, lhs, rhs, e);
  }

  bool BinaryStrictNotEqual(JSVal lhs,
                            JSVal rhs) const {
    return !JSVal::StrictEqual(lhs, rhs);
  }

  int32_t BinaryBitAnd(JSVal lhs,
                       JSVal rhs, Error* e) const {
    const int32_t left = lhs.ToInt32(ctx_, IV_LV5_ERROR_WITH(e, 0));
    return left & rhs.ToInt32(ctx_, e);
  }

  int32_t BinaryBitXor(JSVal lhs,
                       JSVal rhs, Error* e) const {
    const int32_t left = lhs.ToInt32(ctx_, IV_LV5_ERROR_WITH(e, 0));
    return left ^ rhs.ToInt32(ctx_, e);
  }

  int32_t BinaryBitOr(JSVal lhs,
                      JSVal rhs, Error* e) const {
    const int32_t left = lhs.ToInt32(ctx_, IV_LV5_ERROR_WITH(e, 0));
    return left | rhs.ToInt32(ctx_, e);
  }

  template<int Target, std::size_t Returned>
  JSVal IncrementName(JSEnv* env, Symbol s, bool strict, Error* e) {
    if (JSEnv* current = GetEnv(env, s)) {
      const JSVal w = current->GetBindingValue(ctx_, s, strict, CHECK);
      if (w.IsInt32() && detail::IsIncrementOverflowSafe<Target>(w.int32())) {
        std::tuple<JSVal, JSVal> results;
        const int32_t target = w.int32();
        std::get<0>(results) = w;
        std::get<1>(results) = JSVal::Int32(target + Target);
        current->SetMutableBinding(
            ctx_, s,
            std::get<1>(results), strict, e);
        return std::get<Returned>(results);
      } else {
        std::tuple<double, double> results;
        std::get<0>(results) = w.ToNumber(ctx_, CHECK);
        std::get<1>(results) = std::get<0>(results) + Target;
        current->SetMutableBinding(ctx_, s,
                                   std::get<1>(results), strict, e);
        return std::get<Returned>(results);
      }
    }
    RaiseReferenceError(s, e);
    return 0.0;
  }

  template<int Target, std::size_t Returned>
  JSVal IncrementHeap(JSEnv* env, Symbol s,
                      bool strict, uint32_t offset,
                      uint32_t scope_nest_count, Error* e) {
    JSDeclEnv* decl = GetHeapEnv(env, scope_nest_count);
    const JSVal w = decl->GetByOffset(offset, strict, CHECK);
    if (w.IsInt32() && detail::IsIncrementOverflowSafe<Target>(w.int32())) {
      std::tuple<JSVal, JSVal> results;
      const int32_t target = w.int32();
      std::get<0>(results) = w;
      std::get<1>(results) = JSVal::Int32(target + Target);
      decl->SetByOffset(offset, std::get<1>(results), strict, e);
      return std::get<Returned>(results);
    } else {
      std::tuple<double, double> results;
      std::get<0>(results) = w.ToNumber(ctx_, CHECK);
      std::get<1>(results) = std::get<0>(results) + Target;
      decl->SetByOffset(offset, std::get<1>(results), strict, e);
      return std::get<Returned>(results);
    }
  }

  template<int Target, std::size_t Returned>
  JSVal IncrementElement(JSVal base,
                         JSVal element, bool strict, Error* e) {
    base.CheckObjectCoercible(CHECK);
    const Symbol s = element.ToSymbol(ctx_, CHECK);
    const JSVal w = LoadPropImpl(base, s, strict, CHECK);
    if (w.IsInt32() && detail::IsIncrementOverflowSafe<Target>(w.int32())) {
      std::tuple<JSVal, JSVal> results;
      const int32_t target = w.int32();
      std::get<0>(results) = w;
      std::get<1>(results) = JSVal::Int32(target + Target);
      StorePropImpl(base, s, std::get<1>(results), strict, e);
      return std::get<Returned>(results);
    } else {
      std::tuple<double, double> results;
      std::get<0>(results) = w.ToNumber(ctx_, CHECK);
      std::get<1>(results) = std::get<0>(results) + Target;
      StorePropImpl(base, s, std::get<1>(results), strict, e);
      return std::get<Returned>(results);
    }
  }

  template<int Target, std::size_t Returned>
  JSVal IncrementProp(JSVal base,
                      Symbol s, bool strict, Error* e) {
    base.CheckObjectCoercible(CHECK);
    const JSVal w = LoadPropImpl(base, s, strict, CHECK);
    if (w.IsInt32() && detail::IsIncrementOverflowSafe<Target>(w.int32())) {
      std::tuple<JSVal, JSVal> results;
      const int32_t target = w.int32();
      std::get<0>(results) = w;
      std::get<1>(results) = JSVal::Int32(target + Target);
      StorePropImpl(base, s, std::get<1>(results), strict, e);
      return std::get<Returned>(results);
    } else {
      std::tuple<double, double> results;
      std::get<0>(results) = w.ToNumber(ctx_, CHECK);
      std::get<1>(results) = std::get<0>(results) + Target;
      StorePropImpl(base, s, std::get<1>(results), strict, e);
      return std::get<Returned>(results);
    }
  }

  JSVal LoadGlobal(JSGlobal* global,
                   Instruction* instr, const Symbol& s, bool strict, Error* e) {
    // opcode | (dst | index) | nop | nop
    if (instr[2].map == global->map()) {
      // map is cached, so use previous index code
      return global->GetSlot(instr[3].u32[0]);
    } else {
      // now Own Property Pattern only implemented
      Slot slot;
      if (global->GetOwnPropertySlot(ctx_, s, &slot)) {
        if (slot.IsLoadCacheable()) {
          instr[2].map = global->map();
          instr[3].u32[0] = slot.offset();
          return slot.value();
        }
        return slot.Get(ctx_, global, e);
      } else {
        instr[2].map = NULL;
        return LoadName(ctx_->global_env(), s, strict, e);
      }
    }
  }

  bool GetPrimitiveOwnProperty(JSVal base, const Symbol& s, JSVal* res) {
    // section 8.7.1 special [[Get]]
    assert(base.IsPrimitive());
    if (base.IsString()) {
      // string short circuit
      JSString* str = base.string();
      if (s == symbol::length()) {
        *res = JSVal::UInt32(static_cast<uint32_t>(str->size()));
        return true;
      }
      if (symbol::IsArrayIndexSymbol(s)) {
        const uint32_t index = symbol::GetIndexFromSymbol(s);
        if (index < str->size()) {
          *res = JSString::NewSingle(ctx_, str->At(index));
          return true;
        }
      }
    }
    return false;
  }

#undef CHECK

  inline Context* ctx() const { return ctx_; }
 private:
  Context* ctx_;
};

} } }  // namespace iv::lv5::railgun
#endif  // IV_LV5_RAILGUN_OPERATION_H_
