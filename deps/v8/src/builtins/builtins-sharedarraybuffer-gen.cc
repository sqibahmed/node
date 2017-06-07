// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/builtins/builtins-utils-gen.h"
#include "src/builtins/builtins.h"
#include "src/code-stub-assembler.h"
#include "src/objects.h"

namespace v8 {
namespace internal {

using compiler::Node;

class SharedArrayBufferBuiltinsAssembler : public CodeStubAssembler {
 public:
  explicit SharedArrayBufferBuiltinsAssembler(
      compiler::CodeAssemblerState* state)
      : CodeStubAssembler(state) {}

 protected:
  typedef Node* (CodeAssembler::*AssemblerFunction)(MachineType type,
                                                    Node* base, Node* offset,
                                                    Node* value);
  void ValidateSharedTypedArray(Node* tagged, Node* context,
                                Node** out_instance_type,
                                Node** out_backing_store);
  Node* ConvertTaggedAtomicIndexToWord32(Node* tagged, Node* context,
                                         Node** number_index);
  void ValidateAtomicIndex(Node* index_word, Node* array_length_word,
                           Node* context);
  void AtomicBinopBuiltinCommon(Node* array, Node* index, Node* value,
                                Node* context, AssemblerFunction function,
                                Runtime::FunctionId runtime_function);
};

void SharedArrayBufferBuiltinsAssembler::ValidateSharedTypedArray(
    Node* tagged, Node* context, Node** out_instance_type,
    Node** out_backing_store) {
  Label not_float_or_clamped(this), invalid(this);

  // Fail if it is not a heap object.
  GotoIf(TaggedIsSmi(tagged), &invalid);

  // Fail if the array's instance type is not JSTypedArray.
  GotoIf(Word32NotEqual(LoadInstanceType(tagged),
                        Int32Constant(JS_TYPED_ARRAY_TYPE)),
         &invalid);

  // Fail if the array's JSArrayBuffer is not shared.
  Node* array_buffer = LoadObjectField(tagged, JSTypedArray::kBufferOffset);
  Node* bitfield = LoadObjectField(array_buffer, JSArrayBuffer::kBitFieldOffset,
                                   MachineType::Uint32());
  GotoIfNot(IsSetWord32<JSArrayBuffer::IsShared>(bitfield), &invalid);

  // Fail if the array's element type is float32, float64 or clamped.
  Node* elements_instance_type =
      LoadInstanceType(LoadObjectField(tagged, JSObject::kElementsOffset));
  STATIC_ASSERT(FIXED_INT8_ARRAY_TYPE < FIXED_FLOAT32_ARRAY_TYPE);
  STATIC_ASSERT(FIXED_INT16_ARRAY_TYPE < FIXED_FLOAT32_ARRAY_TYPE);
  STATIC_ASSERT(FIXED_INT32_ARRAY_TYPE < FIXED_FLOAT32_ARRAY_TYPE);
  STATIC_ASSERT(FIXED_UINT8_ARRAY_TYPE < FIXED_FLOAT32_ARRAY_TYPE);
  STATIC_ASSERT(FIXED_UINT16_ARRAY_TYPE < FIXED_FLOAT32_ARRAY_TYPE);
  STATIC_ASSERT(FIXED_UINT32_ARRAY_TYPE < FIXED_FLOAT32_ARRAY_TYPE);
  Branch(Int32LessThan(elements_instance_type,
                       Int32Constant(FIXED_FLOAT32_ARRAY_TYPE)),
         &not_float_or_clamped, &invalid);

  BIND(&invalid);
  {
    CallRuntime(Runtime::kThrowNotIntegerSharedTypedArrayError, context,
                tagged);
    Unreachable();
  }

  BIND(&not_float_or_clamped);
  *out_instance_type = elements_instance_type;

  Node* backing_store =
      LoadObjectField(array_buffer, JSArrayBuffer::kBackingStoreOffset);
  Node* byte_offset = ChangeUint32ToWord(TruncateTaggedToWord32(
      context, LoadObjectField(tagged, JSArrayBufferView::kByteOffsetOffset)));
  *out_backing_store =
      IntPtrAdd(BitcastTaggedToWord(backing_store), byte_offset);
}

// https://tc39.github.io/ecmascript_sharedmem/shmem.html#Atomics.ValidateAtomicAccess
Node* SharedArrayBufferBuiltinsAssembler::ConvertTaggedAtomicIndexToWord32(
    Node* tagged, Node* context, Node** number_index) {
  VARIABLE(var_result, MachineRepresentation::kWord32);

  // TODO(jkummerow): Skip ToNumber call when |tagged| is a number already.
  // Maybe this can be unified with other tagged-to-index conversions?
  // Why does this return an int32, and not an intptr?
  // Why is there the additional |number_index| output parameter?
  Callable to_number = CodeFactory::ToNumber(isolate());
  *number_index = CallStub(to_number, context, tagged);
  Label done(this, &var_result);

  Label if_numberissmi(this), if_numberisnotsmi(this);
  Branch(TaggedIsSmi(*number_index), &if_numberissmi, &if_numberisnotsmi);

  BIND(&if_numberissmi);
  {
    var_result.Bind(SmiToWord32(*number_index));
    Goto(&done);
  }

  BIND(&if_numberisnotsmi);
  {
    Node* number_index_value = LoadHeapNumberValue(*number_index);
    Node* access_index = TruncateFloat64ToWord32(number_index_value);
    Node* test_index = ChangeInt32ToFloat64(access_index);

    Label if_indexesareequal(this), if_indexesarenotequal(this);
    Branch(Float64Equal(number_index_value, test_index), &if_indexesareequal,
           &if_indexesarenotequal);

    BIND(&if_indexesareequal);
    {
      var_result.Bind(access_index);
      Goto(&done);
    }

    BIND(&if_indexesarenotequal);
    {
      CallRuntime(Runtime::kThrowInvalidAtomicAccessIndexError, context);
      Unreachable();
    }
  }

  BIND(&done);
  return var_result.value();
}

void SharedArrayBufferBuiltinsAssembler::ValidateAtomicIndex(
    Node* index_word, Node* array_length_word, Node* context) {
  // Check if the index is in bounds. If not, throw RangeError.
  Label check_passed(this);
  GotoIf(Uint32LessThan(index_word, array_length_word), &check_passed);

  CallRuntime(Runtime::kThrowInvalidAtomicAccessIndexError, context);
  Unreachable();

  BIND(&check_passed);
}

TF_BUILTIN(AtomicsLoad, SharedArrayBufferBuiltinsAssembler) {
  Node* array = Parameter(Descriptor::kArray);
  Node* index = Parameter(Descriptor::kIndex);
  Node* context = Parameter(Descriptor::kContext);

  Node* index_integer;
  Node* index_word32 =
      ConvertTaggedAtomicIndexToWord32(index, context, &index_integer);

  Node* instance_type;
  Node* backing_store;
  ValidateSharedTypedArray(array, context, &instance_type, &backing_store);

  Node* array_length_word32 = TruncateTaggedToWord32(
      context, LoadObjectField(array, JSTypedArray::kLengthOffset));
  ValidateAtomicIndex(index_word32, array_length_word32, context);
  Node* index_word = ChangeUint32ToWord(index_word32);

  Label i8(this), u8(this), i16(this), u16(this), i32(this), u32(this),
      other(this);
  int32_t case_values[] = {
      FIXED_INT8_ARRAY_TYPE,   FIXED_UINT8_ARRAY_TYPE, FIXED_INT16_ARRAY_TYPE,
      FIXED_UINT16_ARRAY_TYPE, FIXED_INT32_ARRAY_TYPE, FIXED_UINT32_ARRAY_TYPE,
  };
  Label* case_labels[] = {
      &i8, &u8, &i16, &u16, &i32, &u32,
  };
  Switch(instance_type, &other, case_values, case_labels,
         arraysize(case_labels));

  BIND(&i8);
  Return(SmiFromWord32(
      AtomicLoad(MachineType::Int8(), backing_store, index_word)));

  BIND(&u8);
  Return(SmiFromWord32(
      AtomicLoad(MachineType::Uint8(), backing_store, index_word)));

  BIND(&i16);
  Return(SmiFromWord32(
      AtomicLoad(MachineType::Int16(), backing_store, WordShl(index_word, 1))));

  BIND(&u16);
  Return(SmiFromWord32(AtomicLoad(MachineType::Uint16(), backing_store,
                                  WordShl(index_word, 1))));

  BIND(&i32);
  Return(ChangeInt32ToTagged(
      AtomicLoad(MachineType::Int32(), backing_store, WordShl(index_word, 2))));

  BIND(&u32);
  Return(ChangeUint32ToTagged(AtomicLoad(MachineType::Uint32(), backing_store,
                                         WordShl(index_word, 2))));

  // This shouldn't happen, we've already validated the type.
  BIND(&other);
  Unreachable();
}

TF_BUILTIN(AtomicsStore, SharedArrayBufferBuiltinsAssembler) {
  Node* array = Parameter(Descriptor::kArray);
  Node* index = Parameter(Descriptor::kIndex);
  Node* value = Parameter(Descriptor::kValue);
  Node* context = Parameter(Descriptor::kContext);

  // The value_integer needs to be computed before the validations as the
  // ToInteger function can be potentially modified in JS to invalidate the
  // conditions. This is just a no-cost safety measure as SABs can't be neutered
  // or shrunk.
  Node* value_integer = ToInteger(context, value);
  Node* value_word32 = TruncateTaggedToWord32(context, value_integer);

  Node* index_integer;
  Node* index_word32 =
      ConvertTaggedAtomicIndexToWord32(index, context, &index_integer);

  Node* instance_type;
  Node* backing_store;
  ValidateSharedTypedArray(array, context, &instance_type, &backing_store);

  Node* array_length_word32 = TruncateTaggedToWord32(
      context, LoadObjectField(array, JSTypedArray::kLengthOffset));
  ValidateAtomicIndex(index_word32, array_length_word32, context);
  Node* index_word = ChangeUint32ToWord(index_word32);

  Label u8(this), u16(this), u32(this), other(this);
  int32_t case_values[] = {
      FIXED_INT8_ARRAY_TYPE,   FIXED_UINT8_ARRAY_TYPE, FIXED_INT16_ARRAY_TYPE,
      FIXED_UINT16_ARRAY_TYPE, FIXED_INT32_ARRAY_TYPE, FIXED_UINT32_ARRAY_TYPE,
  };
  Label* case_labels[] = {
      &u8, &u8, &u16, &u16, &u32, &u32,
  };
  Switch(instance_type, &other, case_values, case_labels,
         arraysize(case_labels));

  BIND(&u8);
  AtomicStore(MachineRepresentation::kWord8, backing_store, index_word,
              value_word32);
  Return(value_integer);

  BIND(&u16);
  AtomicStore(MachineRepresentation::kWord16, backing_store,
              WordShl(index_word, 1), value_word32);
  Return(value_integer);

  BIND(&u32);
  AtomicStore(MachineRepresentation::kWord32, backing_store,
              WordShl(index_word, 2), value_word32);
  Return(value_integer);

  // This shouldn't happen, we've already validated the type.
  BIND(&other);
  Unreachable();
}

TF_BUILTIN(AtomicsExchange, SharedArrayBufferBuiltinsAssembler) {
  Node* array = Parameter(Descriptor::kArray);
  Node* index = Parameter(Descriptor::kIndex);
  Node* value = Parameter(Descriptor::kValue);
  Node* context = Parameter(Descriptor::kContext);

  // The value_integer needs to be computed before the validations as the
  // ToInteger function can be potentially modified in JS to invalidate the
  // conditions. This is just a no-cost safety measure as SABs can't be neutered
  // or shrunk.
  Node* value_integer = ToInteger(context, value);

  Node* index_integer;
  Node* index_word32 =
      ConvertTaggedAtomicIndexToWord32(index, context, &index_integer);

  Node* instance_type;
  Node* backing_store;
  ValidateSharedTypedArray(array, context, &instance_type, &backing_store);

  Node* array_length_word32 = TruncateTaggedToWord32(
      context, LoadObjectField(array, JSTypedArray::kLengthOffset));
  ValidateAtomicIndex(index_word32, array_length_word32, context);

#if V8_TARGET_ARCH_MIPS || V8_TARGET_ARCH_MIPS64
  Return(CallRuntime(Runtime::kAtomicsExchange, context, array, index_integer,
                     value_integer));
#else
  Node* index_word = ChangeUint32ToWord(index_word32);

  Node* value_word32 = TruncateTaggedToWord32(context, value_integer);

  Label i8(this), u8(this), i16(this), u16(this), i32(this), u32(this),
      other(this);
  int32_t case_values[] = {
      FIXED_INT8_ARRAY_TYPE,   FIXED_UINT8_ARRAY_TYPE, FIXED_INT16_ARRAY_TYPE,
      FIXED_UINT16_ARRAY_TYPE, FIXED_INT32_ARRAY_TYPE, FIXED_UINT32_ARRAY_TYPE,
  };
  Label* case_labels[] = {
      &i8, &u8, &i16, &u16, &i32, &u32,
  };
  Switch(instance_type, &other, case_values, case_labels,
         arraysize(case_labels));

  BIND(&i8);
  Return(SmiFromWord32(AtomicExchange(MachineType::Int8(), backing_store,
                                      index_word, value_word32)));

  BIND(&u8);
  Return(SmiFromWord32(AtomicExchange(MachineType::Uint8(), backing_store,
                                      index_word, value_word32)));

  BIND(&i16);
  Return(SmiFromWord32(AtomicExchange(MachineType::Int16(), backing_store,
                                      WordShl(index_word, 1), value_word32)));

  BIND(&u16);
  Return(SmiFromWord32(AtomicExchange(MachineType::Uint16(), backing_store,
                                      WordShl(index_word, 1), value_word32)));

  BIND(&i32);
  Return(ChangeInt32ToTagged(AtomicExchange(MachineType::Int32(), backing_store,
                                            WordShl(index_word, 2),
                                            value_word32)));

  BIND(&u32);
  Return(ChangeUint32ToTagged(
      AtomicExchange(MachineType::Uint32(), backing_store,
                     WordShl(index_word, 2), value_word32)));

  // This shouldn't happen, we've already validated the type.
  BIND(&other);
  Unreachable();
#endif  // V8_TARGET_ARCH_MIPS || V8_TARGET_ARCH_MIPS64
}

TF_BUILTIN(AtomicsCompareExchange, SharedArrayBufferBuiltinsAssembler) {
  Node* array = Parameter(Descriptor::kArray);
  Node* index = Parameter(Descriptor::kIndex);
  Node* old_value = Parameter(Descriptor::kOldValue);
  Node* new_value = Parameter(Descriptor::kNewValue);
  Node* context = Parameter(Descriptor::kContext);

  // The value_integers needs to be computed before the validations as the
  // ToInteger function can be potentially modified in JS to invalidate the
  // conditions. This is just a no-cost safety measure as SABs can't be neutered
  // or shrunk.
  Node* old_value_integer = ToInteger(context, old_value);
  Node* new_value_integer = ToInteger(context, new_value);

  Node* index_integer;
  Node* index_word32 =
      ConvertTaggedAtomicIndexToWord32(index, context, &index_integer);

  Node* instance_type;
  Node* backing_store;
  ValidateSharedTypedArray(array, context, &instance_type, &backing_store);

  Node* array_length_word32 = TruncateTaggedToWord32(
      context, LoadObjectField(array, JSTypedArray::kLengthOffset));
  ValidateAtomicIndex(index_word32, array_length_word32, context);

#if V8_TARGET_ARCH_MIPS || V8_TARGET_ARCH_MIPS64 || V8_TARGET_ARCH_PPC64 || \
    V8_TARGET_ARCH_PPC || V8_TARGET_ARCH_S390 || V8_TARGET_ARCH_S390X
  Return(CallRuntime(Runtime::kAtomicsCompareExchange, context, array,
                     index_integer, old_value_integer, new_value_integer));
#else
  Node* index_word = ChangeUint32ToWord(index_word32);

  Node* old_value_word32 = TruncateTaggedToWord32(context, old_value_integer);

  Node* new_value_word32 = TruncateTaggedToWord32(context, new_value_integer);

  Label i8(this), u8(this), i16(this), u16(this), i32(this), u32(this),
      other(this);
  int32_t case_values[] = {
      FIXED_INT8_ARRAY_TYPE,   FIXED_UINT8_ARRAY_TYPE, FIXED_INT16_ARRAY_TYPE,
      FIXED_UINT16_ARRAY_TYPE, FIXED_INT32_ARRAY_TYPE, FIXED_UINT32_ARRAY_TYPE,
  };
  Label* case_labels[] = {
      &i8, &u8, &i16, &u16, &i32, &u32,
  };
  Switch(instance_type, &other, case_values, case_labels,
         arraysize(case_labels));

  BIND(&i8);
  Return(SmiFromWord32(AtomicCompareExchange(MachineType::Int8(), backing_store,
                                             index_word, old_value_word32,
                                             new_value_word32)));

  BIND(&u8);
  Return(SmiFromWord32(
      AtomicCompareExchange(MachineType::Uint8(), backing_store, index_word,
                            old_value_word32, new_value_word32)));

  BIND(&i16);
  Return(SmiFromWord32(AtomicCompareExchange(
      MachineType::Int16(), backing_store, WordShl(index_word, 1),
      old_value_word32, new_value_word32)));

  BIND(&u16);
  Return(SmiFromWord32(AtomicCompareExchange(
      MachineType::Uint16(), backing_store, WordShl(index_word, 1),
      old_value_word32, new_value_word32)));

  BIND(&i32);
  Return(ChangeInt32ToTagged(AtomicCompareExchange(
      MachineType::Int32(), backing_store, WordShl(index_word, 2),
      old_value_word32, new_value_word32)));

  BIND(&u32);
  Return(ChangeUint32ToTagged(AtomicCompareExchange(
      MachineType::Uint32(), backing_store, WordShl(index_word, 2),
      old_value_word32, new_value_word32)));

  // This shouldn't happen, we've already validated the type.
  BIND(&other);
  Unreachable();
#endif  // V8_TARGET_ARCH_MIPS || V8_TARGET_ARCH_MIPS64 || V8_TARGET_ARCH_PPC64
        // || V8_TARGET_ARCH_PPC || V8_TARGET_ARCH_S390 || V8_TARGET_ARCH_S390X
}

#define BINOP_BUILTIN(op)                                       \
  TF_BUILTIN(Atomics##op, SharedArrayBufferBuiltinsAssembler) { \
    Node* array = Parameter(Descriptor::kArray);                \
    Node* index = Parameter(Descriptor::kIndex);                \
    Node* value = Parameter(Descriptor::kValue);                \
    Node* context = Parameter(Descriptor::kContext);            \
    AtomicBinopBuiltinCommon(array, index, value, context,      \
                             &CodeAssembler::Atomic##op,        \
                             Runtime::kAtomics##op);            \
  }
BINOP_BUILTIN(Add)
BINOP_BUILTIN(Sub)
BINOP_BUILTIN(And)
BINOP_BUILTIN(Or)
BINOP_BUILTIN(Xor)
#undef BINOP_BUILTIN

void SharedArrayBufferBuiltinsAssembler::AtomicBinopBuiltinCommon(
    Node* array, Node* index, Node* value, Node* context,
    AssemblerFunction function, Runtime::FunctionId runtime_function) {
  // The value_integer needs to be computed before the validations as the
  // ToInteger function can be potentially modified in JS to invalidate the
  // conditions. This is just a no-cost safety measure as SABs can't be neutered
  // or shrunk.
  Node* value_integer = ToInteger(context, value);

  Node* index_integer;
  Node* index_word32 =
      ConvertTaggedAtomicIndexToWord32(index, context, &index_integer);

  Node* instance_type;
  Node* backing_store;
  ValidateSharedTypedArray(array, context, &instance_type, &backing_store);

  Node* array_length_word32 = TruncateTaggedToWord32(
      context, LoadObjectField(array, JSTypedArray::kLengthOffset));
  ValidateAtomicIndex(index_word32, array_length_word32, context);

#if V8_TARGET_ARCH_MIPS || V8_TARGET_ARCH_MIPS64 || V8_TARGET_ARCH_PPC64 || \
    V8_TARGET_ARCH_PPC || V8_TARGET_ARCH_S390 || V8_TARGET_ARCH_S390X
  Return(CallRuntime(runtime_function, context, array, index_integer,
                     value_integer));
#else
  Node* index_word = ChangeUint32ToWord(index_word32);

  Node* value_word32 = TruncateTaggedToWord32(context, value_integer);

  Label i8(this), u8(this), i16(this), u16(this), i32(this), u32(this),
      other(this);
  int32_t case_values[] = {
      FIXED_INT8_ARRAY_TYPE,   FIXED_UINT8_ARRAY_TYPE, FIXED_INT16_ARRAY_TYPE,
      FIXED_UINT16_ARRAY_TYPE, FIXED_INT32_ARRAY_TYPE, FIXED_UINT32_ARRAY_TYPE,
  };
  Label* case_labels[] = {
      &i8, &u8, &i16, &u16, &i32, &u32,
  };
  Switch(instance_type, &other, case_values, case_labels,
         arraysize(case_labels));

  Bind(&i8);
  Return(SmiFromWord32((this->*function)(MachineType::Int8(), backing_store,
                                         index_word, value_word32)));

  Bind(&u8);
  Return(SmiFromWord32((this->*function)(MachineType::Uint8(), backing_store,
                                         index_word, value_word32)));

  Bind(&i16);
  Return(
      SmiFromWord32((this->*function)(MachineType::Int16(), backing_store,
                                      WordShl(index_word, 1), value_word32)));

  Bind(&u16);
  Return(
      SmiFromWord32((this->*function)(MachineType::Uint16(), backing_store,
                                      WordShl(index_word, 1), value_word32)));

  Bind(&i32);
  Return(ChangeInt32ToTagged(
      (this->*function)(MachineType::Int32(), backing_store,
                        WordShl(index_word, 2), value_word32)));

  Bind(&u32);
  Return(ChangeUint32ToTagged(
      (this->*function)(MachineType::Uint32(), backing_store,
                        WordShl(index_word, 2), value_word32)));

  // This shouldn't happen, we've already validated the type.
  Bind(&other);
  Unreachable();
#endif  // V8_TARGET_ARCH_MIPS || V8_TARGET_ARCH_MIPS64 || V8_TARGET_ARCH_PPC64
        // || V8_TARGET_ARCH_PPC || V8_TARGET_ARCH_S390 || V8_TARGET_ARCH_S390X
}

}  // namespace internal
}  // namespace v8
