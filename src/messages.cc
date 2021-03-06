// Copyright 2011 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/messages.h"

#include "src/api.h"
#include "src/bootstrapper.h"
#include "src/execution.h"
#include "src/isolate-inl.h"
#include "src/keys.h"
#include "src/string-builder.h"
#include "src/wasm/wasm-module.h"

namespace v8 {
namespace internal {

MessageLocation::MessageLocation(Handle<Script> script, int start_pos,
                                 int end_pos)
    : script_(script), start_pos_(start_pos), end_pos_(end_pos) {}
MessageLocation::MessageLocation(Handle<Script> script, int start_pos,
                                 int end_pos, Handle<JSFunction> function)
    : script_(script),
      start_pos_(start_pos),
      end_pos_(end_pos),
      function_(function) {}
MessageLocation::MessageLocation() : start_pos_(-1), end_pos_(-1) {}

// If no message listeners have been registered this one is called
// by default.
void MessageHandler::DefaultMessageReport(Isolate* isolate,
                                          const MessageLocation* loc,
                                          Handle<Object> message_obj) {
  base::SmartArrayPointer<char> str = GetLocalizedMessage(isolate, message_obj);
  if (loc == NULL) {
    PrintF("%s\n", str.get());
  } else {
    HandleScope scope(isolate);
    Handle<Object> data(loc->script()->name(), isolate);
    base::SmartArrayPointer<char> data_str;
    if (data->IsString())
      data_str = Handle<String>::cast(data)->ToCString(DISALLOW_NULLS);
    PrintF("%s:%i: %s\n", data_str.get() ? data_str.get() : "<unknown>",
           loc->start_pos(), str.get());
  }
}


Handle<JSMessageObject> MessageHandler::MakeMessageObject(
    Isolate* isolate, MessageTemplate::Template message,
    MessageLocation* location, Handle<Object> argument,
    Handle<JSArray> stack_frames) {
  Factory* factory = isolate->factory();

  int start = -1;
  int end = -1;
  Handle<Object> script_handle = factory->undefined_value();
  if (location != NULL) {
    start = location->start_pos();
    end = location->end_pos();
    script_handle = Script::GetWrapper(location->script());
  } else {
    script_handle = Script::GetWrapper(isolate->factory()->empty_script());
  }

  Handle<Object> stack_frames_handle = stack_frames.is_null()
      ? Handle<Object>::cast(factory->undefined_value())
      : Handle<Object>::cast(stack_frames);

  Handle<JSMessageObject> message_obj = factory->NewJSMessageObject(
      message, argument, start, end, script_handle, stack_frames_handle);

  return message_obj;
}


void MessageHandler::ReportMessage(Isolate* isolate, MessageLocation* loc,
                                   Handle<JSMessageObject> message) {
  // We are calling into embedder's code which can throw exceptions.
  // Thus we need to save current exception state, reset it to the clean one
  // and ignore scheduled exceptions callbacks can throw.

  // We pass the exception object into the message handler callback though.
  Object* exception_object = isolate->heap()->undefined_value();
  if (isolate->has_pending_exception()) {
    exception_object = isolate->pending_exception();
  }
  Handle<Object> exception(exception_object, isolate);

  Isolate::ExceptionScope exception_scope(isolate);
  isolate->clear_pending_exception();
  isolate->set_external_caught_exception(false);

  // Turn the exception on the message into a string if it is an object.
  if (message->argument()->IsJSObject()) {
    HandleScope scope(isolate);
    Handle<Object> argument(message->argument(), isolate);

    MaybeHandle<Object> maybe_stringified;
    Handle<Object> stringified;
    // Make sure we don't leak uncaught internally generated Error objects.
    if (argument->IsJSError()) {
      Handle<Object> args[] = {argument};
      maybe_stringified = Execution::TryCall(
          isolate, isolate->no_side_effects_to_string_fun(),
          isolate->factory()->undefined_value(), arraysize(args), args);
    } else {
      v8::TryCatch catcher(reinterpret_cast<v8::Isolate*>(isolate));
      catcher.SetVerbose(false);
      catcher.SetCaptureMessage(false);

      maybe_stringified = Object::ToString(isolate, argument);
    }

    if (!maybe_stringified.ToHandle(&stringified)) {
      stringified = isolate->factory()->NewStringFromAsciiChecked("exception");
    }
    message->set_argument(*stringified);
  }

  v8::Local<v8::Message> api_message_obj = v8::Utils::MessageToLocal(message);
  v8::Local<v8::Value> api_exception_obj = v8::Utils::ToLocal(exception);

  v8::NeanderArray global_listeners(isolate->factory()->message_listeners());
  int global_length = global_listeners.length();
  if (global_length == 0) {
    DefaultMessageReport(isolate, loc, message);
    if (isolate->has_scheduled_exception()) {
      isolate->clear_scheduled_exception();
    }
  } else {
    for (int i = 0; i < global_length; i++) {
      HandleScope scope(isolate);
      if (global_listeners.get(i)->IsUndefined(isolate)) continue;
      v8::NeanderObject listener(JSObject::cast(global_listeners.get(i)));
      Handle<Foreign> callback_obj(Foreign::cast(listener.get(0)));
      v8::MessageCallback callback =
          FUNCTION_CAST<v8::MessageCallback>(callback_obj->foreign_address());
      Handle<Object> callback_data(listener.get(1), isolate);
      {
        // Do not allow exceptions to propagate.
        v8::TryCatch try_catch(reinterpret_cast<v8::Isolate*>(isolate));
        callback(api_message_obj, callback_data->IsUndefined(isolate)
                                      ? api_exception_obj
                                      : v8::Utils::ToLocal(callback_data));
      }
      if (isolate->has_scheduled_exception()) {
        isolate->clear_scheduled_exception();
      }
    }
  }
}


Handle<String> MessageHandler::GetMessage(Isolate* isolate,
                                          Handle<Object> data) {
  Handle<JSMessageObject> message = Handle<JSMessageObject>::cast(data);
  Handle<Object> arg = Handle<Object>(message->argument(), isolate);
  return MessageTemplate::FormatMessage(isolate, message->type(), arg);
}


base::SmartArrayPointer<char> MessageHandler::GetLocalizedMessage(
    Isolate* isolate, Handle<Object> data) {
  HandleScope scope(isolate);
  return GetMessage(isolate, data)->ToCString(DISALLOW_NULLS);
}


CallSite::CallSite(Isolate* isolate, Handle<JSObject> call_site_obj)
    : isolate_(isolate) {
  Handle<Object> maybe_function = JSObject::GetDataProperty(
      call_site_obj, isolate->factory()->call_site_function_symbol());
  if (maybe_function->IsJSFunction()) {
    // javascript
    fun_ = Handle<JSFunction>::cast(maybe_function);
    receiver_ = JSObject::GetDataProperty(
        call_site_obj, isolate->factory()->call_site_receiver_symbol());
  } else {
    Handle<Object> maybe_wasm_func_index = JSObject::GetDataProperty(
        call_site_obj, isolate->factory()->call_site_wasm_func_index_symbol());
    if (!maybe_wasm_func_index->IsSmi()) {
      // invalid: neither javascript nor wasm
      return;
    }
    // wasm
    wasm_obj_ = Handle<JSObject>::cast(JSObject::GetDataProperty(
        call_site_obj, isolate->factory()->call_site_wasm_obj_symbol()));
    wasm_func_index_ = Smi::cast(*maybe_wasm_func_index)->value();
    DCHECK(static_cast<int>(wasm_func_index_) >= 0);
  }

  CHECK(JSObject::GetDataProperty(
            call_site_obj, isolate->factory()->call_site_position_symbol())
            ->ToInt32(&pos_));
}


Handle<Object> CallSite::GetFileName() {
  if (!IsJavaScript()) return isolate_->factory()->null_value();
  Object* script = fun_->shared()->script();
  if (!script->IsScript()) return isolate_->factory()->null_value();
  return Handle<Object>(Script::cast(script)->name(), isolate_);
}


Handle<Object> CallSite::GetFunctionName() {
  if (IsWasm()) {
    return wasm::GetWasmFunctionNameOrNull(isolate_, wasm_obj_,
                                           wasm_func_index_);
  }
  Handle<String> result = JSFunction::GetName(fun_);
  if (result->length() != 0) return result;

  Handle<Object> script(fun_->shared()->script(), isolate_);
  if (script->IsScript() &&
      Handle<Script>::cast(script)->compilation_type() ==
          Script::COMPILATION_TYPE_EVAL) {
    return isolate_->factory()->eval_string();
  }
  return isolate_->factory()->null_value();
}

Handle<Object> CallSite::GetScriptNameOrSourceUrl() {
  if (!IsJavaScript()) return isolate_->factory()->null_value();
  Object* script_obj = fun_->shared()->script();
  if (!script_obj->IsScript()) return isolate_->factory()->null_value();
  Handle<Script> script(Script::cast(script_obj), isolate_);
  Object* source_url = script->source_url();
  if (source_url->IsString()) return Handle<Object>(source_url, isolate_);
  return Handle<Object>(script->name(), isolate_);
}

bool CheckMethodName(Isolate* isolate, Handle<JSObject> obj, Handle<Name> name,
                     Handle<JSFunction> fun,
                     LookupIterator::Configuration config) {
  LookupIterator iter =
      LookupIterator::PropertyOrElement(isolate, obj, name, config);
  if (iter.state() == LookupIterator::DATA) {
    return iter.GetDataValue().is_identical_to(fun);
  } else if (iter.state() == LookupIterator::ACCESSOR) {
    Handle<Object> accessors = iter.GetAccessors();
    if (accessors->IsAccessorPair()) {
      Handle<AccessorPair> pair = Handle<AccessorPair>::cast(accessors);
      return pair->getter() == *fun || pair->setter() == *fun;
    }
  }
  return false;
}


Handle<Object> CallSite::GetMethodName() {
  if (!IsJavaScript() || receiver_->IsNull(isolate_) ||
      receiver_->IsUndefined(isolate_)) {
    return isolate_->factory()->null_value();
  }
  Handle<JSReceiver> receiver =
      Object::ToObject(isolate_, receiver_).ToHandleChecked();
  if (!receiver->IsJSObject()) {
    return isolate_->factory()->null_value();
  }

  Handle<JSObject> obj = Handle<JSObject>::cast(receiver);
  Handle<Object> function_name(fun_->shared()->name(), isolate_);
  if (function_name->IsString()) {
    Handle<String> name = Handle<String>::cast(function_name);
    // ES2015 gives getters and setters name prefixes which must
    // be stripped to find the property name.
    if (name->IsUtf8EqualTo(CStrVector("get "), true) ||
        name->IsUtf8EqualTo(CStrVector("set "), true)) {
      name = isolate_->factory()->NewProperSubString(name, 4, name->length());
    }
    if (CheckMethodName(isolate_, obj, name, fun_,
                        LookupIterator::PROTOTYPE_CHAIN_SKIP_INTERCEPTOR)) {
      return name;
    }
  }

  HandleScope outer_scope(isolate_);
  Handle<Object> result;
  for (PrototypeIterator iter(isolate_, obj, kStartAtReceiver); !iter.IsAtEnd();
       iter.Advance()) {
    Handle<Object> current = PrototypeIterator::GetCurrent(iter);
    if (!current->IsJSObject()) break;
    Handle<JSObject> current_obj = Handle<JSObject>::cast(current);
    if (current_obj->IsAccessCheckNeeded()) break;
    Handle<FixedArray> keys =
        KeyAccumulator::GetOwnEnumPropertyKeys(isolate_, current_obj);
    for (int i = 0; i < keys->length(); i++) {
      HandleScope inner_scope(isolate_);
      if (!keys->get(i)->IsName()) continue;
      Handle<Name> name_key(Name::cast(keys->get(i)), isolate_);
      if (!CheckMethodName(isolate_, current_obj, name_key, fun_,
                           LookupIterator::OWN_SKIP_INTERCEPTOR))
        continue;
      // Return null in case of duplicates to avoid confusion.
      if (!result.is_null()) return isolate_->factory()->null_value();
      result = inner_scope.CloseAndEscape(name_key);
    }
  }

  if (!result.is_null()) return outer_scope.CloseAndEscape(result);
  return isolate_->factory()->null_value();
}


int CallSite::GetLineNumber() {
  if (pos_ >= 0 && IsJavaScript()) {
    Handle<Object> script_obj(fun_->shared()->script(), isolate_);
    if (script_obj->IsScript()) {
      Handle<Script> script = Handle<Script>::cast(script_obj);
      return Script::GetLineNumber(script, pos_) + 1;
    }
  }
  return -1;
}


int CallSite::GetColumnNumber() {
  if (pos_ >= 0 && IsJavaScript()) {
    Handle<Object> script_obj(fun_->shared()->script(), isolate_);
    if (script_obj->IsScript()) {
      Handle<Script> script = Handle<Script>::cast(script_obj);
      return Script::GetColumnNumber(script, pos_) + 1;
    }
  }
  return -1;
}


bool CallSite::IsNative() {
  if (!IsJavaScript()) return false;
  Handle<Object> script(fun_->shared()->script(), isolate_);
  return script->IsScript() &&
         Handle<Script>::cast(script)->type() == Script::TYPE_NATIVE;
}


bool CallSite::IsToplevel() {
  if (IsWasm()) return false;
  return receiver_->IsJSGlobalProxy() || receiver_->IsNull(isolate_) ||
         receiver_->IsUndefined(isolate_);
}


bool CallSite::IsEval() {
  if (!IsJavaScript()) return false;
  Handle<Object> script(fun_->shared()->script(), isolate_);
  return script->IsScript() &&
         Handle<Script>::cast(script)->compilation_type() ==
             Script::COMPILATION_TYPE_EVAL;
}


bool CallSite::IsConstructor() {
  // Builtin exit frames mark constructors by passing a special symbol as the
  // receiver.
  Object* ctor_symbol = isolate_->heap()->call_site_constructor_symbol();
  if (*receiver_ == ctor_symbol) return true;
  if (!IsJavaScript() || !receiver_->IsJSObject()) return false;
  Handle<Object> constructor =
      JSReceiver::GetDataProperty(Handle<JSObject>::cast(receiver_),
                                  isolate_->factory()->constructor_string());
  return constructor.is_identical_to(fun_);
}

MaybeHandle<Object> FormatStackTrace(Isolate* isolate, Handle<JSObject> error,
                                     Handle<Object> stack_trace) {
  // TODO(jgruber): Port FormatStackTrace from JS.
  Handle<JSFunction> fun = isolate->error_format_stack_trace();

  int argc = 2;
  ScopedVector<Handle<Object>> argv(argc);
  argv[0] = error;
  argv[1] = stack_trace;

  Handle<Object> formatted_stack_trace;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, formatted_stack_trace,
      Execution::Call(isolate, fun, error, argc, argv.start()), Object);

  return formatted_stack_trace;
}

Handle<String> MessageTemplate::FormatMessage(Isolate* isolate,
                                              int template_index,
                                              Handle<Object> arg) {
  Factory* factory = isolate->factory();
  Handle<String> result_string;
  if (arg->IsString()) {
    result_string = Handle<String>::cast(arg);
  } else {
    Handle<JSFunction> fun = isolate->no_side_effects_to_string_fun();

    MaybeHandle<Object> maybe_result =
        Execution::TryCall(isolate, fun, factory->undefined_value(), 1, &arg);
    Handle<Object> result;
    if (!maybe_result.ToHandle(&result) || !result->IsString()) {
      return factory->InternalizeOneByteString(STATIC_CHAR_VECTOR("<error>"));
    }
    result_string = Handle<String>::cast(result);
  }
  MaybeHandle<String> maybe_result_string = MessageTemplate::FormatMessage(
      template_index, result_string, factory->empty_string(),
      factory->empty_string());
  if (!maybe_result_string.ToHandle(&result_string)) {
    return factory->InternalizeOneByteString(STATIC_CHAR_VECTOR("<error>"));
  }
  // A string that has been obtained from JS code in this way is
  // likely to be a complicated ConsString of some sort.  We flatten it
  // here to improve the efficiency of converting it to a C string and
  // other operations that are likely to take place (see GetLocalizedMessage
  // for example).
  return String::Flatten(result_string);
}


const char* MessageTemplate::TemplateString(int template_index) {
  switch (template_index) {
#define CASE(NAME, STRING) \
  case k##NAME:            \
    return STRING;
    MESSAGE_TEMPLATES(CASE)
#undef CASE
    case kLastMessage:
    default:
      return NULL;
  }
}


MaybeHandle<String> MessageTemplate::FormatMessage(int template_index,
                                                   Handle<String> arg0,
                                                   Handle<String> arg1,
                                                   Handle<String> arg2) {
  Isolate* isolate = arg0->GetIsolate();
  const char* template_string = TemplateString(template_index);
  if (template_string == NULL) {
    isolate->ThrowIllegalOperation();
    return MaybeHandle<String>();
  }

  IncrementalStringBuilder builder(isolate);

  unsigned int i = 0;
  Handle<String> args[] = {arg0, arg1, arg2};
  for (const char* c = template_string; *c != '\0'; c++) {
    if (*c == '%') {
      // %% results in verbatim %.
      if (*(c + 1) == '%') {
        c++;
        builder.AppendCharacter('%');
      } else {
        DCHECK(i < arraysize(args));
        Handle<String> arg = args[i++];
        builder.AppendString(arg);
      }
    } else {
      builder.AppendCharacter(*c);
    }
  }

  return builder.Finish();
}

MaybeHandle<Object> ConstructError(Isolate* isolate, Handle<JSFunction> target,
                                   Handle<Object> new_target,
                                   Handle<Object> message, FrameSkipMode mode,
                                   bool suppress_detailed_trace) {
  // 1. If NewTarget is undefined, let newTarget be the active function object,
  // else let newTarget be NewTarget.

  Handle<JSReceiver> new_target_recv =
      new_target->IsJSReceiver() ? Handle<JSReceiver>::cast(new_target)
                                 : Handle<JSReceiver>::cast(target);

  // 2. Let O be ? OrdinaryCreateFromConstructor(newTarget, "%ErrorPrototype%",
  //    « [[ErrorData]] »).
  Handle<JSObject> err;
  ASSIGN_RETURN_ON_EXCEPTION(isolate, err,
                             JSObject::New(target, new_target_recv), Object);

  // 3. If message is not undefined, then
  //  a. Let msg be ? ToString(message).
  //  b. Let msgDesc be the PropertyDescriptor{[[Value]]: msg, [[Writable]]:
  //     true, [[Enumerable]]: false, [[Configurable]]: true}.
  //  c. Perform ! DefinePropertyOrThrow(O, "message", msgDesc).
  // 4. Return O.

  if (!message->IsUndefined(isolate)) {
    Handle<String> msg_string;
    ASSIGN_RETURN_ON_EXCEPTION(isolate, msg_string,
                               Object::ToString(isolate, message), Object);
    RETURN_ON_EXCEPTION(isolate, JSObject::SetOwnPropertyIgnoreAttributes(
                                     err, isolate->factory()->message_string(),
                                     msg_string, DONT_ENUM),
                        Object);
  }

  // Optionally capture a more detailed stack trace for the message.
  if (!suppress_detailed_trace) {
    RETURN_ON_EXCEPTION(isolate, isolate->CaptureAndSetDetailedStackTrace(err),
                        Object);
  }

  // When we're passed a JSFunction as new target, we can skip frames until that
  // specific function is seen instead of unconditionally skipping the first
  // frame.
  Handle<Object> caller;
  if (mode == SKIP_FIRST && new_target->IsJSFunction()) {
    mode = SKIP_UNTIL_SEEN;
    caller = new_target;
  }

  // Capture a simple stack trace for the stack property.
  RETURN_ON_EXCEPTION(isolate,
                      isolate->CaptureAndSetSimpleStackTrace(err, mode, caller),
                      Object);

  return err;
}

}  // namespace internal
}  // namespace v8
