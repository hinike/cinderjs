#include "cinder/app/AppNative.h"
#include "cinder/gl/gl.h"
#include "cinder/Filesystem.h"

#include <boost/bind.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/thread/thread.hpp>

#include <string.h>
#include <fstream>
#include <sstream>
#include <cerrno>

#include "AppConsole.h"
#include "CinderAppBase.hpp"

#include "v8.h"
#include "libplatform/libplatform.h"

#ifdef __APPLE__
#include <OpenGL/OpenGL.h>
#endif

// Modules
#include "modules/app.hpp"
#include "modules/gl.hpp"
#include "modules/console.hpp"


using namespace ci;
using namespace ci::app;
using namespace std;
using namespace v8;

namespace cjs {

typedef boost::filesystem::path Path;

// TODO
// - Use getIsolate()->AddGCPrologueCallback(<#GCPrologueCallback callback#>) to switch isolates/context on GC
// - move v8 init to main to have a global handlescope (only osx support atm)
// - Split cinderjsApp in header file
// - load js modules with wrapper: "function (module, exports, __filename, ...) {"
// - Expose versions object (cinder, v8, cinderjs)

class CinderjsApp : public AppNative, public CinderAppBase  {
  public:
  ~CinderjsApp(){
    mShouldQuit = true;
    
    _v8Run = true;
    cvJSThread.notify_one();
    
    // Shutdown v8Thread
    if( mV8Thread ) {
      mV8Thread->join();
      mV8Thread.reset();
    }
    
    // Shutdown v8Thread
    if( mV8RenderThread ) {
      mV8RenderThread->join();
      mV8RenderThread.reset();
    }
    
    v8::V8::Dispose();
    
  }
  
	void setup();
	void mouseDown( MouseEvent event );	
	void mouseMove( MouseEvent event );
	void update();
	void draw();
  
  void v8Thread( std::string jsFileContents );
  void v8RenderThread();
  void runJS( std::string scriptStr );
  Local<Context> createMainContext(Isolate* isolate);
  
  private:
  
  volatile int v8Frames = 0;
  volatile double v8FPS = 0;
  volatile int mLastUpdate = 0;
  volatile int mUpdateInterval = 1000;
  
  volatile bool mShouldQuit;
  std::mutex mMainMutex;
  std::condition_variable cvJSThread;
  volatile bool _v8Run = false;
  std::condition_variable cvMainThread;
  volatile bool _mainRun = false;
  
  RendererRef glRenderer;
  
  // Path
  Path mCwd;
  
  // V8
  std::shared_ptr<std::thread> mV8Thread;
  std::shared_ptr<std::thread> mV8RenderThread;
  
  //static void LogCallback(const v8::FunctionCallbackInfo<v8::Value>& args);
  static void drawCallback(const v8::FunctionCallbackInfo<v8::Value>& args);
  
  // GC
  static void gcPrologueCb(Isolate *isolate, GCType type, GCCallbackFlags flags);
  static int sGCRuns;
};

int CinderjsApp::sGCRuns = 0;
void CinderjsApp::gcPrologueCb(Isolate *isolate, GCType type, GCCallbackFlags flags) {
  sGCRuns++;
  //AppConsole::log("GC Prologue " + std::to_string(sGCRuns));
  std::cout << "GC Prologue " << std::to_string(sGCRuns) << std::endl;
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
  
  // Command line arguments
  vector<std::string> args = getArgs();

  mCwd = boost::filesystem::current_path();
  AppConsole::log("Current Path: " + mCwd.string());
  
  // TODO: Choose between loading a script from asset folder or specified in command line
  #ifdef DEBUG
  //std::string jsMainFile = "/Users/sebastian/Dropbox/+Projects/cinderjs/lib/test.js";
  //std::string jsMainFile = "/Users/sebastian/Dropbox/+Projects/cinderjs/examples/particle.js";
  std::string jsMainFile = "/Users/sebastian/Dropbox/+Projects/cinderjs/examples/lines.js";
  #else
  std::string jsMainFile;
  #endif
  
  // Check argv arguments
  int pos = 0;
  for(std::vector<std::string>::iterator it = args.begin(); it != args.end(); ++it) {
    AppConsole::log("argv " + std::to_string(pos) + ":" + *it);
    pos++;
   
    std::string s = *it;
    if( s.find(".js") != std::string::npos ){
      jsMainFile = *it;
    }
  }
  
  // Do we have a js file to run?
  std::string jsFileContents;
  if(jsMainFile.length() > 0) {
    AppConsole::log("Starting app with JS file at: " + jsMainFile);
    
    if( !cinder::fs::exists( jsMainFile ) ){
      AppConsole::log("Could not find specified JS file!");
    } else {
      try {
        std::ifstream in(jsMainFile, std::ios::in );
        if (in)
        {
          std::ostringstream contents;
          contents << in.rdbuf();
          jsFileContents = contents.str();
          in.close();
        }
      } catch(std::exception &e) {
        std::string err = "Error: ";
        err.append(e.what());
        AppConsole::log( err );
      }
    }
  }
  
  // clear out the window with black
  gl::clear( Color( 0, 0, 0 ) );
  
  glRenderer = getRenderer();
  
  //CGLContextObj currCtx = CGLGetCurrentContext();
  mV8Thread = make_shared<std::thread>( boost::bind( &CinderjsApp::v8Thread, this, jsFileContents ) );
  
}

Local<Context> CinderjsApp::createMainContext(Isolate* isolate) {
  EscapableHandleScope handleScope(isolate);
  
  Local<Context> ct = Context::New(mIsolate, NULL, mGlobal);
  Context::Scope scope(ct);
  pContext.Reset(isolate, ct);
  
  return handleScope.Escape(ct);
}

/**
 * Runs a JS string and prints eventual errors to the AppConsole
 */
void CinderjsApp::runJS( std::string scriptStr ){
  v8::TryCatch try_catch;
  
  // Enter the context for compiling and running the hello world script.
  Context::Scope context_scope(mMainContext);
  
  // Create a string containing the JavaScript source code.
  Local<String> source = String::NewFromUtf8( mIsolate, scriptStr.c_str() );
  
  // Compile the source code.
  Local<Script> script = Script::Compile( source );
  
  // Run the script to get the result.
  Local<Value> result = script->Run();
  
  // Check for script errors
  if(result.IsEmpty()){
    if(try_catch.HasCaught()){
      //v8::String::Utf8Value exception(try_catch.Exception());
      v8::String::Utf8Value trace(try_catch.StackTrace());
      std::string ex = "JS Error: ";
      //ex.append(*exception);
      ex.append(*trace);
      AppConsole::log( ex );
    }
  }
  
}

void CinderjsApp::v8Thread( std::string jsFileContents ){
  ThreadSetup threadSetup;
  
  // Initialize V8 (implicit initialization was removed in an earlier revision)
  v8::V8::InitializeICU();
  v8::Platform* platform = v8::platform::CreateDefaultPlatform(4);
  v8::V8::InitializePlatform(platform);
  V8::Initialize();
  
  // Create a new Isolate and make it the current one.
  mIsolate = Isolate::New();
  v8::Locker lock(mIsolate);
  Isolate::Scope isolate_scope(mIsolate);
  
  mIsolate->AddGCPrologueCallback(gcPrologueCb);
  
  
  // Create a stack-allocated handle scope.
  HandleScope handle_scope(mIsolate);
  
  // Set general globals for JS
  mGlobal = ObjectTemplate::New();
  
  
  //
  // Load Modules
  addModule(boost::shared_ptr<AppModule>( new AppModule() ));
  addModule(boost::shared_ptr<GLModule>( new GLModule() ));
  addModule(boost::shared_ptr<ConsoleModule>( new ConsoleModule() ));
  
  
  // Create a new context.
  //mMainContext = Context::New(mIsolate, NULL, mGlobal);
  mMainContext = createMainContext(mIsolate);
  
  if( jsFileContents.length() > 0 ){
    runJS( jsFileContents );
  }

  mV8RenderThread = make_shared<std::thread>( boost::bind( &CinderjsApp::v8RenderThread, this ) );
}

void CinderjsApp::v8RenderThread(){
  ThreadSetup threadSetup;
  
  CGLContextObj currCtx = glRenderer->getCglContext();
  CGLSetCurrentContext( currCtx );  //also important as it now sets newly created context for use in this thread
  CGLEnable( currCtx, kCGLCEMPEngine ); //Apple's magic sauce that allows this OpenGL context  to run in a thread
  
  //
  // Render Loop, do work if available
  while( !mShouldQuit ) {
    
    // Wait for data to be processed...
    {
        std::unique_lock<std::mutex> lck( mMainMutex );
        cvJSThread.wait(lck, [this]{ return _v8Run; });
    }
    
    if(!mShouldQuit){
      
      // Gather some info...
      double now = getElapsedSeconds() * 1000;
      double elapsed = now - mLastUpdate;
      if(elapsed > mUpdateInterval) {
        v8FPS = v8Frames;
        v8Frames = 0;
        mLastUpdate = now;
      }
  
      CGLLockContext( currCtx ); //not sure if this is necessary but Apple's docs seem to suggest it
      glRenderer->startDraw();
      
      v8::Locker lock(mIsolate);
      
      // clear out the window with black
      gl::clear( Color( 0, 0, 0 ) );
    
      // Draw modules
      for( std::vector<boost::shared_ptr<PipeModule>>::iterator it = MODULES.begin(); it < MODULES.end(); ++it ) {
        it->get()->draw();
      }
      
      v8::Unlocker unlock(mIsolate);
      
      // FPS (TODO: if active)
      cinder::TextLayout fpsText;
      fpsText.setColor( cinder::ColorA( 1, 1, 1, 1 ) );
      fpsText.addRightLine( "FPS: " + std::to_string( cinder::app::AppBasic::getAverageFps() ) );
      fpsText.addRightLine( "v8FPS: " + std::to_string( v8FPS ) );
      cinder::gl::draw( cinder::gl::Texture( fpsText.render() ) );
//
//      // Draw console (TODO: if active)
//      Vec2f cPos;
//      cPos.y = getWindowHeight();
//      AppConsole::draw( cPos );
      
      v8Frames++;
      
      glRenderer->finishDraw();
      CGLUnlockContext( currCtx );
    }
    
    _v8Run = false;
    _mainRun = true;
    cvMainThread.notify_one();
  }
  
  std::cout << "V8 Render thread ending" << std::endl;
  
  CGLDisable( currCtx, kCGLCEMPEngine );
  
}

void CinderjsApp::mouseMove( MouseEvent event )
{
  // Push event to modules
  for( std::vector<boost::shared_ptr<PipeModule>>::iterator it = MODULES.begin(); it < MODULES.end(); ++it ) {
    //it->get()->mouseMove( event );
  }
}

void CinderjsApp::mouseDown( MouseEvent event )
{
  // Push event to modules
  for( std::vector<boost::shared_ptr<PipeModule>>::iterator it = MODULES.begin(); it < MODULES.end(); ++it ) {
    it->get()->mouseDown( event );
  }
}

void CinderjsApp::update()
{
}

void CinderjsApp::draw()
{
  
  if(!_v8Run){
    _v8Run = true;
    cvJSThread.notify_one();
  }
  
  // Wait for data to be processed...
//  {
//      std::unique_lock<std::mutex> lck( mMainMutex );
//      cvMainThread.wait(lck, [this]{ return _mainRun; });
//  }
  
  _mainRun = false;
}
  
} // namespace cjs

CINDER_APP_NATIVE( cjs::CinderjsApp, RendererGl )
