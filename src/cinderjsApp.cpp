#include "cinderjsApp.hpp"
#include "ArrayBufferAllocator.h"
#include "cinder/app/RendererGl.h"

#include <boost/bind.hpp>
#include <boost/thread/thread.hpp>

#include <string.h>
#include <fstream>
#include <sstream>
#include <cerrno>
#include <chrono>
#include <thread>

#ifdef __APPLE__
#include <OpenGL/OpenGL.h>
#endif

// Modules
#include "modules/app.hpp"
#include "modules/gl.hpp"
#include "modules/console.hpp"
#include "modules/text.hpp"
#include "modules/fs.hpp"
#include "modules/vm.hpp"
#include "modules/ray.hpp"
#include "modules/camera.hpp"
#include "modules/shader.hpp"
#include "modules/batch.hpp"
#include "modules/texture.hpp"
#include "modules/glm.hpp"
#include "modules/fbo.hpp"
#include "modules/vbo.hpp"
#include "modules/vao.hpp"
#include "modules/color.hpp"

#include <assert.h>

using namespace ci;
using namespace ci::app;
using namespace std;
using namespace v8;

namespace cjs {

#ifdef DEBUG
volatile bool CinderjsApp::_consoleActive = true;
volatile bool CinderjsApp::_v8StatsActive = true;
volatile bool CinderjsApp::_fpsActive = true;
#else
volatile bool CinderjsApp::_consoleActive = false;
volatile bool CinderjsApp::_v8StatsActive = false;
volatile bool CinderjsApp::_fpsActive = true;
#endif

bool CinderjsApp::sQuitRequested = false;
bool CinderAppBase::shutdownInProgress = false;

v8::Persistent<v8::Object> CinderjsApp::sModuleCache;
v8::Persistent<v8::Array> CinderjsApp::sModuleList;
  
v8::Persistent<v8::Function> CinderjsApp::sDrawCallback;
v8::Local<v8::Function> CinderjsApp::_fnDrawCallback;
v8::Persistent<v8::Function> CinderjsApp::sEventCallback;

v8::Persistent<v8::Object> CinderjsApp::sEmptyObject;

ConcurrentCircularBuffer<NextFrameFn> CinderjsApp::sExecutionQueue(1024);

Timer CinderjsApp::_mainTimer;
std::function<void(boost::any passOn)> CinderjsApp::_timerCallback;

int CinderjsApp::sGCRuns = 0;
void CinderjsApp::gcPrologueCb(Isolate *isolate, GCType type, GCCallbackFlags flags) {
  sGCRuns++;
  //AppConsole::log("GC Prologue " + std::to_string(sGCRuns));
  std::cout << "GC Prologue " << std::to_string(sGCRuns) << std::endl;
}

/**
 * Handling v8 errors in native land
 * TODO: can be vastly improved
 */
void CinderjsApp::handleV8TryCatch( v8::TryCatch &tryCatch, std::string info ) {
  std::string msg;
  if(tryCatch.StackTrace().IsEmpty()){
    v8::String::Utf8Value except(tryCatch.Exception());
    msg = *except;
  } else {
    v8::String::Utf8Value trace(tryCatch.StackTrace());
    msg = *trace;
  }
  std::string ex = "Uncaught Exception";
  ex.append(" (" + info + "):");
  ex.append(msg);
  #ifdef DEBUG
  std::cout << ex << std::endl;
  #endif
  AppConsole::log( ex );
}


/**
 * Cleanup
 */
void CinderjsApp::cleanup()
{
  shutdownInProgress = true;
  _consoleActive = false;
  mShouldQuit = true;
  
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  
  _eventRun = true;
  cvEventThread.notify_all();
  
  mEventQueue.cancel();
  sExecutionQueue.cancel();
  
  // Shutdown v8EventThread
  if( mV8EventThread ) {
    mV8EventThread->join();
    mV8EventThread.reset();
  }
  
  v8::V8::Dispose();
}

/**
 * Setup
 * If given a *.js file in the command line arguments, it will be run in the main context
 * and can be used to setup things for other scripts or to mockup the main app.
 */
void CinderjsApp::setup()
{
  // Init Console
  AppConsole::initialize();
  
  mLastEscPressed = getElapsedSeconds();
  
  // Command line arguments
  vector<std::string> args = getCommandLineArgs();
  

  mCwd = boost::filesystem::current_path();
  
  // Check argv arguments
  // TODO: Check for debug flag
  int pos = 0;
  for(std::vector<std::string>::iterator it = args.begin(); it != args.end(); ++it) {
    pos++;
  }
  
  // clear out the window with black
  gl::clear( Color( 0, 0, 0 ) );
  
  glRenderer = getRenderer();
  
  // Get cinder.js main native
  if(cinder_native && sizeof(cinder_native) > 0) {
    std::string mainJS(cinder_native);
    v8Setup(mainJS);
  } else {
    // FATAL: No main entry source
    quit();
  }
  
}

/**
 * Runs a JS string and prints eventual errors to the AppConsole
 */
v8::Local<v8::Value> CinderjsApp::executeScriptString( std::string scriptStr, Isolate* isolate, v8::Local<v8::Context> context, Handle<String> filename ){
  EscapableHandleScope scope(isolate);

  v8::TryCatch try_catch;
  
  // Enter the context for compiling and running
  Context::Scope context_scope(context);
  
  // Create a string containing the JavaScript source code.
  Local<String> source = String::NewFromUtf8( isolate, scriptStr.c_str() );
  
  // Compile the source code.
  Local<Script> script = Script::Compile( source, filename );
  
  // Run the script to get the result.
  Local<Value> result = script->Run();
  
  // Check for script errors
  if(try_catch.HasCaught()){
    String::Utf8Value str(filename);
    handleV8TryCatch(try_catch, "executeScriptString/" + std::string(*str));
  }
  
  return scope.Escape(result);
}

/**
 * Setup V8
 */
void CinderjsApp::v8Setup( std::string mainJS ){
  
  // Initialize V8 (implicit initialization was removed in an earlier revision)
  v8::V8::InitializeICU();
  v8::Platform* platform = v8::platform::CreateDefaultPlatform(4);
  v8::V8::InitializePlatform(platform);
  V8::Initialize();
  
  // Create a new Isolate and make it the current one.
  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = &ArrayBufferAllocator::the_singleton;
  mIsolate = Isolate::New(create_params);
  v8::Locker lock(mIsolate);
  Isolate::Scope isolate_scope(mIsolate);
  
  //mIsolate->AddGCPrologueCallback(gcPrologueCb);
  
  // Setup timer callback
  _timerCallback = [=](boost::any passOn){
    NextFrameFn next = boost::any_cast<NextFrameFn>(passOn);
    sExecutionQueue.pushFront(next);
  };
  
  // Create a stack-allocated handle scope.
  HandleScope handle_scope(mIsolate);
  
  // Set general globals for JS
  mGlobal = ObjectTemplate::New();
  mGlobal->Set(v8::String::NewFromUtf8(mIsolate, "__draw__"), v8::FunctionTemplate::New(mIsolate, setDrawCallback));
  mGlobal->Set(v8::String::NewFromUtf8(mIsolate, "__event__"), v8::FunctionTemplate::New(mIsolate, setEventCallback));
  mGlobal->Set(v8::String::NewFromUtf8(mIsolate, "toggleAppConsole"), v8::FunctionTemplate::New(mIsolate, toggleAppConsole));
  mGlobal->Set(v8::String::NewFromUtf8(mIsolate, "toggleV8Stats"), v8::FunctionTemplate::New(mIsolate, toggleV8Stats));
  mGlobal->Set(v8::String::NewFromUtf8(mIsolate, "quit"), v8::FunctionTemplate::New(mIsolate, requestQuit));
  
  // Timer
  mGlobal->Set(v8::String::NewFromUtf8(mIsolate, "setTimer"), v8::FunctionTemplate::New(mIsolate, setTimer));
  
  // Setup process object
  v8::Local<v8::ObjectTemplate> processObj = ObjectTemplate::New();
  processObj->Set(v8::String::NewFromUtf8(mIsolate, "nextFrame"), v8::FunctionTemplate::New(mIsolate, nextFrameJS));
  processObj->Set(v8::String::NewFromUtf8(mIsolate, "nativeBinding"), v8::FunctionTemplate::New(mIsolate, NativeBinding));
  // TODO: export process.platform
  // TODO: export process.noDeprecation
  // TODO: export process.throwDeprecation
  // TODO: export process.traceDeprecation
  // TODO: export process.pid
  // TODO: export process.env (environment vars etc.)
  //       -> Empty object for now (placeholer)
  // TODO:
  processObj->Set(v8::String::NewFromUtf8(mIsolate, "env"), ObjectTemplate::New());
  //process.execPath
  processObj->Set(v8::String::NewFromUtf8(mIsolate, "execPath"), v8::String::NewFromUtf8(mIsolate, getAppPath().c_str()));
  processObj->Set(v8::String::NewFromUtf8(mIsolate, "cwd"), v8::String::NewFromUtf8(mIsolate, mCwd.c_str()));
  mGlobal->Set(v8::String::NewFromUtf8(mIsolate, "process"), processObj);
  
  // TODO: export console[log, error, trace, etc.]
  
  //
  // Load Modules
  addModule(std::shared_ptr<AppModule>( new AppModule() ));
  addModule(std::shared_ptr<GLModule>( new GLModule() ));
  addModule(std::shared_ptr<ConsoleModule>( new ConsoleModule() ));
  addModule(std::shared_ptr<TextModule>( new TextModule() ));
  addModule(std::shared_ptr<FSModule>( new FSModule() ));
  addModule(std::shared_ptr<VMModule>( new VMModule() ));
  addModule(std::shared_ptr<RayModule>( new RayModule() ));
  addModule(std::shared_ptr<CameraModule>( new CameraModule() ));
  addModule(std::shared_ptr<ShaderModule>( new ShaderModule() ));
  addModule(std::shared_ptr<BatchModule>( new BatchModule() ));
  addModule(std::shared_ptr<TextureModule>( new TextureModule() ));
  addModule(std::shared_ptr<GlmModule>( new GlmModule() ));
  addModule(std::shared_ptr<FBOModule>( new FBOModule() ));
  addModule(std::shared_ptr<VBOModule>( new VBOModule() ));
  addModule(std::shared_ptr<VAOModule>( new VAOModule() ));
  addModule(std::shared_ptr<ColorModule>( new ColorModule() ));
  
  
  // Create a new context.
  v8::Local<v8::Context> mMainContext = Context::New(mIsolate, NULL, mGlobal);
  Context::Scope scope(mMainContext);
  pContext.Reset(mIsolate, mMainContext);
  
  // Get process object instance and set additional values
  Local<Value> processObjInstance = mMainContext->Global()->Get(v8::String::NewFromUtf8(mIsolate, "process"));
  
  // Initialize module list and set on process
  Local<Array> list = Array::New(mIsolate);
  sModuleList.Reset(mIsolate, list);
  processObjInstance.As<Object>()->Set(v8::String::NewFromUtf8(mIsolate, "modules"), list);
  
  // Initialize module cache
  Local<Object> obj = Object::New(mIsolate);
  sModuleCache.Reset(mIsolate, obj);
  processObjInstance.As<Object>()->Set(v8::String::NewFromUtf8(mIsolate, "_moduleCache"), obj);
  
  // add process.argv
  vector<std::string> argv = getCommandLineArgs();
  
  #ifdef DEBUG
  // For development loading...
  //argv.push_back("/Users/sebastian/Dropbox/+Projects/cinderjs/examples/test.js");
  //argv.push_back("/Users/sebastian/Dropbox/+Projects/cinderjs/examples/particle.js");
  //argv.push_back("/Users/sebastian/Dropbox/+Projects/cinderjs/examples/lines.js");
  //argv.push_back("/Users/sebastian/Dropbox/+Projects/cinderjs/examples/cube/cubes.js");
  //argv.push_back("/Users/sebastian/Dropbox/+Projects/cinderjs/examples/geometry_shader/index.js");
  //argv.push_back("/Users/sebastian/Dropbox/+Projects/cinderjs/examples/ParticleSphereGPU/index.js");
  //argv.push_back("/Users/sebastian/Dropbox/+Projects/cinderjs/examples/physics.js");
  //argv.push_back("/Users/sebastian/Dropbox/+Projects/cinderjs/examples/ray.js");
  //argv.push_back("/Users/sebastian/Dropbox/+Projects/cinderjs/examples/fbo_basic.js");
  //argv.push_back("/Users/sebastian/Dropbox/+Projects/cinderjs/test/weak_callback.js");
  //argv.push_back("/Users/sebastian/Dropbox/+Projects/cinderjs/test/lib/jasmine/runner.js");
  #endif
  
  // Forward command line arguments to process object, available from js context
  Local<Array> argvArr = Array::New(mIsolate);
  for (int i = 0; i < argv.size(); ++i) {
    argvArr->Set(i, String::NewFromUtf8(mIsolate, argv[i].c_str()));
  }
  processObjInstance.As<Object>()->Set(v8::String::NewFromUtf8(mIsolate, "argv"), argvArr);
  
  // Setup global empty object for function callbacks
  Local<ObjectTemplate> emptyObj = ObjectTemplate::New();
  Handle<Object> emptyObjInstance = emptyObj->NewInstance();
  sEmptyObject.Reset(mIsolate, emptyObjInstance);
  
  // Grab GL context to execute the entry script and allow gl setup
  // @todo: deprecated as setup has context to draw with glNext already?
  glRenderer->startDraw();
  
  // Execute entry script
  Local<Value> mainResult = executeScriptString( mainJS, mIsolate,
    mMainContext, v8::String::NewFromUtf8(mIsolate, "cinder.js") );
  
  // Call wrapper function with process object
  if(mainResult->IsFunction()){
    Local<Function> mainFn = mainResult.As<Function>();
    
    v8::TryCatch tryCatch;
    
    mainFn->Call(mMainContext->Global(), 1, &processObjInstance);
    
    if(tryCatch.HasCaught()){
      handleV8TryCatch(tryCatch, "mainFn");
    }
    
    // TODO: Get Global()->global object and use in new context for external js modules
    
  } else {
    // FATAL: Main script not a function
    quit();
  }
  
  // Get rid of gl context again
  // @todo: deprecated as setup has context to draw with glNext already?
  glRenderer->finishDraw();
  
  gl::ContextRef backgroundCtx = gl::Context::create( gl::context() );
  
  // Start sub threads
  mV8EventThread = make_shared<std::thread>( boost::bind( &CinderjsApp::v8EventThread, this, backgroundCtx ) );
}

/**
 *
 */
v8::Handle<v8::Value> drawCallbackArgs[3];
void CinderjsApp::v8Draw(){
  
  // Gather some info...
  double now = getElapsedSeconds() * 1000;
  double timePassed = now - lastFrameTime;
  lastFrameTime = now;
  double elapsed = now - mLastUpdate;
  if(elapsed > mUpdateInterval) {
    v8FPS = v8Frames;
    v8Frames = 0;
    mLastUpdate = now;
    if(_v8StatsActive) mIsolate->GetHeapStatistics(&_mHeapStats);
  }

  v8::Locker lock(mIsolate);
  
  // JS Draw callback
  
  // Isolate
  v8::Isolate::Scope isolate_scope(mIsolate);
  v8::HandleScope handleScope(mIsolate);
  
  // Callback
  _fnDrawCallback = v8::Local<v8::Function>::New(mIsolate, sDrawCallback);
  
  v8::TryCatch try_catch;
    
  gl::pushMatrices();

  // Handle execution queue
  // TODO: Use a buffer queue and reuse event/frame buffer "packets" instead of instantiation at each creation time
  while(sExecutionQueue.isNotEmpty()){
    NextFrameFn nffn(new NextFrameFnHolder());
    sExecutionQueue.popBack(&nffn);
    v8::Local<v8::Function> callback = v8::Local<v8::Function>::New(mIsolate, nffn->v8Fn);
    if(!nffn->repeat){
      nffn->v8Fn.Reset(); // Get rid of persistent
    }
    v8::Handle<v8::Value> exArgv[0] = {};
    callback->Call(callback->CreationContext()->Global(), 0, exArgv);
  }

  if( !_fnDrawCallback.IsEmpty() ){

    drawCallbackArgs[0] = v8::Number::New(mIsolate, timePassed);
    drawCallbackArgs[1] = v8::Number::New(mIsolate, mousePosBuf.x);
    drawCallbackArgs[2] = v8::Number::New(mIsolate, mousePosBuf.y);
    
    _fnDrawCallback->Call(_fnDrawCallback->CreationContext()->Global(), 3, drawCallbackArgs);
    
    // TODO: Check if an FBO buffer was bound and not unbound.
    //       -> Unbind it to show console correctly and show eventual errors.
    //       -> Same goes for pushed matrices which were not popped after an error
  }
  
  gl::popMatrices();
    
  // Check for errors
  if(try_catch.HasCaught()){
    handleV8TryCatch(try_catch, "v8Draw");
  }
  
  v8::Unlocker unlock(mIsolate);
  
  // FPS (TODO: if active)
  cinder::TextLayout fpsText;
  fpsText.setColor( cinder::ColorA( 1, 1, 1, 1 ) );
  
  if(_fpsActive){
    fpsText.addLine( "Ci FPS: " + std::to_string( cinder::app::App::getAverageFps() ) );
    fpsText.addLine( "V8 FPS: " + std::to_string( v8FPS ) );
  }
  
  if(_v8StatsActive){
    fpsText.addLine( "V8 Heap limit: " + std::to_string( _mHeapStats.heap_size_limit() ) );
    fpsText.addLine( "V8 Heap total: " + std::to_string( _mHeapStats.total_heap_size() ) );
    fpsText.addLine( "V8 Heap Used: " + std::to_string( _mHeapStats.used_heap_size() ) );
  }
  
  if(_fpsActive || _v8StatsActive) {
    try {
      cinder::gl::draw( cinder::gl::Texture::create( fpsText.render() ) );
    } catch ( std::exception &e ){
      // don't draw if window not available
    }
  }
  
  // Draw console (if active)
  if(_consoleActive){
    vec2 cPos;
    // TODO: this still fails sometimes on shutdown
    try {
      cPos.y = getWindowHeight();
      AppConsole::draw( cPos );
    } catch ( std::exception &e ){
      // don't draw console if window is not available
    }
  }
  
  v8Frames++;
  
}

/**
 *
 */
void CinderjsApp::v8EventThread( gl::ContextRef context ){
  ThreadSetup threadSetup;
  context->makeCurrent();
  
  // TODO: Work around v8 blocking...
  
  // Thread loop
  while( !mShouldQuit ) {
    
    // Wait for data to be processed...
    {
        std::unique_lock<std::mutex> lck( mMainMutex );
        cvEventThread.wait(lck, [this]{ return _eventRun; });
    }
    
    if(!mShouldQuit && mEventQueue.isNotEmpty()){
      
      // TODO: If available, push mouse/key/resize events to v8
      v8::Locker lock(mIsolate);

      // Isolate
      v8::Isolate::Scope isolate_scope(mIsolate);
      v8::HandleScope handleScope(mIsolate);
      
      
      v8::Local<v8::Context> context = v8::Local<v8::Context>::New(mIsolate, pContext);
      
      if(context.IsEmpty()) return;
      
      Context::Scope ctxScope(context);
      context->Enter();
      
      
      // TODO: do not treat events further if shutdown was requested (quit from js)
      // TODO: Get rid of if/else statements
      while(mEventQueue.isNotEmpty()){
        BufferedEvent evt(new BufferedEventHolder());
        mEventQueue.popBack(&evt);
        
        // Callback
        v8::Local<v8::Function> callback = v8::Local<v8::Function>::New(mIsolate, sEventCallback);
        
        if(callback.IsEmpty()) continue;
        
        // Resize Event
        if(evt->type == CJS_RESIZE){
          v8::Handle<v8::Value> argv[3] = {
            v8::Number::New(mIsolate, CJS_RESIZE),
            v8::Number::New(mIsolate, getWindowWidth()),
            v8::Number::New(mIsolate, getWindowHeight())
          };
          callback->Call(context->Global(), 3, argv);
        }
        
        // Mouse down
        else if(evt->type == CJS_MOUSE_DOWN){
          v8::Handle<v8::Value> argv[1] = {
            v8::Number::New(mIsolate, CJS_MOUSE_DOWN)
            // ... TODO: Push more event info
          };
          callback->Call(context->Global(), 1, argv);
        }
        
        // Mouse up
        else if(evt->type == CJS_MOUSE_UP){
          v8::Handle<v8::Value> argv[1] = {
            v8::Number::New(mIsolate, CJS_MOUSE_UP)
            // ... TODO: Push more event info
          };
          callback->Call(context->Global(), 1, argv);
        }
        
        // Key down
        else if(evt->type == CJS_KEY_DOWN){
          v8::Handle<v8::Value> argv[3] = {
            v8::Number::New(mIsolate, CJS_KEY_DOWN),
            v8::Number::New(mIsolate, evt->kEvt.getCode()),
            v8::Number::New(mIsolate, evt->kEvt.getChar())
            // ... TODO: Push more event info
          };
          callback->Call(context->Global(), 3, argv);
        }
        
        // Key up
        else if(evt->type == CJS_KEY_UP){
          v8::Handle<v8::Value> argv[3] = {
            v8::Number::New(mIsolate, CJS_KEY_UP),
            v8::Number::New(mIsolate, evt->kEvt.getCode()),
            v8::Number::New(mIsolate, evt->kEvt.getChar())
            // ... TODO: Push more event info
          };
          callback->Call(context->Global(), 3, argv);
        }
        
        // File Drop
        else if(evt->type == CJS_FILE_DROP){
        
          Local<Array> files = Array::New(mIsolate);
          for(int i = 0; i < evt->fdFiles.size(); i++){
            files->Set(i, v8::String::NewFromUtf8(mIsolate, evt->fdFiles[i].c_str()));
          }
  
          v8::Handle<v8::Value> argv[2] = {
            v8::Number::New(mIsolate, CJS_FILE_DROP),
            files
          };
          callback->Call(context->Global(), 2, argv);
        }
        
        // Shutdown (gracefully)
        else if(evt->type == CJS_SHUTDOWN_REQUEST){
          quit();
        }
      }
      
      context->Exit();
      v8::Unlocker unlock(mIsolate);
    }
    
    _eventRun = false;
  }
  
  //@debug
  //std::cout << "V8 Event thread ending" << std::endl;
}


/**
 *
 */
void CinderjsApp::fileDrop( FileDropEvent event )
{
  BufferedEvent evt(new BufferedEventHolder());
  evt->type = CJS_FILE_DROP;
  evt->fdFiles = event.getFiles();
  mEventQueue.pushFront(evt);
  _eventRun = true;
  cvEventThread.notify_one();
}



/**
 *
 */
void CinderjsApp::resize()
{
  BufferedEvent evt(new BufferedEventHolder());
  evt->type = CJS_RESIZE;
  mEventQueue.pushFront(evt);
  _eventRun = true;
  cvEventThread.notify_one();
}

/**
 *
 */
void CinderjsApp::mouseMove( MouseEvent event )
{
  // Update mouse position (pushed to v8 with draw callback)
  mousePosBuf.x = event.getX();
  mousePosBuf.y = event.getY();
}

/**
 *
 */
void CinderjsApp::mouseDown( MouseEvent event )
{
  BufferedEvent evt(new BufferedEventHolder());
  evt->type = CJS_MOUSE_DOWN;
  evt->mEvt = event;
  mEventQueue.pushFront(evt);
  _eventRun = true;
  cvEventThread.notify_one();
}

/**
 *
 */
void CinderjsApp::mouseUp( MouseEvent event )
{
  BufferedEvent evt(new BufferedEventHolder());
  evt->type = CJS_MOUSE_UP;
  evt->mEvt = event;
  mEventQueue.pushFront(evt);
  _eventRun = true;
  cvEventThread.notify_one();
}

/**
 * Key down
 */
void CinderjsApp::keyDown( KeyEvent event )
{
  // Add default failsafe keys (ESC1x exit fullscreen, ESC2x exit app)
  if(getElapsedSeconds() - mLastEscPressed < 0.5){
    if(isFullScreen()){
      setFullScreen(false);
    } else {
      sQuitRequested = true;
    }
    return;
  }
  
  // Default Hotkeys
  if(event.getCode() == 27){
    mLastEscPressed = getElapsedSeconds();
  }
  // F1 - toggle fps text
  else if(event.getCode() == 282){
    _fpsActive = !_fpsActive;
  }
  // F2 - toggle v8 stats text
  else if(event.getCode() == 283){
    _v8StatsActive = !_v8StatsActive;
  }
  // F3 - toggle Console
  else if(event.getCode() == 284){
    _consoleActive = !_consoleActive;
  }
  
  
  BufferedEvent evt(new BufferedEventHolder());
  evt->type = CJS_KEY_DOWN;
  evt->kEvt = event;
  mEventQueue.pushFront(evt);
  _eventRun = true;
  cvEventThread.notify_one();
}

/**
 * Key up
 */
void CinderjsApp::keyUp( KeyEvent event )
{
  BufferedEvent evt(new BufferedEventHolder());
  evt->type = CJS_KEY_UP;
  evt->kEvt = event;
  mEventQueue.pushFront(evt);
  _eventRun = true;
  cvEventThread.notify_one();
}

/**
 *
 */
void CinderjsApp::update()
{
  
}

/**
 * Cinder draw loop (tick)
 * - Triggers v8 rendering
 */
void CinderjsApp::draw()
{
  // Quit if requested
  // Push to event queue and execute there (not check every frame, duh)
  if(sQuitRequested) {
    BufferedEvent evt(new BufferedEventHolder());
    evt->type = CJS_SHUTDOWN_REQUEST;
    mEventQueue.pushFront(evt);
    return;
  }

  v8Draw();
}
	

/**
 * Set the draw callback from javascript
 */
void CinderjsApp::setDrawCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::Locker lock(isolate);
  v8::HandleScope handleScope(isolate);
  
  if(!args[0]->IsFunction()){
    // throw js exception
    isolate->ThrowException(v8::String::NewFromUtf8(isolate, "draw callback expects one argument of type function."));
    return;
  }
  AppConsole::log("draw callback set.");
  
  sDrawCallback.Reset(isolate, args[0].As<v8::Function>());
  
  return;
}

/**
 * Set event callback from javascript to push mouse/key events to
 */
void CinderjsApp::setEventCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::Locker lock(isolate);
  v8::HandleScope handleScope(isolate);
  
  if(!args[0]->IsFunction()){
    // throw js exception
    isolate->ThrowException(v8::String::NewFromUtf8(isolate, "event callback expects one argument of type function."));
    return;
  }
  AppConsole::log("event callback set.");
  
  sEventCallback.Reset(isolate, args[0].As<v8::Function>());
  
  return;
}

/**
 * Toggle the AppConsole view on/off
 */
void CinderjsApp::toggleAppConsole(const v8::FunctionCallbackInfo<v8::Value>& args) {
  if(args.Length() > 0) {
    bool active = args[0]->ToBoolean()->Value();
    _consoleActive = active;
    return;
  }

  _consoleActive = !_consoleActive;
  return;
}

/**
 * Toggle v8 stats on/off
 */
void CinderjsApp::toggleV8Stats(const v8::FunctionCallbackInfo<v8::Value>& args) {
  _v8StatsActive = !_v8StatsActive;
  return;
}

/**
 * Toggle fps on/off
 */
void CinderjsApp::toggleFPS(const v8::FunctionCallbackInfo<v8::Value>& args) {
  _fpsActive = !_fpsActive;
  return;
}

/**
 * Allow JS to quit the App
 *
 * Tries to shutdown the application process gracefully,
 * meaning everything scheduled up until this point will still be executed,
 * shutdown will happen only after the scheduled tasks finish.
 *
 */
void CinderjsApp::requestQuit(const v8::FunctionCallbackInfo<v8::Value>& args) {
  // TODO: emit process exit event for js to react on and shutdown stuff if necessary
  //       Will at this point be pushed to event queue before the shutdown request,
  //       so it will be executed before shutdown.
  sQuitRequested = true;
  return;
}

/**
 * Async function execution in the next frame (executed in v8 event thread)
 *
 * Function callbacks scheduled for the next frame will be executed in a separate thread,
 * not the main render thread. The event thread they are executed in has a separate GL context,
 * with shared resources, so GL objects can be setup there and be used in the main render loop.
 * Drawing directly to the canvas however is not possible within the event thread.
 *
 */
void CinderjsApp::nextFrameJS(const v8::FunctionCallbackInfo<v8::Value>& args) {
  Isolate* isolate = args.GetIsolate();
  HandleScope scope(isolate);
  
  // TODO: check next frame execution queue and warn (fail) if full
  
  if(args[0]->IsFunction()){
    NextFrameFn nffn(new NextFrameFnHolder());
    nffn->v8Fn.Reset(isolate, args[0].As<Function>());
    sExecutionQueue.pushFront(nffn);
  }
}

/**
 * Set a timeout from JS
 * args: id, fn, timeout, repeat
 */
void CinderjsApp::setTimer(const v8::FunctionCallbackInfo<v8::Value>& args) {
  Isolate* isolate = args.GetIsolate();
  HandleScope scope(isolate);
  
  if(args[0]->IsFunction()){
    
    double timeout = args[1]->ToNumber()->Value();
    
    // If timeout is too little to be executed in time, run directly next frame;
    if(timeout <= 1){
      nextFrameJS(args);
      return;
    }
    
    NextFrameFn nffn(new NextFrameFnHolder());
    nffn->v8Fn.Reset(isolate, args[0].As<v8::Function>());
    nffn->repeat = args[2]->ToBoolean()->Value();
    
    uint32_t id = _mainTimer.set( timeout, _timerCallback, nffn->repeat, nffn );
    
    args.GetReturnValue().Set(id);
  }
  else if(args[0]->IsNumber()){
    _mainTimer.clear(args[0]->ToNumber()->Value());
  }

}

/**
 * Load a native js module (lib)
 */
void CinderjsApp::NativeBinding(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  HandleScope handle_scope(isolate);
  
  Locker lock(isolate);
  
  Local<String> moduleName = args[0]->ToString();
  
  if (moduleName.IsEmpty()) {
    std::string except = "NativeBinding needs a module name. fail.";
    isolate->ThrowException(String::NewFromUtf8(isolate, except.c_str()));
    return;
  }
  
  // if we have a second argument and it is a boolean true,
  // check for module existence only and return a boolean that can be checked with === operator
  bool checkExistence = false;
  if( args.Length() == 2 && args[1]->ToBoolean()->Value() ) {
    checkExistence = true;
  }
  
  Local<Object> cache = Local<Object>::New(isolate, sModuleCache);
  Local<Object> exports;
  Local<Object> moduleObj;

  if (!checkExistence && cache->Has(moduleName)) {
    exports = cache->Get(moduleName)->ToObject();
    args.GetReturnValue().Set(exports);
    return;
  }
  
  v8::String::Utf8Value strModuleName(moduleName);
  std::string cmpModName(*strModuleName);
  
  // Add to list
  std::string buf = "Native ";
  buf.append(*strModuleName);
  
  Local<Array> moduleList = Local<Array>::New(isolate, sModuleList);
  uint32_t len = moduleList->Length();
  moduleList->Set(len, String::NewFromUtf8(isolate, buf.c_str()));
  
  // Lookup native modules
  _native mod;
  bool found = false;
  for(int i = 0; i < sizeof(natives); i++) {
    if(natives[i].name == NULL) break;
    std::string cmp(natives[i].name);
    if(cmp == "cinder") continue;
    if(cmp == cmpModName){
      mod = natives[i];
      found = true;
      break;
    }
  }
  
  // If we only have to check existence, we return here
  if( checkExistence ) {
    args.GetReturnValue().Set(v8::Boolean::New(isolate, found));
    return;
  }
  
  if (found) {
  
    exports = Object::New(isolate);
    moduleObj = Object::New(isolate);
    
    v8::TryCatch tryCatch;
    
    // Use wrappers and wrap method set in cinder.js
    Local<Function> wrap = isolate
      ->GetCurrentContext()
      ->Global()
      ->Get(v8::String::NewFromUtf8(isolate, "process"))
      .As<Object>()
      ->Get(v8::String::NewFromUtf8(isolate, "wrap"))
      .As<Function>()
    ; // wtf v8...
    
    // Check v8 in sanity
    if(!wrap->IsFunction()) {
      std::string except = "Native module loading failed: No wrap method on the process object.";
      isolate->ThrowException(String::NewFromUtf8(isolate, except.c_str()));
      return;
    }
    
    // Try: v8::Local<Context>::New(isolate, pContext)
    std::shared_ptr<PipeModule> nativeMod = CinderjsApp::NAMED_MODULES[cmpModName];
    Local<Object> modObj = isolate->GetCurrentContext()->Global();
    if(nativeMod) {
      Local<ObjectTemplate> ctxGlb = ObjectTemplate::New(isolate);
      nativeMod->loadGlobalJS(ctxGlb);
      modObj = ctxGlb->NewInstance();
    }
    
    // Wrap it
    Local<Value> modSource = String::NewFromUtf8(isolate, mod.source);
    v8::String::Utf8Value wrappedSource( wrap->Call(Local<Object>::New(isolate, sEmptyObject), 1, &modSource) );
    std::string filename = cmpModName;
    filename.append(".js");
    Local<Value> modResult = executeScriptString(*wrappedSource, isolate, isolate->GetCurrentContext(),
      v8::String::NewFromUtf8(isolate, filename.c_str()) );
    
    // Check native module validity
    if(modResult.IsEmpty() || !modResult->IsFunction()){
      std::string except = "Native module not a function: ";
      except.append(*strModuleName);
    
      isolate->ThrowException(String::NewFromUtf8(isolate, except.c_str()));
      // TODO: remove from module list again
      return;
    }
    
    Local<Function> modFn = modResult.As<Function>();
    
    // Call
    Local<Value> argv[3] = {
      exports,
      // use this method as require as native modules won't require external ones(?)
      v8::Local<v8::Value>::Cast(v8::FunctionTemplate::New(isolate, NativeBinding)->GetFunction()),
      moduleObj
    };
    
    modFn->Call(modObj, 3, argv);
    
    if(tryCatch.HasCaught()){
      isolate->ThrowException( tryCatch.Exception() );
      //handleV8TryCatch(tryCatch);
      // TODO: remove from module list again
      return;
    }
    
    // check if module.exports exists,
    // if so, cache and return module.exports
    if( moduleObj->Has(v8::String::NewFromUtf8(isolate, "exports"))){
      exports = moduleObj->Get(v8::String::NewFromUtf8(isolate, "exports")).As<Object>();
    }
    
    cache->Set(moduleName, exports);
  } else {
    std::string except = "No such native module: ";
    except.append(*strModuleName);
  
    isolate->ThrowException(String::NewFromUtf8(isolate, except.c_str()));
    // TODO: remove from module list again
    return;
  }

  args.GetReturnValue().Set(exports);
}
  
} // namespace cjs

CINDER_APP( cjs::CinderjsApp, RendererGl )
