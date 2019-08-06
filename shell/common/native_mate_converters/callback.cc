// Copyright (c) 2015 GitHub, Inc. All rights reserved.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/common/native_mate_converters/callback.h"
#include "base/stl_util.h"
#include "content/public/browser/browser_thread.h"

#include "native_mate/dictionary.h"

using content::BrowserThread;

namespace mate {

namespace internal {

namespace {

struct TranslaterHolder {
  explicit TranslaterHolder(v8::Isolate* isolate)
      : handle(isolate, v8::External::New(isolate, this)) {
    handle.SetWeak(this, &GC, v8::WeakCallbackType::kFinalizer);
  }
  ~TranslaterHolder() {
    if (!handle.IsEmpty()) {
      handle.ClearWeak();
      handle.Reset();
    }
  }

  static void GC(const v8::WeakCallbackInfo<TranslaterHolder>& data) {
    delete data.GetParameter();
  }

  v8::Global<v8::External> handle;
  Translater translater;
};

// Cached JavaScript version of |CallTranslater|.
v8::Persistent<v8::FunctionTemplate> g_call_translater;

void CallTranslater(v8::Local<v8::External> external,
                    v8::Local<v8::Object> state,
                    mate::Arguments* args) {
  // Whether the callback should only be called for once.
  v8::Isolate* isolate = args->isolate();
  auto context = isolate->GetCurrentContext();
  bool one_time =
      state->Has(context, mate::StringToSymbol(isolate, "oneTime")).ToChecked();

  // Check if the callback has already been called.
  if (one_time) {
    auto called_symbol = mate::StringToSymbol(isolate, "called");
    if (state->Has(context, called_symbol).ToChecked()) {
      args->ThrowError("callback can only be called for once");
      return;
    } else {
      state->Set(context, called_symbol, v8::Boolean::New(isolate, true))
          .ToChecked();
    }
  }

  TranslaterHolder* holder = static_cast<TranslaterHolder*>(external->Value());
  holder->translater.Run(args);

  // Free immediately for one-time callback.
  if (one_time)
    delete holder;
}

}  // namespace

// Destroy the class on UI thread when possible.
struct DeleteOnUIThread {
  template <typename T>
  static void Destruct(const T* x) {
    if (Locker::IsBrowserProcess() &&
        !BrowserThread::CurrentlyOn(BrowserThread::UI)) {
      BrowserThread::DeleteSoon(BrowserThread::UI, FROM_HERE, x);
    } else {
      delete x;
    }
  }
};

// Like v8::Global, but ref-counted.
template <typename T>
class RefCountedGlobal
    : public base::RefCountedThreadSafe<RefCountedGlobal<T>, DeleteOnUIThread> {
 public:
  RefCountedGlobal(v8::Isolate* isolate, v8::Local<v8::Value> value)
      : handle_(isolate, v8::Local<T>::Cast(value)) {}

  bool IsAlive() const { return !handle_.IsEmpty(); }

  v8::Local<T> NewHandle(v8::Isolate* isolate) const {
    return v8::Local<T>::New(isolate, handle_);
  }

 private:
  v8::Global<T> handle_;

  DISALLOW_COPY_AND_ASSIGN(RefCountedGlobal);
};

SafeV8Function::SafeV8Function(v8::Isolate* isolate, v8::Local<v8::Value> value)
    : v8_function_(new RefCountedGlobal<v8::Function>(isolate, value)) {}

SafeV8Function::SafeV8Function(const SafeV8Function& other)
    : v8_function_(other.v8_function_) {}

SafeV8Function::~SafeV8Function() {}

bool SafeV8Function::IsAlive() const {
  return v8_function_.get() && v8_function_->IsAlive();
}

v8::Local<v8::Function> SafeV8Function::NewHandle(v8::Isolate* isolate) const {
  return v8_function_->NewHandle(isolate);
}

v8::Local<v8::Value> CreateFunctionFromTranslater(v8::Isolate* isolate,
                                                  const Translater& translater,
                                                  bool one_time) {
  // The FunctionTemplate is cached.
  if (g_call_translater.IsEmpty())
    g_call_translater.Reset(isolate,
                            mate::CreateFunctionTemplate(
                                isolate, base::BindRepeating(&CallTranslater)));

  v8::Local<v8::FunctionTemplate> call_translater =
      v8::Local<v8::FunctionTemplate>::New(isolate, g_call_translater);
  auto* holder = new TranslaterHolder(isolate);
  holder->translater = translater;
  Dictionary state = mate::Dictionary::CreateEmpty(isolate);
  if (one_time)
    state.Set("oneTime", true);
  auto context = isolate->GetCurrentContext();
  return BindFunctionWith(
      isolate, context, call_translater->GetFunction(context).ToLocalChecked(),
      holder->handle.Get(isolate), state.GetHandle());
}

// func.bind(func, arg1).
// NB(zcbenz): Using C++11 version crashes VS.
v8::Local<v8::Value> BindFunctionWith(v8::Isolate* isolate,
                                      v8::Local<v8::Context> context,
                                      v8::Local<v8::Function> func,
                                      v8::Local<v8::Value> arg1,
                                      v8::Local<v8::Value> arg2) {
  v8::MaybeLocal<v8::Value> bind =
      func->Get(context, mate::StringToV8(isolate, "bind"));
  CHECK(!bind.IsEmpty());
  v8::Local<v8::Function> bind_func =
      v8::Local<v8::Function>::Cast(bind.ToLocalChecked());
  v8::Local<v8::Value> converted[] = {func, arg1, arg2};
  return bind_func->Call(context, func, base::size(converted), converted)
      .ToLocalChecked();
}

}  // namespace internal

}  // namespace mate