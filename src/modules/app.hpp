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
#ifndef _AppModule_hpp_
#define _AppModule_hpp_

#pragma once

#define APP_MOD_ID 0

#include "../PipeModule.hpp"

using namespace cinder;

namespace cjs {
  
class AppModule : public PipeModule {
  public:
    AppModule(){}
    ~AppModule(){}
  
    inline int moduleId() {
      return APP_MOD_ID;
    }
  
    inline std::string getName() {
      //std::string name = "app";
      return "app";
    }
  
    void loadGlobalJS( v8::Local<v8::ObjectTemplate> &global );
    
  //
  private:
    static void getAspectRatio(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void addAssetDirectory(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void disableFrameRate(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void setFrameRate(const v8::FunctionCallbackInfo<v8::Value>& args);
  
 };
  
} // namespace cjs

#endif