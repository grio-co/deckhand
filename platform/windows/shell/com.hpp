// Minimal COM smart pointer for the Windows shell (no ATL / WRL, so MSVC and MinGW build the same code).
#pragma once
#include <unknwn.h>

namespace deckhand::shell {

template <class T>
class Com {
 public:
  Com() = default;
  Com(const Com&) = delete;
  Com& operator=(const Com&) = delete;
  Com(Com&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
  Com& operator=(Com&& o) noexcept {
    if (this != &o) {
      reset();
      p_ = o.p_;
      o.p_ = nullptr;
    }
    return *this;
  }
  ~Com() { reset(); }
  void reset() {
    if (p_) p_->Release();
    p_ = nullptr;
  }
  T** put() {
    reset();
    return &p_;
  }
  void** put_void() { return reinterpret_cast<void**>(put()); }
  T* get() const { return p_; }
  T* operator->() const { return p_; }
  explicit operator bool() const { return p_ != nullptr; }

 private:
  T* p_ = nullptr;
};

}  // namespace deckhand::shell
