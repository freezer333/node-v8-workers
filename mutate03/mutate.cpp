#include <node.h>
#include <chrono>
#include <thread>
#include <iostream>
using namespace v8;

// Stays in scope the entire time the addon is loaded.
Persistent<Object> persist;

void mutate(Isolate * isolate) {
	while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

		std::cerr << "Worker thread trying to enter isolate" << std::endl;
		v8::Locker locker(isolate);
		isolate->Enter();
		std::cerr << "Worker thread has entered isolate" << std::endl;
    // we need this to create local handles, since this
    // function is NOT called by Node.js
    v8::HandleScope handleScope(isolate);
    Local<String> key = String::NewFromUtf8(isolate, "x");
		Local<Object> target = Local<Object>::New(isolate, persist);
    double current = target->ToObject()->Get(key)->NumberValue();
		target->Set(key, Number::New(isolate,  current + 42));

		// Note, the locker will go out of scope here, so the thread
		// will leave the isolate (release the lock)
  }
}

void Start(const FunctionCallbackInfo<Value>& args) {
	Isolate * isolate = args.GetIsolate();
  persist.Reset(isolate, args[0]->ToObject());

  // spawn a new worker thread to modify the target object
  std::thread t(mutate, isolate);
	t.detach();
}


void init(Local<Object> exports) {
  NODE_SET_METHOD(exports, "start", Start);
}

NODE_MODULE(mutate, init)
