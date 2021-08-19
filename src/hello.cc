#include <node.h>
#include <pthread.h>
#include <iostream>
#include <unistd.h>
using namespace std;
using namespace v8;

Local<String> NewLatin1String(Isolate* isolate, const char* string) {
  return String::NewFromOneByte(isolate, (const uint8_t*)string, NewStringType::kNormal).ToLocalChecked();
}
#define THROW(x, m, a) return a.GetReturnValue().Set(Isolate::GetCurrent()->ThrowException(x(NewLatin1String(Isolate::GetCurrent(), m))))
namespace fiber {


pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

void* runInitFiber(void* _args);

class Fiber {
private:
  bool started = false;
  static size_t pool_size;

  // used to track both the value passed into run() and yield(), but also the values returned by these
  Persistent<Value> yielded;

  bool yielding = false;
  bool yielded_exception = false;
  bool resetting = false;
  bool zombie = false;
  pthread_t thread_id = 0;
  Fiber* entry_fiber;
  Persistent<Value> zombie_exception;
  static vector<Fiber*> orphaned_fibers;
  static Persistent<Value> fatal_stack;
  static Fiber* delete_me;

public:
  static Fiber* current;
  pthread_cond_t cond;
	Isolate* isolate;
  Persistent<Object> handle;
  Persistent<Function> cb;
  Persistent<Context> v8_context;

  static Persistent<FunctionTemplate> tmpl;
  static Persistent<Function> fiber_object;
  static void New(FunctionCallbackInfo<v8::Value> &args) {
		Local<Function> fn = Local<Function>::Cast(args[0]);
    if (!args.IsConstructCall()) {
      Local<Value> argv[1] = { args[0] };
      args.GetReturnValue().Set(
        Local<FunctionTemplate>::New(Isolate::GetCurrent(), tmpl)
        ->GetFunction(Isolate::GetCurrent()->GetCurrentContext()).ToLocalChecked()
        ->NewInstance(Isolate::GetCurrent()->GetCurrentContext(), 1, argv).ToLocalChecked()
      );
      return;
    }
		new Fiber(
      args.This(),
      fn,
      Isolate::GetCurrent()->GetCurrentContext()
    );
    return args.GetReturnValue().Set(args.This());
  }

	static Fiber& Unwrap(Local<Object> handle) {
		assert(!handle.IsEmpty());
		assert(handle->InternalFieldCount() == 1);
		return *static_cast<Fiber*>(handle->GetAlignedPointerFromInternalField(0));
	}

  static void mutex_unlock_wrapper (void *arg) {
    pthread_mutex_unlock ((pthread_mutex_t *)arg);
  }

  void SwapContext() {
    Unlocker unlocker(this->isolate);
    this->isolate->Exit();
    Fiber* last_fiber = current;
    current = this;
    pthread_cond_signal(&this->cond);
    pthread_cond_wait(&last_fiber->cond, &mutex);
    this->isolate->Enter();
    Locker locker(this->isolate);
    current = last_fiber;
  }

  static void Reset(FunctionCallbackInfo<v8::Value> &args) {
     Fiber& that = Unwrap(args.Holder());

     if (!that.started) {
       args.GetReturnValue().Set(Undefined(that.isolate));
       return;
     } else if (!that.yielding) {
       THROW(Exception::Error, "This Fiber is not yielding", args);
     } else if (args.Length()) {
       THROW(Exception::TypeError, "reset() expects no arguments", args);
     }

     that.resetting = true;
     that.UnwindStack();
     that.resetting = false;
     that.MakeWeak();

     Local<Value> val = Local<Value>::New(that.isolate, that.yielded);
     that.yielded.Reset();
     if (that.yielded_exception) {
       return args.GetReturnValue().Set(that.isolate->ThrowException(val));
     } else {
       return args.GetReturnValue().Set(val);
     }
  }

  static void* StartThread(void* _args) {
    pthread_mutex_lock(&mutex);
    pthread_cleanup_push (mutex_unlock_wrapper, &mutex);
      Fiber* fiber = (Fiber*)_args;
      Locker locker(fiber->isolate);
      Isolate::Scope isolate_scope(fiber->isolate);
      v8::HandleScope scope(fiber->isolate);
      Local<Function> func = Local<Function>::New(fiber->isolate, fiber->cb);
      Local<Context> context = Local<Context>::New(fiber->isolate, fiber->v8_context);
      TryCatch try_catch(fiber->isolate);
      fiber->ClearWeak();
      context->Enter();
			Local<Value> yielded;
      if (fiber->yielded.IsEmpty()) {
        func->Call(fiber->isolate->GetCurrentContext(), context->Global(), 0, NULL).ToLocal(&yielded);
      }
      else {
        Local<Value> runWith = Local<Value>::New(fiber->isolate, fiber->yielded);
  			Local<Value> argv[1] = { runWith };
        func->Call(fiber->isolate->GetCurrentContext(), context->Global(), 1, argv).ToLocal(&yielded);
      }

      if (try_catch.HasCaught()) {
        fiber->yielded.Reset(fiber->isolate, try_catch.Exception());
        fiber->yielded_exception = true;
        if (fiber->zombie && !fiber->resetting && !Local<Value>::New(fiber->isolate, fiber->yielded)->StrictEquals(Local<Value>::New(fiber->isolate, fiber->zombie_exception))) {
          // Throwing an exception from a garbage sweep
          fatal_stack.Reset(fiber->isolate, try_catch.StackTrace(context).ToLocalChecked());
        }
      } else {
        fiber->yielded.Reset(fiber->isolate, yielded);
        fiber->yielded_exception = false;
      }
      // Don't make weak until after notifying the garbage collector. Otherwise it may try and
      // free this very fiber!
      if (!fiber->zombie) {
        fiber->MakeWeak();
      }
      context->Exit();
      fiber->started = false;
      fiber->isolate->DiscardThreadSpecificMetadata();

      pthread_cond_signal(&fiber->entry_fiber->cond);
    pthread_cleanup_pop (1);
    return NULL;
  }

  static void Yield(FunctionCallbackInfo<v8::Value> &args) {
    if (current == NULL || !current->thread_id) {
      THROW(Exception::Error, "yield() called with no fiber running", args);
    }
    Fiber& that = *current;
    if (that.zombie) {
      args.GetReturnValue().Set(that.isolate->ThrowException(Local<Value>::New(that.isolate, that.zombie_exception)));
      return;
    }

    if (args.Length()) {
      that.yielded.Reset(that.isolate, (args)[0]);
    }
    else {
      that.yielded.Reset(that.isolate, v8::Undefined(that.isolate));
    }
    that.yielded_exception = false;
    that.MakeWeak();
    that.yielding = true;
    that.entry_fiber->SwapContext();
    that.yielding = false;
    that.ClearWeak();
    Local<Value> yielded = Local<Value>::New(that.isolate, that.yielded);
    if (that.yielded_exception) {
      args.GetReturnValue().Set(that.isolate->ThrowException(yielded));
    }
    else {
      args.GetReturnValue().Set(yielded);
    }
  }

  static void Run(FunctionCallbackInfo<v8::Value> &args) {
    Fiber& that = Unwrap(args.Holder());
		// There seems to be no better place to put this check..
		DestroyOrphans();

    if (that.started && !that.yielding) {
      THROW(Exception::Error, "This Fiber is already running", args);
    } else if (args.Length() > 1) {
      THROW(Exception::TypeError, "run() excepts 1 or no arguments", args);
    }
    that.entry_fiber = current;
    if (!that.started) {
      that.started = true;
      pthread_cond_init(&that.cond, 0);
      pthread_attr_t attr;
      pthread_attr_init (&attr);
      pthread_attr_setscope(&attr, PTHREAD_SCOPE_PROCESS);
      pthread_create(&that.thread_id, &attr, StartThread, (void*)&that);
    }
    if (args.Length()) {
      that.yielded.Reset(that.isolate, (args)[0]);
    }
    else {
      that.yielded.Reset(that.isolate, v8::Undefined(that.isolate));
    }
    that.SwapContext();
    Local<Value> yielded = Local<Value>::New(that.isolate, that.yielded);
    if (that.yielded_exception) {
      args.GetReturnValue().Set(that.isolate->ThrowException(yielded));
    }
    else {
      args.GetReturnValue().Set(yielded);
    }
  }

  static void WeakCallbackShim(const WeakCallbackInfo<Fiber>& data) {
    WeakCallback(data.GetParameter());
  }
  /**
   * Called when there are no more references to this object in Javascript. If this happens and
   * the fiber is currently suspended we'll unwind the fiber's stack by throwing exceptions in
   * order to clear all references.
   */
  static void WeakCallback(Fiber* data) {
    Fiber& that = *static_cast<Fiber*>(data);
    assert(current != &that);

    // We'll unwind running fibers later... doing it from the garbage collector is bad news.
    if (that.started) {
      assert(that.yielding);
      orphaned_fibers.push_back(&that);
      that.ClearWeak();
      return;
    }

    delete &that;
  }

  void MakeWeak() {
    handle.SetWeak(this, WeakCallbackShim, WeakCallbackType::kFinalizer);
  }

  void ClearWeak() {
    handle.ClearWeak();
  }

  Fiber() :
  isolate(Isolate::GetCurrent()),
  started(false),
  yielding(false) {
    pthread_cond_init(&this->cond, 0);
  }

  Fiber(Local<Object> handle, Local<Function> cb, Local<Context> v8_context) :
    isolate(Isolate::GetCurrent()),
    started(false),
    yielding(false)  {
      this->handle.Reset(isolate, handle);
      this->cb.Reset(isolate, cb);
      this->v8_context.Reset(isolate, v8_context);
  		MakeWeak();
      handle->SetAlignedPointerInInternalField(0, this);
  }

  virtual ~Fiber() {
    assert(!this->started);
    handle.Reset();
    cb.Reset();
    v8_context.Reset();
  }


  /**
   * Turns the fiber into a zombie and unwinds its whole stack.
   *
   * After calling this function you must either destroy this fiber or call MakeWeak() or it will
   * be leaked.
   */
  void UnwindStack() {
    assert(!zombie);
    assert(started);
    assert(yielding);
    zombie = true;

    // Setup an exception which will be thrown and rethrown from Fiber::Yield()
    Local<Value> zombie_exception = Exception::Error(NewLatin1String(isolate, "This Fiber is a zombie"));
    this->zombie_exception.Reset(isolate, zombie_exception);
    yielded.Reset(isolate, zombie_exception);
    yielded_exception = true;

    // Swap context back to Fiber::Yield() which will throw an exception to unwind the stack.
    // Futher calls to yield from this fiber will rethrow the same exception.
    SwapContext();
    assert(!started);
    zombie = false;

    // Make sure this is the exception we threw
    if (yielded_exception && yielded == zombie_exception) {
      yielded_exception = false;
      yielded.Reset();
      yielded.Reset(isolate, v8::Undefined(this->isolate));
    }
    this->zombie_exception.Reset();
  }


  // should this be exposed? Maybe not - but I dont see why it shouldn't be exposed, and it helps with testing
  // (e.g., when two versions of the fiber library are spewing threads)
  static void _DestroyOrphans(FunctionCallbackInfo<v8::Value> &args) {
    DestroyOrphans();
  }
  /**
   * When the v8 garbage collector notifies us about dying fibers instead of unwindng their
   * stack as soon as possible we put them aside to unwind later. Unwinding from the garbage
   * collector leads to exponential time garbage collections if there are many orphaned Fibers,
   * there's also the possibility of running out of stack space. It's generally bad news.
   *
   * So instead we have this function to clean up all the fibers after the garbage collection
   * has finished.
   */
  static void DestroyOrphans() {
    if (orphaned_fibers.empty()) {
      return;
    }
    vector<Fiber*> orphans(orphaned_fibers);
    orphaned_fibers.clear();

    for (vector<Fiber*>::iterator ii = orphans.begin(); ii != orphans.end(); ++ii) {
      Fiber& that = **ii;
      that.UnwindStack();

      if (that.yielded_exception) {
        // If you throw an exception from a fiber that's being garbage collected there's no way
        // to bubble that exception up to the application.
        auto stack(Local<Value>::New(that.isolate, fatal_stack));
        cerr <<
          "An exception was thrown from a Fiber which was being garbage collected. This error "
          "can not be gracefully recovered from. The only acceptable behavior is to terminate "
          "this application. The exception appears below:\n\n"
          <<*stack <<"\n";
        exit(1);
      } else {
        fatal_stack.Reset();
      }
      that.yielded.Reset();
      that.MakeWeak();
    }
  }

  /**
   * Getters for `started`, and `current`.
   */
  static void GetStarted(Local<String> property, const PropertyCallbackInfo<Value> &info) {
    if (info.This().IsEmpty() || info.This()->InternalFieldCount() != 1) {
      info.GetReturnValue().Set(v8::Undefined(Isolate::GetCurrent()));
      return;
    }
    Fiber& that = Unwrap(info.This());
    return info.GetReturnValue().Set(Boolean::New(that.isolate, that.started));
  }

  static void GetCurrent(Local<String> property, const PropertyCallbackInfo<Value> &info) {
    if (current) {
      info.GetReturnValue().Set(Local<Object>::New(Isolate::GetCurrent(), current->handle));
    } else {
      return info.GetReturnValue().Set(v8::Undefined(Isolate::GetCurrent()));
    }
  }

	/**
	 * Allow access to coroutine pool size
	 */
	static void GetPoolSize(Local<String> property, const PropertyCallbackInfo<Value>& info) {
		info.GetReturnValue().Set(Number::New(Isolate::GetCurrent(), pool_size));
	}

	static void SetPoolSize(Local<String> property, Local<Value> value, const PropertyCallbackInfo<void>& info) {
		pool_size = value->ToNumber(Isolate::GetCurrent()->GetCurrentContext()).ToLocalChecked()->Value();
	}

  static void Init(Local<Object> exports) {
  	Isolate* isolate = Isolate::GetCurrent();
    Local<Context> context = isolate->GetCurrentContext();

    current = new Fiber();

    Local<FunctionTemplate> localTmpl = FunctionTemplate::New(
      isolate,
      (FunctionCallback)Fiber::New,
      Local<Value>(),
      Local<Signature>(),
      0
    );
    tmpl.Reset(isolate, localTmpl);

    localTmpl->SetClassName(NewLatin1String(isolate, "Fiber"));

    Local<Signature> sig = Signature::New(isolate, localTmpl);
    localTmpl->InstanceTemplate()->SetInternalFieldCount(1);

    Local<ObjectTemplate> proto = localTmpl->PrototypeTemplate();
    proto->Set(
      NewLatin1String(isolate, "run"),
      FunctionTemplate::New(isolate, (FunctionCallback)Run, Local<Value>(), sig)
    );
    proto->Set(
      NewLatin1String(isolate, "reset"),
      FunctionTemplate::New(isolate, (FunctionCallback)Reset, Local<Value>(), sig)
    );
    proto->SetAccessor(
      NewLatin1String(isolate, "started"),
      (AccessorNameGetterCallback)GetStarted
    );

    Local<Function> yield = FunctionTemplate::New(
      isolate,
      (FunctionCallback)Yield
    )->GetFunction(isolate->GetCurrentContext()).ToLocalChecked();

    Local<Function> destroyOrphans = FunctionTemplate::New(
      isolate,
      (FunctionCallback)_DestroyOrphans
    )->GetFunction(isolate->GetCurrentContext()).ToLocalChecked();

    Local<Function> fn = localTmpl->GetFunction(isolate->GetCurrentContext()).ToLocalChecked();
    exports->Set(
      context,
      NewLatin1String(isolate, "Fiber"),
      fn
    ).FromJust();
    fn->SetAccessor(context, NewLatin1String(isolate, "current"), (AccessorNameGetterCallback)GetCurrent).ToChecked();
    fn->SetAccessor(context, NewLatin1String(isolate, "poolSize"), (AccessorNameGetterCallback)GetPoolSize, (AccessorNameSetterCallback)SetPoolSize);
    fn->Set(context, NewLatin1String(isolate, "yield"), yield).FromJust();
    fn->Set(context, NewLatin1String(isolate, "destroyOrphans"), destroyOrphans).FromJust();
    fiber_object.Reset(isolate, fn);
  }
};
Persistent<FunctionTemplate> Fiber::tmpl;
Persistent<Function> Fiber::fiber_object;
Fiber* Fiber::current;
Fiber* Fiber::delete_me;
vector<Fiber*> Fiber::orphaned_fibers;
Persistent<Value> Fiber::fatal_stack;
size_t Fiber::pool_size = 120;

void Initialize(Local<Object> exports) {
  pthread_mutex_lock (&mutex);
  Fiber::Init(exports);
}

NODE_MODULE(NODE_GYP_MODULE_NAME, Initialize)

}
