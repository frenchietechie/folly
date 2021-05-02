/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <folly/lang/Exception.h>

#include <atomic>
#include <cassert>

//  Accesses std::type_info and std::exception_ptr internals. Since these vary
//  by platform and library, import or copy the structure and function
//  signatures from each platform and library.
//
//  Support:
//    libstdc++ via libgcc libsupc++
//    libc++ via llvm libcxxabi
//    win32 via msvc crt
//
//  Both libstdc++ and libc++ are based on cxxabi but they are not identical.
//
//  Reference: https://github.com/RedBeard0531/better_exception_ptr.

//  imports ---

#if defined(__GLIBCXX__)

//  nada

#endif // defined(__GLIBCXX__)

#if defined(_LIBCPP_VERSION)

//  https://github.com/llvm/llvm-project/blob/llvmorg-11.0.1/libcxxabi/src/cxa_exception.h
//  https://github.com/llvm/llvm-project/blob/llvmorg-11.0.1/libcxxabi/src/private_typeinfo.h

#include <cxxabi.h>
#include <unwind.h>

namespace __cxxabiv1 {

struct _LIBCXXABI_HIDDEN __cxa_exception {
#if defined(__LP64__) || defined(_WIN64) || defined(_LIBCXXABI_ARM_EHABI)
  void* reserve;
  size_t referenceCount;
#endif
  std::type_info* exceptionType;
  void (*exceptionDestructor)(void*);
  void (*unexpectedHandler)();
  std::terminate_handler terminateHandler;
  __cxa_exception* nextException;
  int handlerCount;
#if defined(_LIBCXXABI_ARM_EHABI)
  __cxa_exception* nextPropagatingException;
  int propagationCount;
#else
  int handlerSwitchValue;
  const unsigned char* actionRecord;
  const unsigned char* languageSpecificData;
  void* catchTemp;
  void* adjustedPtr;
#endif
#if !defined(__LP64__) && !defined(_WIN64) && !defined(_LIBCXXABI_ARM_EHABI)
  size_t referenceCount;
#endif
  _Unwind_Exception unwindHeader;
};

class _LIBCXXABI_TYPE_VIS __shim_type_info : public std::type_info {
 public:
  _LIBCXXABI_HIDDEN virtual ~__shim_type_info();

  _LIBCXXABI_HIDDEN virtual void noop1() const;
  _LIBCXXABI_HIDDEN virtual void noop2() const;
  _LIBCXXABI_HIDDEN virtual bool can_catch(
      const __shim_type_info* thrown_type, void*& adjustedPtr) const = 0;
};

} // namespace __cxxabiv1

namespace abi = __cxxabiv1;

#endif // defined(_LIBCPP_VERSION)

#if defined(_WIN32)

#if defined(__clang__)
struct _s_ThrowInfo; // compiler intrinsic in msvc
typedef const struct _s_ThrowInfo _ThrowInfo;
#endif

#include <ehdata.h> // @manual

extern "C" _CRTIMP2 void* __cdecl __AdjustPointer(void*, PMD const&);

template <class _E>
void* __GetExceptionInfo(_E); // builtin

#endif // defined(_WIN32)

//  implementations ---

namespace folly {

#if defined(__GLIBCXX__)

std::type_info const* exception_ptr_get_type(
    std::exception_ptr const& ptr) noexcept {
  return !ptr ? nullptr : ptr.__cxa_exception_type();
}

void* exception_ptr_get_object(
    std::exception_ptr const& ptr,
    std::type_info const* const target) noexcept {
  if (!ptr) {
    return nullptr;
  }
  auto object = reinterpret_cast<void* const&>(ptr);
  auto type = ptr.__cxa_exception_type();
  return !target || target->__do_catch(type, &object, 1) ? object : nullptr;
}

#endif // defined(__GLIBCXX__)

#if defined(_LIBCPP_VERSION)

static void* cxxabi_get_object(std::exception_ptr const& ptr) noexcept {
  return reinterpret_cast<void* const&>(ptr);
}

std::type_info const* exception_ptr_get_type(
    std::exception_ptr const& ptr) noexcept {
  if (!ptr) {
    return nullptr;
  }
  auto object = cxxabi_get_object(ptr);
  auto exception = static_cast<abi::__cxa_exception*>(object) - 1;
  return exception->exceptionType;
}

void* exception_ptr_get_object(
    std::exception_ptr const& ptr,
    std::type_info const* const target) noexcept {
  if (!ptr) {
    return nullptr;
  }
  auto object = cxxabi_get_object(ptr);
  auto type = exception_ptr_get_type(ptr);
  auto stype = static_cast<abi::__shim_type_info const*>(type);
  auto starget = static_cast<abi::__shim_type_info const*>(target);
  return !target || starget->can_catch(stype, object) ? object : nullptr;
}

#endif // defined(_LIBCPP_VERSION)

#if defined(_WIN32)

template <typename T>
static T* win32_decode_pointer(T* ptr) {
  return static_cast<T*>(
      DecodePointer(const_cast<void*>(static_cast<void const*>(ptr))));
}

static EHExceptionRecord* win32_get_record(
    std::exception_ptr const& ptr) noexcept {
  return reinterpret_cast<std::shared_ptr<EHExceptionRecord> const&>(ptr).get();
}

static bool win32_eptr_throw_info_ptr_is_encoded() {
  // detect and cache whether this version of the microsoft c++ standard library
  // encodes the throw-info pointer in the std::exception_ptr internals
  //
  // earlier versions of std::exception_ptr did encode the throw-info pointer
  // but the most recent versions do not, as visible on github at
  // https://github.com/microsoft/STL
  //
  // prefer optimistic concurrency over pessimistic concurrency
  static std::atomic<int> cache{0}; // 0 uninit, -1 false, 1 true
  if (auto value = cache.load(std::memory_order_relaxed)) {
    return value > 0;
  }
  // detection is done by observing actual runtime behavior, using int as the
  // exception object type to save cost
  auto info = __GetExceptionInfo(0);
  auto ptr = std::make_exception_ptr(0);
  auto rec = win32_get_record(ptr);
  int value = 0;
  if (info == rec->params.pThrowInfo) {
    value = -1;
  }
  if (info == win32_decode_pointer(rec->params.pThrowInfo)) {
    value = +1;
  }
  assert(value);
  // last writer wins for simplicity, assuming it to be impossible for multiple
  // writers to write different values
  cache.store(value, std::memory_order_relaxed);
  return value > 0;
}

static ThrowInfo* win32_throw_info(EHExceptionRecord* rec) {
  auto encoded = win32_eptr_throw_info_ptr_is_encoded();
  auto info = rec->params.pThrowInfo;
  return encoded ? win32_decode_pointer(info) : info;
}

static std::uintptr_t win32_throw_image_base(EHExceptionRecord* rec) {
#if _EH_RELATIVE_TYPEINFO
  return reinterpret_cast<std::uintptr_t>(rec->params.pThrowImageBase);
#else
  (void)rec;
  return 0;
#endif
}

std::type_info const* exception_ptr_get_type(
    std::exception_ptr const& ptr) noexcept {
  auto rec = win32_get_record(ptr);
  if (!rec) {
    return nullptr;
  }
  auto base = win32_throw_image_base(rec);
  auto info = win32_throw_info(rec);
  auto cta_ = base + info->pCatchableTypeArray;
  auto cta = reinterpret_cast<CatchableTypeArray*>(cta_);
  // assumption: the compiler emits the most-derived type first
  auto ct_ = base + cta->arrayOfCatchableTypes[0];
  auto ct = reinterpret_cast<CatchableType*>(ct_);
  auto td_ = base + ct->pType;
  auto td = reinterpret_cast<TypeDescriptor*>(td_);
  return reinterpret_cast<std::type_info*>(td);
}

void* exception_ptr_get_object(
    std::exception_ptr const& ptr,
    std::type_info const* const target) noexcept {
  auto rec = win32_get_record(ptr);
  if (!rec) {
    return nullptr;
  }
  auto object = rec->params.pExceptionObject;
  if (!target) {
    return object;
  }
  auto base = win32_throw_image_base(rec);
  auto info = win32_throw_info(rec);
  auto cta_ = base + info->pCatchableTypeArray;
  auto cta = reinterpret_cast<CatchableTypeArray*>(cta_);
  for (int i = 0; i < cta->nCatchableTypes; i++) {
    auto ct_ = base + cta->arrayOfCatchableTypes[i];
    auto ct = reinterpret_cast<CatchableType*>(ct_);
    auto td_ = base + ct->pType;
    auto td = reinterpret_cast<TypeDescriptor*>(td_);
    if (*target == *reinterpret_cast<std::type_info*>(td)) {
      return __AdjustPointer(object, ct->thisDisplacement);
    }
  }
  return nullptr;
}

#endif // defined(_WIN32)

} // namespace folly
