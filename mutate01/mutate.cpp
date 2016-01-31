#include <node.h>
using namespace v8;

// Stays in scope the entire time the addon is loaded.
Persistent<Object> persist;

void Mutate(const FunctionCallbackInfo<Value>& args) {
  Isolate * isolate = args.GetIsolate();
  Local<Object> target = Local<Object>::New(isolate, persist);

  Local<String> key = String::NewFromUtf8(isolate, "x");
  // pull the current value of prop x out of the object
  double current = target->ToObject()->Get(key)->NumberValue();
  // increment prop x by 42
  target->Set(key, Number::New(isolate,  current + 42));
}

void Setup(const FunctionCallbackInfo<Value>& args) {
	Isolate * isolate = args.GetIsolate();
  // Save a persistent handle to this object for later use in Mutate
	persist.Reset(isolate, args[0]->ToObject());
}

void init(Local<Object> exports) {
  NODE_SET_METHOD(exports, "setup", Setup);
  NODE_SET_METHOD(exports, "mutate", Mutate);
}

NODE_MODULE(mutate, init)
