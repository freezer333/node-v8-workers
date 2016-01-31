# How (not) to access V8 memory from a Node.js C++ addon's worker thread
When we create C++ addons for Node.js, we have two strategies - synchronous or asynchronous processing.  A synchronous C++ addon does it's number crunching in the thread that Node.js's event loop executes in.  This, unfortunately, blocks all JavaScript execution until the C++ addon completes it's work and returns control back to JavaScript.  In asynchronous addons, we can execute the heavy lifting in a worker thread.  This allows the initial call into the addon to return quickly to JavaScript - freeing up the Node.js event loop to continue it's business.  An asynchronous addon will invoke a JavaScript callback function when it has completed (or at least whenever it has something to return).  If you are unfamiliar with asynchronous callbacks - check out two of my previous blog posts on the subject:

1. [Building an Asynchronous C++ Addon for Node.js using Nan](http://blog.scottfrees.com/building-an-asynchronous-c-addon-for-node-js-using-nan)
2. [C++ processing from Node.js - Part 4 - Asynchronous addons](http://blog.scottfrees.com/c-processing-from-node-js-part-4-asynchronous-addons)

Both of those posts (and pretty much anything else you read on the web about async addons) make the following assertion - either implicitly or explicitly:  *you can't access V8 (JavaScript) memory outside the event-loop's thread*.  This essentially means if you want the *asynchronous* part of your addon to be able to (1) access input data sent from JavaScript and/or (2) return data to JavaScript then you need to create copies of the input/output data.  The mechanics work like this:

// image where input/output data is being copied.

For situations where input data and output data is relatively small, this poses absolutely no issue - and I'd argue these are probably the most common cases.  However - what if your async addon is going to do a lot of computation over a large input?  What if it generates a huge amount of data?  Note that in the figure above - copying input and output data is being done in the event loop - meaning if it takes a long time, we're blocking the event loop (which we hate doing!).  So we have two problems:

1. Copying data might be a waste of memory
2. Copying data might take long, which ties up the Node.js event loop.

Ideally, we'd prefer a way to do this:

// image where there is no data copied.

As mentioned above - this is something that the official Node.js and V8 documentation both specifically say we can't do... however I think it's instructive to see why (and documentation covering the "why" is hard to come by!).  The remainder of this post is about how I went about proving to myself it wouldn't work, and the ways I'd recommend mitigating the problems of copying within the event loop.

## Handles - Local and Persistent
How does C++ access JavaScript data in the first place?  It's all explained in the [V8 Embedder Guide](https://developers.google.com/v8/embed?hl=en) - but I'll breifly describe it here.  When your Node.js JavaScript code is executing and allocating variables, it's doing so within the V8 JavaScript engine.  V8 allocates JavaScript variables within it's own address space inside what we'll call "storage cells".  When JavaScript calls into a C++ addon, the C++ code may obtain references to these storage cells by creating *handles* using the V8 API.  The most common handle type is `Local` - meaning it's scope is tied specifically to the current *handle scope*.  The scope of a handle is critical, as V8 is charged with implementing garbage collection - and to do this it must keep track of how many references point to given storage cells.  These references are normally within JavaScript, but the handles in our C++ addons count too.

The most basic form of accessing pre-existing JavaScript variables occurs when we access arguments that have been passed into our C++ addon's when a method is invoked from JavaScript:

// image showing the handle.

As you can see from the diagram, the `Local<Object>` handle we create (`target`) will allow us to access V8 storage cells.  `Local` handles only remain valid while the `HandleScope` object active when they are created is in scope.  The [V8 Embedder's Guide](https://developers.google.com/v8/embed?hl=en) is once again the primary place to learn about `HandleScope` objects - however put simply, they are containers for handles.  At a given time, only one `HandleScope` is active within V8 (or more specifically, a V8 `Isolate`).  Where's the `HandleScope` in the above example?  Good question!  

It turns out that Node.js creates a `HandleScope` right before it calls our addon on the JavaScript code's behalf.  This `HandleScope` is destroyed when the C++ addon function returns.  Thus, any `Local` handle created inside our addon's function only survives until that function returns - meaning `Local` handles can never be accessed in worker threads when dealing with async addons - the worker threads clearly outlive the initial addon function call!

### Are `Persistent` handles the answer?
Maybe!  As the name implied, `Persistent` handles are not automatically destroyed using `HandleScope`s - they can hang around indefinitely.  Once you've created a persistent handle in your C++ code, V8 will honor that reference (and make sure you can still access the storage cells it points to) until you explicitly release it (you do this by calling the handle's `Reset` method).  At first glance, this appears to be precisely the tool that would allow a long-lived C++ worker thread to access V8 data.  

As a first experiment, we can prove out the idea that we can maintain a reference to a JavaScript variable across C++ function calls by setting up a simple (non-threaded) example.  Let's build a quick addon that allows JavaScript to pass in an object that the C++ hangs on to.  Subsequent calls to the addon will *mutate* the JavaScript object originally passed into it - and we'll see these changes in JavaScript after the C++ returns.

```c++
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
```

If you are unfamiliar with building addons, checkout the build tool (`node-gyp`) and the details of how to set it up can be found [here](http://blog.scottfrees.com/c-processing-from-node-js).  

Notice the two functions exposed by the addon - `Setup`, which sets up a (global) `Persistent` handle to the object passed in from JavaScript.  Of course, this is a pretty dubious use of global variables within an addon - it's probably a bad idea for non-trivial stuff - but this is just for demonstration.  The point is that `Persistent<Object> target`'s scope is **not** tied to `Setup`.

The second function exposed by the addon is `Mutate`, and it simply adds 42 to `target`s only property - `x`. Now let's look at the calling Node.js program.  

```js
const addon = require('./build/Release/mutate');

var obj = { 	x: 0  };

// save the target JS object in the addon
addon.setup(obj);
console.log(obj);  // should print 0

addon.mutate(obj);
console.log(obj);  // should print 42

addon.mutate(obj);
console.log(obj);  // should print 84
```

When we run this program we'll see `obj.x` is initially 0, then 42, and then 84 when printed out.  Living proof we can hang on to V8 within our addon across invocations... we're on to something!

## Bring on the worker threads!
Let's simulate a use case where a worker thread spends a long time modifying data iteratively.  We'll modify the addon from above such that instead of needing JavaScript to call `Mutate`, it repeatedly changes `target`'s x value every 500ms in a worker thread.

```
The threading

```

Note - we're not using libuv or any of the (best) common practice you'd normally see in an async addon; just ordinary C++ threads.  Let's update the JavaScript to print out `obj` each second so we can see the fabulous work our C++ thread is doing.

```
java script with setInterval
```

If you've tried this before, you know what's coming next!

```
run it... seg fault
```

Why are we getting a seg fault?  Well... it's because V8 doesn't allow what we're trying to do.  Specifically, a single V8 instance (represented by an `Isolate`) can never be accessed by two threads.  That is unless, of course, we use build in V8 synchronization facilities in the form of `v8::Locker`.  By using `v8::Locker`, we can prove to the V8 `isolate` that we are switching between threads - but that we never allow simultaneous access from multiple threads.  

### Understanding V8 Locker objects
Viewing V8 `isolate` as a shared resource, anyone familiar with thread synchronization can easily understand `v8::Locker` through the lens of a typical synchronization primitive in C++ - such as a mutex.  The `v8::Locker` object is a lock, and we use [RAII](https://en.wikipedia.org/wiki/Resource_Acquisition_Is_Initialization) to us it.  Specifically, the creation of a `v8::Locker` object (constructor) automatically blocks and waits until no other thread is within the `isolate`.  Destruction of the `v8::Locker` (when it goes out of scope) automatically releases the lock - allowing other threads to enter.

We have two threads:  (1)  the Node.js (libuv) event loop and (2) the worker thread.  Let's first look at adding locking to the worker:

```
Add locker to the while loop
```

At this point, since we haven't added locking anywhere else, you'd be forgiven if you think this would have very little effect if we run our program now.  After all, there is seemingly no contention on the V8 lock, since the the worker is the only thread trying to lock it.

```
Show that the worker never runs...
```

This is not what we see though... instead, we see that our worker thread *never acquires the lock!*.  This implies our event loop thread owns the `isolate`.  If your first instinct is to force the event loop thread to *release the lock* - I'll indulge.  The V8 API also has a `Unlocker` construct, which when created unlocks the isolates.  If we add this to the end of our `Setup` function (since it was called by the event loop) - perhaps this would help!

```
C++ unlock in Setup
```

Running this ends in disaster though.  

```
Seg fault
```

This really should not have been a surprise.  When `Setup` returns, it's relinquished the lock it apparently had on V8 - but this is the thread that actually runs our JavaScript - a clear conflict in logic!  The `seg fault` is a result of Node.js calling into V8 (to return control back to JavaScript) without having the `isolate`'s exclusive lock.

## Arriving at the dead end
The final nail in the coffin for this strategy is to actually track down how Node.js uses V8's locking mechanism.  Diving into the Node.js source code, we can locate the specific code which implements the libuv message pump loop.  

```
Node.js code
```

Notice that the loop starting on line XXX is basically just pulling an event off libuv's event queue and executing the appropriate JavaScript to process it (which would be a callback function in your Node.js program).  Right above that though, on line YYY, we see our familiar `Locker`.  Node.js acquires the `isolate` lock *before* beginning the main event loop that drives yoruo entire JavaScript program - *and it never relinquishes it*.  Ultimately - this where we must realize our goal of accessing V8 data from a worker thread is impossible with Node.js.  In other V8 programs, you might very well allow workflows like this, however Node.js specifically disallows multithreaded access by holding the `isolate`'s lock throughout the entire lifetime of your program.

## Why typical async addons work
If you've written async addons in C++ already, you surely have used `Persistent` handles to callbacks.  You've gone through the trouble to copy input data into plain old C++ variables, used the `libuv` or `Nan`'s wrappers to queue up a work object to be processed in a worker thread, and then copied the data back to V8 handles and invoked a callback.  Of course, if you haven't done this - read [this now](http://blog.scottfrees.com/c-processing-from-node-js-part-4-asynchronous-addons), so you understand what I'm talking about.  It's a complicated workflow - but hopefully the above discussion highlights why its so important.  Callback functions (passed in as arguments to our addon when it's invoked) must be held in `Persistent` handles, since we must access them when our work thread is completed (and well after the initial C++ function returns to JavaScript).  Note however that we *never* invoke that callback from the worker thread.  We invoke the callback (accessing the `Persistent` handle) when our C++ "completion" function (in Nan, our `HandleOKCallback`) is called by `libuv` - **in the event loop thread**.  Once you understand the threading rules, I think the elegance of the typical async solution pattern becomes a lot more clear.

## Actually avoiding copying data
Our premise was that we could have a C++ addon that used data *from JavaScript* as input.  Since we know making a copy would be necessary - we come back to the issue of copying *a lot* of data.  If you find yourself in this situation, there may be some very simple ways to mitigate the problem.  First - does your input data *really need* to originate from JavaScript?  If your addon is using a lot of data as input, where is that data *actually* coming from?  Is it coming from a database?  If so, the retrieve it in C++ - not JavaScript!  Is it coming from a file (or an upload)?  If so, can you avoid reading it into JavaScript variables and just pass the filename/location into C++?  

If these ideas are inadequate, and you are truly in a situation where code in JavaScript must have access to the same data your C++ addon will work on, there is another way to resolve the copying problem.  While we've shown that C++ threads can't access data V8 owns - no one has said JavaScript can't access data owned by C++.  Instead of sharing JavaScript variables (V8 storage cells) between JavaScript and C++ worker threads - could we share C++ memory (plain old C++, not within V8) between JavaScript and C++ workers?  The answer is **"sort of"**!

### Accessors
The V8 API provides a very important (and sometimes overlooked) mechanism for allowing JavaScript code to manipulate data held in C++ proper.   The concept is called


Actually - I think this might be better described using Node's ObjectWrap functionality.  Explain it breifly (maybe this is the next blog post).  The premise is that data is at rest within C++, not JavaScript.  You can access from JavaScript (albeit, each access does result in a copy of the specific data you are accessing), and you can certainly access from C++ worker thread.

## Conclusion
Wrap up the post by plugging the ebook again :)
