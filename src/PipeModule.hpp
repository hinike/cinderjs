/*
 Copyright (c) Sebastian Herrlinger - All rights reserved.
 This code is intended for use with the Cinder C++ library: http://libcinder.org
 
 Redistribution and use in source and binary forms, with or without modification, are permitted provided that
 the following conditions are met:
 
 * Redistributions of source code must retain the above copyright notice, this list of conditions and
 the following disclaimer.
 * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and
 the following disclaimer in the documentation and/or other materials provided with the distribution.
 
 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
 WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef _PipeModule_hpp_
#define _PipeModule_hpp_

#pragma once

#include "cinder/app/Event.h"

#include "cinder/app/App.h"

#include "v8.h"

namespace cjs {
  
  // TODO:
  // - provide base methods to register as callback for draw, mouse/key events etc. -> registerMouseMove() -> vector<PipeModule> _mouseMoveModules;
  // - force modules to have a name which they can be identified with
  
  class PipeModule {
    public:
      PipeModule(){}
      ~PipeModule(){}
    
      inline void setIsolate( v8::Isolate &isolate ) {
        mIsolate = &isolate;
      }
    
      inline v8::Isolate* getIsolate() {
        return mIsolate;
      }
    
      inline void setContext( v8::Persistent<v8::Context>* context ) {
        ctx = context;
      }
    
      static void setApp( ci::app::App *app ) {
        sApp = app;
      }
    
      static ci::app::App* getApp() {
        return sApp;
      }
    
      inline v8::Persistent<v8::Context>* getContext() {
        return ctx;
      }
    
      // Virtual Spec
      // TODO: rename loadGlobalJS to loadBindings
      virtual void loadGlobalJS( v8::Local<v8::ObjectTemplate> &global ) = 0;
      virtual std::string getName() = 0;
      
    private:
      v8::Isolate* mIsolate;
      v8::Persistent<v8::Context>* ctx;
      static cinder::app::App* sApp;
  };
}

#endif