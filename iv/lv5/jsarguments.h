#ifndef IV_LV5_JSARGUMENTS_H_
#define IV_LV5_JSARGUMENTS_H_
#include <iv/ast.h>
#include <iv/lv5/symbol.h>
#include <iv/lv5/gc_template.h>
#include <iv/lv5/error.h>
#include <iv/lv5/property.h>
#include <iv/lv5/jsenv.h>
#include <iv/lv5/jsobject.h>
#include <iv/lv5/map.h>
#include <iv/lv5/slot.h>
#include <iv/lv5/arguments.h>
#include <iv/lv5/error_check.h>
#include <iv/lv5/bind.h>
#include <iv/lv5/specialized_ast.h>
#include <iv/lv5/radio/core_fwd.h>
namespace iv {
namespace lv5 {

class Context;
class AstFactory;

// only class placeholder
class JSArguments {
 public:
  static const Class* GetClass() {
    static const Class cls = {
      "Arguments",
      Class::Arguments
    };
    return &cls;
  }
};

class JSNormalArguments : public JSObject {
 public:
  typedef GCVector<Symbol>::type Indice;

  template<typename Idents, typename ArgsReverseIter>
  static JSNormalArguments* New(Context* ctx,
                                JSFunction* func,
                                const Idents& names,
                                ArgsReverseIter it,
                                ArgsReverseIter last,
                                JSDeclEnv* env,
                                Error* e) {
    JSNormalArguments* const obj = new JSNormalArguments(ctx, env);
    const uint32_t len = static_cast<uint32_t>(std::distance(it, last));
    obj->set_cls(JSArguments::GetClass());
    obj->set_prototype(context::GetClassSlot(ctx, Class::Arguments).prototype);
    bind::Object binder(ctx, obj);
    binder
        .def(symbol::length(),
             JSVal::UInt32(len), ATTR::W | ATTR::C);
    SetArguments(ctx, obj, &binder, names, it, last, len);
    binder.def(symbol::callee(), func, ATTR::W | ATTR::C);
    return obj;
  }

  JSVal Get(Context* ctx, Symbol name, Error* e) {
    if (symbol::IsArrayIndexSymbol(name)) {
      const uint32_t index = symbol::GetIndexFromSymbol(name);
      if (mapping_.size() > index) {
        const Symbol mapped = mapping_[index];
        if (mapped != symbol::kDummySymbol) {
          return env_->GetBindingValue(ctx, mapped, true, e);
        }
      }
    }
    const JSVal v = JSObject::Get(ctx, name, IV_LV5_ERROR(e));
    if (name == symbol::caller() &&
        v.IsCallable() &&
        v.object()->AsCallable()->IsStrict()) {
      e->Report(Error::Type,
                "access to strict function \"caller\" not allowed");
      return JSUndefined;
    }
    return v;
  }

  bool GetOwnPropertySlot(Context* ctx,
                          Symbol name, Slot* slot) const {
    if (!JSObject::GetOwnPropertySlot(ctx, name, slot)) {
      return false;
    }
    if (symbol::IsArrayIndexSymbol(name)) {
      const uint32_t index = symbol::GetIndexFromSymbol(name);
      if (mapping_.size() > index) {
        const Symbol mapped = mapping_[index];
        if (mapped != symbol::kDummySymbol) {
          const JSVal val = env_->GetBindingValue(mapped);
          slot->set_descriptor(
              DataDescriptor(
                  val,
                  slot->desc().attrs() & ATTR::DATA_ATTR_MASK));
          return true;
        }
      }
    }
    return true;
  }

  // TODO(Constellation): clean up
  bool DefineOwnPropertyPatching(
      Context* ctx,
      Symbol name,
      const PropertyDescriptor& desc, bool th, Error* e) {
    Slot slot;
    if (JSObject::GetOwnPropertySlot(ctx, name, &slot)) {
      // found
      const PropertyDescriptor current = slot.desc();
      assert(!current.IsEmpty());
      bool returned = false;
      if (IsDefineOwnPropertyAccepted(current, desc, th, &returned, e)) {
        if (slot.IsCacheable()) {
          GetSlot(slot.offset()) = PropertyDescriptor::Merge(desc, current);
        } else {
          // add property transition
          // searching already created maps
          // and if this is available, move to this
          std::size_t offset;
          map_ = map_->AddPropertyTransition(ctx, name, &offset);
          slots_.resize(map_->GetSlotsSize(), JSEmpty);
          // set newly created property
          GetSlot(offset) = PropertyDescriptor::Merge(desc, current);
        }
      }
      return returned;
    } else {
      // not found
      if (!IsExtensible()) {
        if (th) {
          e->Report(Error::Type, "object not extensible");\
        }
        return false;
      } else {
        // add property transition
        // searching already created maps and if this is available, move to this
        std::size_t offset;
        map_ = map_->AddPropertyTransition(ctx, name, &offset);
        slots_.resize(map_->GetSlotsSize(), JSEmpty);
        // set newly created property
        GetSlot(offset) = PropertyDescriptor::SetDefault(desc);
        return true;
      }
    }
  }

  bool DefineOwnProperty(Context* ctx, Symbol name,
                         const PropertyDescriptor& desc,
                         bool th, Error* e) {
    // monkey patching...
    // not reserve map object, so not use default DefineOwnProperty
    const bool allowed = DefineOwnPropertyPatching(ctx, name, desc, false, e);
    if (!allowed) {
      if (th) {
        e->Report(Error::Type,
                  "[[DefineOwnProperty]] failed");
      }
      return false;
    }
    if (symbol::IsArrayIndexSymbol(name)) {
      const uint32_t index = symbol::GetIndexFromSymbol(name);
      if (mapping_.size() > index) {
        const Symbol mapped = mapping_[index];
        if (mapped != symbol::kDummySymbol) {
          if (desc.IsAccessorDescriptor()) {
            mapping_[index] = symbol::kDummySymbol;
          } else {
            if (desc.IsDataDescriptor()) {
              const DataDescriptor* const data = desc.AsDataDescriptor();
              if (!data->IsValueAbsent()) {
                env_->SetMutableBinding(ctx, mapped, data->value(),
                                        th, IV_LV5_ERROR_WITH(e, false));
              }
              if (!data->IsWritableAbsent() && !data->IsWritable()) {
                mapping_[index] = symbol::kDummySymbol;
              }
            }
          }
        }
      }
    }
    return true;
  }

  bool Delete(Context* ctx, Symbol name, bool th, Error* e) {
    const bool result = JSObject::Delete(ctx, name, th,
                                         IV_LV5_ERROR_WITH(e, result));
    if (result) {
      if (symbol::IsArrayIndexSymbol(name)) {
        const uint32_t index = symbol::GetIndexFromSymbol(name);
        if (mapping_.size() > index) {
          const Symbol mapped = mapping_[index];
          if (mapped != symbol::kDummySymbol) {
            mapping_[index] = symbol::kDummySymbol;
            return true;
          }
        }
      }
    }
    return result;
  }

  void MarkChildren(radio::Core* core) {
    JSObject::MarkChildren(core);
    core->MarkCell(env_);
  }

 private:
  JSNormalArguments(Context* ctx, JSDeclEnv* env)
    : JSObject(Map::NewUniqueMap(ctx)),
      env_(env),
      mapping_() { }

  template<typename Idents, typename ArgsReverseIter>
  static void SetArguments(Context* ctx,
                           JSNormalArguments* obj,
                           bind::Object* binder,
                           const Idents& names,
                           ArgsReverseIter it, ArgsReverseIter last,
                           uint32_t len) {
    uint32_t index = len - 1;
    const uint32_t names_len = static_cast<uint32_t>(names.size());
    obj->mapping_.resize((std::min)(len, names_len), symbol::kDummySymbol);
    for (; it != last; ++it) {
      binder->def(symbol::MakeSymbolFromIndex(index),
                  *it, ATTR::W | ATTR::E | ATTR::C);
      if (index < names_len) {
        obj->mapping_[index] = GetSymbol(names, index);
      }
      index -= 1;
    }
  }

  template<typename Ident>
  static Symbol GetSymbol(const Ident& ident, uint32_t index) {
    return ident[index];
  }

  static Symbol GetSymbol(const Assigneds& ident, uint32_t index) {
    return ident[index]->symbol();
  }

  JSDeclEnv* env_;
  Indice mapping_;
};

// not search environment
class JSStrictArguments : public JSObject {
 public:
  template<typename ArgsReverseIter>
  static JSStrictArguments* New(Context* ctx,
                                JSFunction* func,
                                ArgsReverseIter it,
                                ArgsReverseIter last,
                                Error* e) {
    JSStrictArguments* const obj = new JSStrictArguments(ctx);
    const uint32_t len = static_cast<uint32_t>(std::distance(it, last));
    obj->set_cls(JSArguments::GetClass());
    obj->set_prototype(context::GetClassSlot(ctx, Class::Arguments).prototype);
    bind::Object binder(ctx, obj);
    binder
        .def(symbol::length(),
             JSVal::UInt32(len), ATTR::W | ATTR::C);
    uint32_t index = len - 1;
    for (; it != last; ++it, --index) {
      binder.def(symbol::MakeSymbolFromIndex(index),
                 *it, ATTR::W | ATTR::E | ATTR::C);
    }

    JSFunction* const throw_type_error = context::throw_type_error(ctx);
    binder
        .def_accessor(symbol::caller(),
                      throw_type_error,
                      throw_type_error,
                      ATTR::NONE)
        .def_accessor(symbol::callee(),
                      throw_type_error,
                      throw_type_error,
                      ATTR::NONE);
    return obj;
  }

  JSVal Get(Context* ctx, Symbol name, Error* e) {
    const JSVal v = JSObject::Get(ctx, name, IV_LV5_ERROR(e));
    if (name == symbol::caller() &&
        v.IsCallable() &&
        v.object()->AsCallable()->IsStrict()) {
      e->Report(Error::Type,
                "access to strict function \"caller\" not allowed");
      return JSUndefined;
    }
    return v;
  }

 private:
  explicit JSStrictArguments(Context* ctx)
    : JSObject(Map::NewUniqueMap(ctx)) {
  }
};

} }  // namespace iv::lv5
#endif  // IV_LV5_JSARGUMENTS_H_