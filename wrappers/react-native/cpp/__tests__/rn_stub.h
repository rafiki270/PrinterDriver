// Minimal, test-only stand-in for the React Native headers cpp/PrinterDriverModule.cpp
// includes (<jsi/jsi.h>, <ReactCommon/TurboModule.h>, <ReactCommon/CallInvoker.h>), none
// of which exists on a host with no React Native application checked out.
//
// This file exists SOLELY so that
//
//   clang++ -std=c++17 -fsyntax-only -DPD_RN_TEST_STUB -include rn_stub.h \
//           -I ../../capi/include cpp/PrinterDriverModule.cpp
//
// can parse and type-check the module against the REAL pd.h on a plain host compiler --
// see scripts/check_rn_cpp_syntax.sh for the exact command and its negative controls, and
// README.md "Verification status" for what that does and does not prove. It is a
// syntax/type-check fixture, NOT a functional JSI implementation:
//
//   - every method below is declared and never defined (fine for -fsyntax-only, which
//     never reaches the linker);
//   - the OWNERSHIP RULES are modelled faithfully, because those are what a host compiler
//     can actually catch: jsi::Value, jsi::Object and jsi::String are move-only, exactly
//     as in the real jsi.h, so a stray copy is a compile error here as it is there. One of
//     the negative controls in check_rn_cpp_syntax.sh proves that rule is live;
//   - the class hierarchy mirrors the real one (Pointer <- Object <- Array/ArrayBuffer/
//     Function) closely enough to catch real type mistakes in the glue, but no layout here
//     is ABI-compatible with any real JSI build;
//   - it is never compiled into the shipped module. The real iOS and Android builds
//     (printerdriver-react-native.podspec, android/CMakeLists.txt) always use React
//     Native's own headers -- this file is not referenced anywhere in that path.
//
// Keep this in sync with whatever subset of JSI / ReactCommon
// cpp/PrinterDriverModule.cpp actually uses; it only needs to cover that subset.

#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace facebook {
namespace jsi {

class Runtime;
class Value;
class String;
class Object;
class Array;
class ArrayBuffer;
class Function;
class PropNameID;

/// The real jsi.h's base for everything that owns a JS reference. Move-only.
class Pointer {
 protected:
  Pointer() = default;

 public:
  Pointer(Pointer&&) noexcept;
  Pointer& operator=(Pointer&&) noexcept;
  Pointer(const Pointer&) = delete;
  Pointer& operator=(const Pointer&) = delete;
  ~Pointer();
};

class PropNameID : public Pointer {
 public:
  PropNameID(PropNameID&&) noexcept;
  PropNameID& operator=(PropNameID&&) noexcept;
  static PropNameID forAscii(Runtime& runtime, const char* ascii);
  static PropNameID forAscii(Runtime& runtime, const char* ascii, size_t length);
  static PropNameID forUtf8(Runtime& runtime, const std::string& utf8);
  std::string utf8(Runtime& runtime) const;
};

class String : public Pointer {
 public:
  String(String&&) noexcept;
  String& operator=(String&&) noexcept;
  static String createFromAscii(Runtime& runtime, const char* ascii);
  static String createFromUtf8(Runtime& runtime, const std::string& utf8);
  std::string utf8(Runtime& runtime) const;
};

class Object : public Pointer {
 public:
  explicit Object(Runtime& runtime);
  Object(Object&&) noexcept;
  Object& operator=(Object&&) noexcept;

  bool isFunction(Runtime& runtime) const;
  bool isArray(Runtime& runtime) const;
  bool isArrayBuffer(Runtime& runtime) const;

  Array getArray(Runtime& runtime) const&;
  ArrayBuffer getArrayBuffer(Runtime& runtime) const&;
  Function getFunction(Runtime& runtime) &&;

  Value getProperty(Runtime& runtime, const char* name) const;
  Value getProperty(Runtime& runtime, const PropNameID& name) const;
  bool hasProperty(Runtime& runtime, const char* name) const;
  Function getPropertyAsFunction(Runtime& runtime, const char* name) const;
  Object getPropertyAsObject(Runtime& runtime, const char* name) const;

  template <typename T>
  void setProperty(Runtime& runtime, const char* name, T&& value);
  template <typename T>
  void setProperty(Runtime& runtime, const PropNameID& name, T&& value);
};

class Array : public Object {
 public:
  Array(Runtime& runtime, size_t length);
  Array(Array&&) noexcept;
  Array& operator=(Array&&) noexcept;
  size_t size(Runtime& runtime) const;
  Value getValueAtIndex(Runtime& runtime, size_t index) const;
  void setValueAtIndex(Runtime& runtime, size_t index, const Value& value);
  void setValueAtIndex(Runtime& runtime, size_t index, Value&& value);
};

/// The real jsi.h's backing store for an ArrayBuffer created from C++.
class MutableBuffer {
 public:
  virtual ~MutableBuffer();
  virtual size_t size() const = 0;
  virtual uint8_t* data() = 0;
};

class ArrayBuffer : public Object {
 public:
  ArrayBuffer(Runtime& runtime, std::shared_ptr<MutableBuffer> buffer);
  ArrayBuffer(ArrayBuffer&&) noexcept;
  ArrayBuffer& operator=(ArrayBuffer&&) noexcept;
  size_t size(Runtime& runtime) const;
  uint8_t* data(Runtime& runtime) const;
};

class Function : public Object {
 public:
  Function(Function&&) noexcept;
  Function& operator=(Function&&) noexcept;

  using HostFunctionType =
      std::function<Value(Runtime&, const Value& thisVal, const Value* args, size_t count)>;

  static Function createFromHostFunction(Runtime& runtime, const PropNameID& name,
                                         unsigned int paramCount, HostFunctionType func);

  Value call(Runtime& runtime, const Value* args, size_t count) const;
  template <typename... Args>
  Value call(Runtime& runtime, Args&&... args) const;
  Value callAsConstructor(Runtime& runtime, const Value* args, size_t count) const;
  template <typename... Args>
  Value callAsConstructor(Runtime& runtime, Args&&... args) const;
};

using HostFunctionType = Function::HostFunctionType;

/// Move-only, exactly like the real one. Copying a jsi::Value must not compile.
class Value {
 public:
  Value();
  Value(std::nullptr_t);
  explicit Value(bool value);
  explicit Value(double value);
  explicit Value(int value);
  Value(Runtime& runtime, const Value& other);
  Value(String&& value);
  Value(Object&& value);
  Value(Array&& value);
  Value(ArrayBuffer&& value);
  Value(Function&& value);
  Value(Value&&) noexcept;
  Value& operator=(Value&&) noexcept;
  Value(const Value&) = delete;
  Value& operator=(const Value&) = delete;
  ~Value();

  static Value undefined();
  static Value null();

  bool isUndefined() const;
  bool isNull() const;
  bool isBool() const;
  bool isNumber() const;
  bool isString() const;
  bool isObject() const;

  bool getBool() const;
  double getNumber() const;
  double asNumber() const;
  String getString(Runtime& runtime) const&;
  Object getObject(Runtime& runtime) const&;
  Object asObject(Runtime& runtime) const&;
};

class HostObject {
 public:
  virtual ~HostObject();
  virtual Value get(Runtime& runtime, const PropNameID& name);
  virtual void set(Runtime& runtime, const PropNameID& name, const Value& value);
  virtual std::vector<PropNameID> getPropertyNames(Runtime& runtime);
};

class Runtime {
 public:
  virtual ~Runtime();
  Object global();
};

class JSError : public std::exception {
 public:
  JSError(Runtime& runtime, std::string message);
  const char* what() const noexcept override;
};

}  // namespace jsi

namespace react {

/// <ReactCommon/CallInvoker.h>. The overload the module uses is the plain
/// `std::function<void()>` one, which every React Native version from 0.71 onwards has.
class CallInvoker {
 public:
  virtual ~CallInvoker();
  virtual void invokeAsync(std::function<void()>&& func) = 0;
  virtual void invokeSync(std::function<void()>&& func) = 0;
};

/// <ReactCommon/TurboModule.h>.
class TurboModule : public jsi::HostObject {
 public:
  TurboModule(std::string name, std::shared_ptr<CallInvoker> jsInvoker);
  ~TurboModule() override;
  jsi::Value get(jsi::Runtime& runtime, const jsi::PropNameID& propName) override;
  std::vector<jsi::PropNameID> getPropertyNames(jsi::Runtime& runtime) override;

 protected:
  std::string name_;
  std::shared_ptr<CallInvoker> jsInvoker_;
};

}  // namespace react
}  // namespace facebook
