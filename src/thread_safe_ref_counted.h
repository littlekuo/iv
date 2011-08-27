#ifndef _IV_THREAD_SAFE_REF_COUNTED_H_
#define _IV_THREAD_SAFE_REF_COUNTED_H_
#include "noncopyable.h"
#include "atomic.h"
namespace iv {
namespace core {

template<typename T>
class ThreadSafeRefCounted : private iv::core::Noncopyable<T> {
 public:
  explicit ThreadSafeRefCounted(int count = 1)
    : ref_(count) {
  }

  void Retain() {
    AtomicIncrement(&ref_);
  }

  void Release() {
    assert(ref_ != 0);
    if (!(AtomicDecrement(&ref_))) {  // ref counter will be 0
      delete static_cast<T*>(this);
    }
  }

  int RetainCount() const {
    return ref_;
  }

 private:
  int ref_;
};

// for boost::intrusive_ptr
template<typename T>
inline void intrusive_ptr_add_ref(ThreadSafeRefCounted<T>* obj) {
  obj->Retain();
}

template<typename T>
inline void intrusive_ptr_release(ThreadSafeRefCounted<T>* obj) {
  obj->Release();
}

} }  // namespace iv::core
#endif  // _IV_REF_COUNTED_H_
