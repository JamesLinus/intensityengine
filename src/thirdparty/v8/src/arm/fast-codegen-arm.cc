// Copyright 2009 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "v8.h"

#include "codegen-inl.h"
#include "compiler.h"
#include "debug.h"
#include "fast-codegen.h"
#include "parser.h"

namespace v8 {
namespace internal {

#define __ ACCESS_MASM(masm_)

// Generate code for a JS function.  On entry to the function the receiver
// and arguments have been pushed on the stack left to right.  The actual
// argument count matches the formal parameter count expected by the
// function.
//
// The live registers are:
//   o r1: the JS function object being called (ie, ourselves)
//   o cp: our context
//   o fp: our caller's frame pointer
//   o sp: stack pointer
//   o lr: return address
//
// The function builds a JS frame.  Please see JavaScriptFrameConstants in
// frames-arm.h for its layout.
void FastCodeGenerator::Generate(FunctionLiteral* fun) {
  function_ = fun;
  SetFunctionPosition(fun);
  int locals_count = fun->scope()->num_stack_slots();

  __ stm(db_w, sp, r1.bit() | cp.bit() | fp.bit() | lr.bit());
  if (locals_count > 0) {
      // Load undefined value here, so the value is ready for the loop below.
      __ LoadRoot(ip, Heap::kUndefinedValueRootIndex);
  }
  // Adjust fp to point to caller's fp.
  __ add(fp, sp, Operand(2 * kPointerSize));

  { Comment cmnt(masm_, "[ Allocate locals");
    for (int i = 0; i < locals_count; i++) {
      __ push(ip);
    }
  }

  bool function_in_register = true;

  // Possibly allocate a local context.
  if (fun->scope()->num_heap_slots() > 0) {
    Comment cmnt(masm_, "[ Allocate local context");
    // Argument to NewContext is the function, which is in r1.
    __ push(r1);
    __ CallRuntime(Runtime::kNewContext, 1);
    function_in_register = false;
    // Context is returned in both r0 and cp.  It replaces the context
    // passed to us.  It's saved in the stack and kept live in cp.
    __ str(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
    // Copy any necessary parameters into the context.
    int num_parameters = fun->scope()->num_parameters();
    for (int i = 0; i < num_parameters; i++) {
      Slot* slot = fun->scope()->parameter(i)->slot();
      if (slot != NULL && slot->type() == Slot::CONTEXT) {
        int parameter_offset = StandardFrameConstants::kCallerSPOffset +
                               (num_parameters - 1 - i) * kPointerSize;
        // Load parameter from stack.
        __ ldr(r0, MemOperand(fp, parameter_offset));
        // Store it in the context
        __ str(r0, MemOperand(cp, Context::SlotOffset(slot->index())));
      }
    }
  }

  Variable* arguments = fun->scope()->arguments()->AsVariable();
  if (arguments != NULL) {
    // Function uses arguments object.
    Comment cmnt(masm_, "[ Allocate arguments object");
    if (!function_in_register) {
      // Load this again, if it's used by the local context below.
      __ ldr(r3, MemOperand(fp, JavaScriptFrameConstants::kFunctionOffset));
    } else {
      __ mov(r3, r1);
    }
    // Receiver is just before the parameters on the caller's stack.
    __ add(r2, fp, Operand(StandardFrameConstants::kCallerSPOffset +
                               fun->num_parameters() * kPointerSize));
    __ mov(r1, Operand(Smi::FromInt(fun->num_parameters())));
    __ stm(db_w, sp, r3.bit() | r2.bit() | r1.bit());

    // Arguments to ArgumentsAccessStub:
    //   function, receiver address, parameter count.
    // The stub will rewrite receiever and parameter count if the previous
    // stack frame was an arguments adapter frame.
    ArgumentsAccessStub stub(ArgumentsAccessStub::NEW_OBJECT);
    __ CallStub(&stub);
    // Duplicate the value; move-to-slot operation might clobber registers.
    __ mov(r3, r0);
    Move(arguments->slot(), r0, r1, r2);
    Slot* dot_arguments_slot =
        fun->scope()->arguments_shadow()->AsVariable()->slot();
    Move(dot_arguments_slot, r3, r1, r2);
  }

  // Check the stack for overflow or break request.
  // Put the lr setup instruction in the delay slot.  The kInstrSize is
  // added to the implicit 8 byte offset that always applies to operations
  // with pc and gives a return address 12 bytes down.
  { Comment cmnt(masm_, "[ Stack check");
  __ LoadRoot(r2, Heap::kStackLimitRootIndex);
  __ add(lr, pc, Operand(Assembler::kInstrSize));
  __ cmp(sp, Operand(r2));
  StackCheckStub stub;
  __ mov(pc,
         Operand(reinterpret_cast<intptr_t>(stub.GetCode().location()),
                 RelocInfo::CODE_TARGET),
         LeaveCC,
         lo);
  }

  { Comment cmnt(masm_, "[ Declarations");
    VisitDeclarations(fun->scope()->declarations());
  }

  if (FLAG_trace) {
    __ CallRuntime(Runtime::kTraceEnter, 0);
  }

  { Comment cmnt(masm_, "[ Body");
    ASSERT(loop_depth() == 0);
    VisitStatements(fun->body());
    ASSERT(loop_depth() == 0);
  }

  { Comment cmnt(masm_, "[ return <undefined>;");
    // Emit a 'return undefined' in case control fell off the end of the
    // body.
    __ LoadRoot(r0, Heap::kUndefinedValueRootIndex);
  }
  EmitReturnSequence(function_->end_position());
}


void FastCodeGenerator::EmitReturnSequence(int position) {
  Comment cmnt(masm_, "[ Return sequence");
  if (return_label_.is_bound()) {
    __ b(&return_label_);
  } else {
    __ bind(&return_label_);
    if (FLAG_trace) {
      // Push the return value on the stack as the parameter.
      // Runtime::TraceExit returns its parameter in r0.
      __ push(r0);
      __ CallRuntime(Runtime::kTraceExit, 1);
    }

    // Add a label for checking the size of the code used for returning.
    Label check_exit_codesize;
    masm_->bind(&check_exit_codesize);

    // Calculate the exact length of the return sequence and make sure that
    // the constant pool is not emitted inside of the return sequence.
    int num_parameters = function_->scope()->num_parameters();
    int32_t sp_delta = (num_parameters + 1) * kPointerSize;
    int return_sequence_length = Assembler::kJSReturnSequenceLength;
    if (!masm_->ImmediateFitsAddrMode1Instruction(sp_delta)) {
      // Additional mov instruction generated.
      return_sequence_length++;
    }
    masm_->BlockConstPoolFor(return_sequence_length);

    CodeGenerator::RecordPositions(masm_, position);
    __ RecordJSReturn();
    __ mov(sp, fp);
    __ ldm(ia_w, sp, fp.bit() | lr.bit());
    __ add(sp, sp, Operand(sp_delta));
    __ Jump(lr);

    // Check that the size of the code used for returning matches what is
    // expected by the debugger. The add instruction above is an addressing
    // mode 1 instruction where there are restrictions on which immediate values
    // can be encoded in the instruction and which immediate values requires
    // use of an additional instruction for moving the immediate to a temporary
    // register.
    ASSERT_EQ(return_sequence_length,
              masm_->InstructionsGeneratedSince(&check_exit_codesize));
  }
}


void FastCodeGenerator::Apply(Expression::Context context, Register reg) {
  switch (context) {
    case Expression::kUninitialized:
      UNREACHABLE();

    case Expression::kEffect:
      // Nothing to do.
      break;

    case Expression::kValue:
      // Move value into place.
      switch (location_) {
        case kAccumulator:
          if (!reg.is(result_register())) __ mov(result_register(), reg);
          break;
        case kStack:
          __ push(reg);
          break;
      }
      break;

    case Expression::kValueTest:
    case Expression::kTestValue:
      // Push an extra copy of the value in case it's needed.
      __ push(reg);
      // Fall through.

    case Expression::kTest:
      // We always call the runtime on ARM, so push the value as argument.
      __ push(reg);
      DoTest(context);
      break;
  }
}


void FastCodeGenerator::Apply(Expression::Context context, Slot* slot) {
  switch (context) {
    case Expression::kUninitialized:
      UNREACHABLE();
    case Expression::kEffect:
      // Nothing to do.
      break;
    case Expression::kValue:
    case Expression::kTest:
    case Expression::kValueTest:
    case Expression::kTestValue:
      // On ARM we have to move the value into a register to do anything
      // with it.
      Move(result_register(), slot);
      Apply(context, result_register());
      break;
  }
}


void FastCodeGenerator::Apply(Expression::Context context, Literal* lit) {
  switch (context) {
    case Expression::kUninitialized:
      UNREACHABLE();
    case Expression::kEffect:
      break;
      // Nothing to do.
    case Expression::kValue:
    case Expression::kTest:
    case Expression::kValueTest:
    case Expression::kTestValue:
      // On ARM we have to move the value into a register to do anything
      // with it.
      __ mov(result_register(), Operand(lit->handle()));
      Apply(context, result_register());
      break;
  }
}


void FastCodeGenerator::ApplyTOS(Expression::Context context) {
  switch (context) {
    case Expression::kUninitialized:
      UNREACHABLE();

    case Expression::kEffect:
      __ Drop(1);
      break;

    case Expression::kValue:
      switch (location_) {
        case kAccumulator:
          __ pop(result_register());
          break;
        case kStack:
          break;
      }
      break;

    case Expression::kValueTest:
    case Expression::kTestValue:
      // Duplicate the value on the stack in case it's needed.
      __ ldr(ip, MemOperand(sp));
      __ push(ip);
      // Fall through.

    case Expression::kTest:
      DoTest(context);
      break;
  }
}


void FastCodeGenerator::DropAndApply(int count,
                                     Expression::Context context,
                                     Register reg) {
  ASSERT(count > 0);
  ASSERT(!reg.is(sp));
  switch (context) {
    case Expression::kUninitialized:
      UNREACHABLE();

    case Expression::kEffect:
      __ Drop(count);
      break;

    case Expression::kValue:
      switch (location_) {
        case kAccumulator:
          __ Drop(count);
          if (!reg.is(result_register())) __ mov(result_register(), reg);
          break;
        case kStack:
          if (count > 1) __ Drop(count - 1);
          __ str(reg, MemOperand(sp));
          break;
      }
      break;

    case Expression::kTest:
      if (count > 1) __ Drop(count - 1);
      __ str(reg, MemOperand(sp));
      DoTest(context);
      break;

    case Expression::kValueTest:
    case Expression::kTestValue:
      if (count == 1) {
        __ str(reg, MemOperand(sp));
        __ push(reg);
      } else {  // count > 1
        __ Drop(count - 2);
        __ str(reg, MemOperand(sp, kPointerSize));
        __ str(reg, MemOperand(sp));
      }
      DoTest(context);
      break;
  }
}


void FastCodeGenerator::Apply(Expression::Context context,
                              Label* materialize_true,
                              Label* materialize_false) {
  switch (context) {
    case Expression::kUninitialized:

    case Expression::kEffect:
      ASSERT_EQ(materialize_true, materialize_false);
      __ bind(materialize_true);
      break;

    case Expression::kValue: {
      Label done;
      __ bind(materialize_true);
      __ mov(result_register(), Operand(Factory::true_value()));
      __ jmp(&done);
      __ bind(materialize_false);
      __ mov(result_register(), Operand(Factory::false_value()));
      __ bind(&done);
      switch (location_) {
        case kAccumulator:
          break;
        case kStack:
          __ push(result_register());
          break;
      }
      break;
    }

    case Expression::kTest:
      break;

    case Expression::kValueTest:
      __ bind(materialize_true);
      __ mov(result_register(), Operand(Factory::true_value()));
      switch (location_) {
        case kAccumulator:
          break;
        case kStack:
          __ push(result_register());
          break;
      }
      __ jmp(true_label_);
      break;

    case Expression::kTestValue:
      __ bind(materialize_false);
      __ mov(result_register(), Operand(Factory::false_value()));
      switch (location_) {
        case kAccumulator:
          break;
        case kStack:
          __ push(result_register());
          break;
      }
      __ jmp(false_label_);
      break;
  }
}


void FastCodeGenerator::DoTest(Expression::Context context) {
  // The value to test is pushed on the stack, and duplicated on the stack
  // if necessary (for value/test and test/value contexts).
  ASSERT_NE(NULL, true_label_);
  ASSERT_NE(NULL, false_label_);

  // Call the runtime to find the boolean value of the source and then
  // translate it into control flow to the pair of labels.
  __ CallRuntime(Runtime::kToBool, 1);
  __ LoadRoot(ip, Heap::kTrueValueRootIndex);
  __ cmp(r0, ip);

  // Complete based on the context.
  switch (context) {
    case Expression::kUninitialized:
    case Expression::kEffect:
    case Expression::kValue:
      UNREACHABLE();

    case Expression::kTest:
      __ b(eq, true_label_);
      __ jmp(false_label_);
      break;

    case Expression::kValueTest: {
      Label discard;
      switch (location_) {
        case kAccumulator:
          __ b(ne, &discard);
          __ pop(result_register());
          __ jmp(true_label_);
          break;
        case kStack:
          __ b(eq, true_label_);
          break;
      }
      __ bind(&discard);
      __ Drop(1);
      __ jmp(false_label_);
      break;
    }

    case Expression::kTestValue: {
      Label discard;
      switch (location_) {
        case kAccumulator:
          __ b(eq, &discard);
          __ pop(result_register());
          __ jmp(false_label_);
          break;
        case kStack:
          __ b(ne, false_label_);
          break;
      }
      __ bind(&discard);
      __ Drop(1);
      __ jmp(true_label_);
      break;
    }
  }
}


MemOperand FastCodeGenerator::EmitSlotSearch(Slot* slot, Register scratch) {
  switch (slot->type()) {
    case Slot::PARAMETER:
    case Slot::LOCAL:
      return MemOperand(fp, SlotOffset(slot));
    case Slot::CONTEXT: {
      int context_chain_length =
          function_->scope()->ContextChainLength(slot->var()->scope());
      __ LoadContext(scratch, context_chain_length);
      return CodeGenerator::ContextOperand(scratch, slot->index());
    }
    case Slot::LOOKUP:
      UNREACHABLE();
  }
  UNREACHABLE();
  return MemOperand(r0, 0);
}


void FastCodeGenerator::Move(Register destination, Slot* source) {
  // Use destination as scratch.
  MemOperand slot_operand = EmitSlotSearch(source, destination);
  __ ldr(destination, slot_operand);
}


void FastCodeGenerator::Move(Slot* dst,
                             Register src,
                             Register scratch1,
                             Register scratch2) {
  ASSERT(dst->type() != Slot::LOOKUP);  // Not yet implemented.
  ASSERT(!scratch1.is(src) && !scratch2.is(src));
  MemOperand location = EmitSlotSearch(dst, scratch1);
  __ str(src, location);
  // Emit the write barrier code if the location is in the heap.
  if (dst->type() == Slot::CONTEXT) {
    __ mov(scratch2, Operand(Context::SlotOffset(dst->index())));
    __ RecordWrite(scratch1, scratch2, src);
  }
}


void FastCodeGenerator::VisitDeclaration(Declaration* decl) {
  Comment cmnt(masm_, "[ Declaration");
  Variable* var = decl->proxy()->var();
  ASSERT(var != NULL);  // Must have been resolved.
  Slot* slot = var->slot();
  Property* prop = var->AsProperty();

  if (slot != NULL) {
    switch (slot->type()) {
      case Slot::PARAMETER:
      case Slot::LOCAL:
        if (decl->mode() == Variable::CONST) {
          __ LoadRoot(ip, Heap::kTheHoleValueRootIndex);
          __ str(ip, MemOperand(fp, SlotOffset(slot)));
        } else if (decl->fun() != NULL) {
          VisitForValue(decl->fun(), kAccumulator);
          __ str(result_register(), MemOperand(fp, SlotOffset(slot)));
        }
        break;

      case Slot::CONTEXT:
        // We bypass the general EmitSlotSearch because we know more about
        // this specific context.

        // The variable in the decl always resides in the current context.
        ASSERT_EQ(0, function_->scope()->ContextChainLength(var->scope()));
        if (FLAG_debug_code) {
          // Check if we have the correct context pointer.
          __ ldr(r1,
                 CodeGenerator::ContextOperand(cp, Context::FCONTEXT_INDEX));
          __ cmp(r1, cp);
          __ Check(eq, "Unexpected declaration in current context.");
        }
        if (decl->mode() == Variable::CONST) {
          __ LoadRoot(ip, Heap::kTheHoleValueRootIndex);
          __ str(ip, CodeGenerator::ContextOperand(cp, slot->index()));
          // No write barrier since the_hole_value is in old space.
        } else if (decl->fun() != NULL) {
          VisitForValue(decl->fun(), kAccumulator);
          __ str(result_register(),
                 CodeGenerator::ContextOperand(cp, slot->index()));
          int offset = Context::SlotOffset(slot->index());
          __ mov(r2, Operand(offset));
          // We know that we have written a function, which is not a smi.
          __ RecordWrite(cp, r2, result_register());
        }
        break;

      case Slot::LOOKUP: {
        __ mov(r2, Operand(var->name()));
        // Declaration nodes are always introduced in one of two modes.
        ASSERT(decl->mode() == Variable::VAR ||
               decl->mode() == Variable::CONST);
        PropertyAttributes attr =
            (decl->mode() == Variable::VAR) ? NONE : READ_ONLY;
        __ mov(r1, Operand(Smi::FromInt(attr)));
        // Push initial value, if any.
        // Note: For variables we must not push an initial value (such as
        // 'undefined') because we may have a (legal) redeclaration and we
        // must not destroy the current value.
        if (decl->mode() == Variable::CONST) {
          __ LoadRoot(r0, Heap::kTheHoleValueRootIndex);
          __ stm(db_w, sp, cp.bit() | r2.bit() | r1.bit() | r0.bit());
        } else if (decl->fun() != NULL) {
          __ stm(db_w, sp, cp.bit() | r2.bit() | r1.bit());
          // Push initial value for function declaration.
          VisitForValue(decl->fun(), kStack);
        } else {
          __ mov(r0, Operand(Smi::FromInt(0)));  // No initial value!
          __ stm(db_w, sp, cp.bit() | r2.bit() | r1.bit() | r0.bit());
        }
        __ CallRuntime(Runtime::kDeclareContextSlot, 4);
        break;
      }
    }

  } else if (prop != NULL) {
    if (decl->fun() != NULL || decl->mode() == Variable::CONST) {
      // We are declaring a function or constant that rewrites to a
      // property.  Use (keyed) IC to set the initial value.
      VisitForValue(prop->obj(), kStack);
      VisitForValue(prop->key(), kStack);

      if (decl->fun() != NULL) {
        VisitForValue(decl->fun(), kAccumulator);
      } else {
        __ LoadRoot(result_register(), Heap::kTheHoleValueRootIndex);
      }

      Handle<Code> ic(Builtins::builtin(Builtins::KeyedStoreIC_Initialize));
      __ Call(ic, RelocInfo::CODE_TARGET);

      // Value in r0 is ignored (declarations are statements).  Receiver
      // and key on stack are discarded.
      __ Drop(2);
    }
  }
}


void FastCodeGenerator::DeclareGlobals(Handle<FixedArray> pairs) {
  // Call the runtime to declare the globals.
  // The context is the first argument.
  __ mov(r1, Operand(pairs));
  __ mov(r0, Operand(Smi::FromInt(is_eval_ ? 1 : 0)));
  __ stm(db_w, sp, cp.bit() | r1.bit() | r0.bit());
  __ CallRuntime(Runtime::kDeclareGlobals, 3);
  // Return value is ignored.
}


void FastCodeGenerator::VisitFunctionLiteral(FunctionLiteral* expr) {
  Comment cmnt(masm_, "[ FunctionLiteral");

  // Build the function boilerplate and instantiate it.
  Handle<JSFunction> boilerplate =
      Compiler::BuildBoilerplate(expr, script_, this);
  if (HasStackOverflow()) return;

  ASSERT(boilerplate->IsBoilerplate());

  // Create a new closure.
  __ mov(r0, Operand(boilerplate));
  __ stm(db_w, sp, cp.bit() | r0.bit());
  __ CallRuntime(Runtime::kNewClosure, 2);
  Apply(expr->context(), r0);
}


void FastCodeGenerator::VisitVariableProxy(VariableProxy* expr) {
  Comment cmnt(masm_, "[ VariableProxy");
  EmitVariableLoad(expr->var(), expr->context());
}


void FastCodeGenerator::EmitVariableLoad(Variable* var,
                                         Expression::Context context) {
  Expression* rewrite = var->rewrite();
  if (rewrite == NULL) {
    ASSERT(var->is_global());
    Comment cmnt(masm_, "Global variable");
    // Use inline caching. Variable name is passed in r2 and the global
    // object on the stack.
    __ ldr(ip, CodeGenerator::GlobalObject());
    __ push(ip);
    __ mov(r2, Operand(var->name()));
    Handle<Code> ic(Builtins::builtin(Builtins::LoadIC_Initialize));
    __ Call(ic, RelocInfo::CODE_TARGET_CONTEXT);
    DropAndApply(1, context, r0);
  } else if (rewrite->AsSlot() != NULL) {
    Slot* slot = rewrite->AsSlot();
    if (FLAG_debug_code) {
      switch (slot->type()) {
        case Slot::PARAMETER:
        case Slot::LOCAL: {
          Comment cmnt(masm_, "Stack slot");
          break;
        }
        case Slot::CONTEXT: {
          Comment cmnt(masm_, "Context slot");
          break;
        }
        case Slot::LOOKUP:
          UNIMPLEMENTED();
          break;
      }
    }
    Apply(context, slot);
  } else {
    Comment cmnt(masm_, "Variable rewritten to property");
    // A variable has been rewritten into an explicit access to an object
    // property.
    Property* property = rewrite->AsProperty();
    ASSERT_NOT_NULL(property);

    // The only property expressions that can occur are of the form
    // "slot[literal]".

    // Assert that the object is in a slot.
    Variable* object_var = property->obj()->AsVariableProxy()->AsVariable();
    ASSERT_NOT_NULL(object_var);
    Slot* object_slot = object_var->slot();
    ASSERT_NOT_NULL(object_slot);

    // Load the object.
    Move(r2, object_slot);

    // Assert that the key is a smi.
    Literal* key_literal = property->key()->AsLiteral();
    ASSERT_NOT_NULL(key_literal);
    ASSERT(key_literal->handle()->IsSmi());

    // Load the key.
    __ mov(r1, Operand(key_literal->handle()));

    // Push both as arguments to ic.
    __ stm(db_w, sp, r2.bit() | r1.bit());

    // Do a keyed property load.
    Handle<Code> ic(Builtins::builtin(Builtins::KeyedLoadIC_Initialize));
    __ Call(ic, RelocInfo::CODE_TARGET);

    // Drop key and object left on the stack by IC, and push the result.
    DropAndApply(2, context, r0);
  }
}


void FastCodeGenerator::VisitRegExpLiteral(RegExpLiteral* expr) {
  Comment cmnt(masm_, "[ RegExpLiteral");
  Label done;
  // Registers will be used as follows:
  // r4 = JS function, literals array
  // r3 = literal index
  // r2 = RegExp pattern
  // r1 = RegExp flags
  // r0 = temp + return value (RegExp literal)
  __ ldr(r0, MemOperand(fp,  JavaScriptFrameConstants::kFunctionOffset));
  __ ldr(r4,  FieldMemOperand(r0, JSFunction::kLiteralsOffset));
  int literal_offset =
    FixedArray::kHeaderSize + expr->literal_index() * kPointerSize;
  __ ldr(r0, FieldMemOperand(r4, literal_offset));
  __ LoadRoot(ip, Heap::kUndefinedValueRootIndex);
  __ cmp(r0, ip);
  __ b(ne, &done);
  __ mov(r3, Operand(Smi::FromInt(expr->literal_index())));
  __ mov(r2, Operand(expr->pattern()));
  __ mov(r1, Operand(expr->flags()));
  __ stm(db_w, sp, r4.bit() | r3.bit() | r2.bit() | r1.bit());
  __ CallRuntime(Runtime::kMaterializeRegExpLiteral, 4);
  __ bind(&done);
  Apply(expr->context(), r0);
}


void FastCodeGenerator::VisitObjectLiteral(ObjectLiteral* expr) {
  Comment cmnt(masm_, "[ ObjectLiteral");
  __ ldr(r2, MemOperand(fp,  JavaScriptFrameConstants::kFunctionOffset));
  __ ldr(r2, FieldMemOperand(r2, JSFunction::kLiteralsOffset));
  __ mov(r1, Operand(Smi::FromInt(expr->literal_index())));
  __ mov(r0, Operand(expr->constant_properties()));
  __ stm(db_w, sp, r2.bit() | r1.bit() | r0.bit());
  if (expr->depth() > 1) {
    __ CallRuntime(Runtime::kCreateObjectLiteral, 3);
  } else {
    __ CallRuntime(Runtime::kCreateObjectLiteralShallow, 3);
  }

  // If result_saved is true the result is on top of the stack.  If
  // result_saved is false the result is in r0.
  bool result_saved = false;

  for (int i = 0; i < expr->properties()->length(); i++) {
    ObjectLiteral::Property* property = expr->properties()->at(i);
    if (property->IsCompileTimeValue()) continue;

    Literal* key = property->key();
    Expression* value = property->value();
    if (!result_saved) {
      __ push(r0);  // Save result on stack
      result_saved = true;
    }
    switch (property->kind()) {
      case ObjectLiteral::Property::CONSTANT:
        UNREACHABLE();
      case ObjectLiteral::Property::MATERIALIZED_LITERAL:
        ASSERT(!CompileTimeValue::IsCompileTimeValue(property->value()));
        // Fall through.
      case ObjectLiteral::Property::COMPUTED:
        if (key->handle()->IsSymbol()) {
          VisitForValue(value, kAccumulator);
          __ mov(r2, Operand(key->handle()));
          Handle<Code> ic(Builtins::builtin(Builtins::StoreIC_Initialize));
          __ Call(ic, RelocInfo::CODE_TARGET);
          // StoreIC leaves the receiver on the stack.
          break;
        }
        // Fall through.
      case ObjectLiteral::Property::PROTOTYPE:
        // Duplicate receiver on stack.
        __ ldr(r0, MemOperand(sp));
        __ push(r0);
        VisitForValue(key, kStack);
        VisitForValue(value, kStack);
        __ CallRuntime(Runtime::kSetProperty, 3);
        break;
      case ObjectLiteral::Property::GETTER:
      case ObjectLiteral::Property::SETTER:
        // Duplicate receiver on stack.
        __ ldr(r0, MemOperand(sp));
        __ push(r0);
        VisitForValue(key, kStack);
        __ mov(r1, Operand(property->kind() == ObjectLiteral::Property::SETTER ?
                           Smi::FromInt(1) :
                           Smi::FromInt(0)));
        __ push(r1);
        VisitForValue(value, kStack);
        __ CallRuntime(Runtime::kDefineAccessor, 4);
        break;
    }
  }

  if (result_saved) {
    ApplyTOS(expr->context());
  } else {
    Apply(expr->context(), r0);
  }
}


void FastCodeGenerator::VisitArrayLiteral(ArrayLiteral* expr) {
  Comment cmnt(masm_, "[ ArrayLiteral");
  __ ldr(r3, MemOperand(fp, JavaScriptFrameConstants::kFunctionOffset));
  __ ldr(r3, FieldMemOperand(r3, JSFunction::kLiteralsOffset));
  __ mov(r2, Operand(Smi::FromInt(expr->literal_index())));
  __ mov(r1, Operand(expr->constant_elements()));
  __ stm(db_w, sp, r3.bit() | r2.bit() | r1.bit());
  if (expr->depth() > 1) {
    __ CallRuntime(Runtime::kCreateArrayLiteral, 3);
  } else {
    __ CallRuntime(Runtime::kCreateArrayLiteralShallow, 3);
  }

  bool result_saved = false;  // Is the result saved to the stack?

  // Emit code to evaluate all the non-constant subexpressions and to store
  // them into the newly cloned array.
  ZoneList<Expression*>* subexprs = expr->values();
  for (int i = 0, len = subexprs->length(); i < len; i++) {
    Expression* subexpr = subexprs->at(i);
    // If the subexpression is a literal or a simple materialized literal it
    // is already set in the cloned array.
    if (subexpr->AsLiteral() != NULL ||
        CompileTimeValue::IsCompileTimeValue(subexpr)) {
      continue;
    }

    if (!result_saved) {
      __ push(r0);
      result_saved = true;
    }
    VisitForValue(subexpr, kAccumulator);

    // Store the subexpression value in the array's elements.
    __ ldr(r1, MemOperand(sp));  // Copy of array literal.
    __ ldr(r1, FieldMemOperand(r1, JSObject::kElementsOffset));
    int offset = FixedArray::kHeaderSize + (i * kPointerSize);
    __ str(result_register(), FieldMemOperand(r1, offset));

    // Update the write barrier for the array store with r0 as the scratch
    // register.
    __ mov(r2, Operand(offset));
    __ RecordWrite(r1, r2, result_register());
  }

  if (result_saved) {
    ApplyTOS(expr->context());
  } else {
    Apply(expr->context(), r0);
  }
}


void FastCodeGenerator::EmitNamedPropertyLoad(Property* prop) {
  SetSourcePosition(prop->position());
  Literal* key = prop->key()->AsLiteral();
  __ mov(r2, Operand(key->handle()));
  Handle<Code> ic(Builtins::builtin(Builtins::LoadIC_Initialize));
  __ Call(ic, RelocInfo::CODE_TARGET);
}


void FastCodeGenerator::EmitKeyedPropertyLoad(Property* prop) {
  SetSourcePosition(prop->position());
  Handle<Code> ic(Builtins::builtin(Builtins::KeyedLoadIC_Initialize));
  __ Call(ic, RelocInfo::CODE_TARGET);
}


void FastCodeGenerator::EmitBinaryOp(Token::Value op,
                                     Expression::Context context) {
  __ pop(r1);
  GenericBinaryOpStub stub(op, NO_OVERWRITE);
  __ CallStub(&stub);
  Apply(context, r0);
}


void FastCodeGenerator::EmitVariableAssignment(Variable* var,
                                               Expression::Context context) {
  ASSERT(var != NULL);
  ASSERT(var->is_global() || var->slot() != NULL);
  if (var->is_global()) {
    // Assignment to a global variable.  Use inline caching for the
    // assignment.  Right-hand-side value is passed in r0, variable name in
    // r2, and the global object on the stack.
    __ mov(r2, Operand(var->name()));
    __ ldr(ip, CodeGenerator::GlobalObject());
    __ push(ip);
    Handle<Code> ic(Builtins::builtin(Builtins::StoreIC_Initialize));
    __ Call(ic, RelocInfo::CODE_TARGET);
    // Overwrite the global object on the stack with the result if needed.
    DropAndApply(1, context, r0);

  } else if (var->slot() != NULL) {
    Slot* slot = var->slot();
    switch (slot->type()) {
      case Slot::LOCAL:
      case Slot::PARAMETER:
        __ str(result_register(), MemOperand(fp, SlotOffset(slot)));
        break;

      case Slot::CONTEXT: {
        MemOperand target = EmitSlotSearch(slot, r1);
        __ str(result_register(), target);

        // RecordWrite may destroy all its register arguments.
        __ mov(r3, result_register());
        int offset = FixedArray::kHeaderSize + slot->index() * kPointerSize;

        __ mov(r2, Operand(offset));
        __ RecordWrite(r1, r2, r3);
        break;
      }

      case Slot::LOOKUP:
        UNREACHABLE();
        break;
    }
    Apply(context, result_register());
  } else {
    // Variables rewritten as properties are not treated as variables in
    // assignments.
    UNREACHABLE();
  }
}


void FastCodeGenerator::EmitNamedPropertyAssignment(Assignment* expr) {
  // Assignment to a property, using a named store IC.
  Property* prop = expr->target()->AsProperty();
  ASSERT(prop != NULL);
  ASSERT(prop->key()->AsLiteral() != NULL);

  // If the assignment starts a block of assignments to the same object,
  // change to slow case to avoid the quadratic behavior of repeatedly
  // adding fast properties.
  if (expr->starts_initialization_block()) {
    __ push(result_register());
    __ ldr(ip, MemOperand(sp, kPointerSize));  // Receiver is now under value.
    __ push(ip);
    __ CallRuntime(Runtime::kToSlowProperties, 1);
    __ pop(result_register());
  }

  // Record source code position before IC call.
  SetSourcePosition(expr->position());
  __ mov(r2, Operand(prop->key()->AsLiteral()->handle()));
  Handle<Code> ic(Builtins::builtin(Builtins::StoreIC_Initialize));
  __ Call(ic, RelocInfo::CODE_TARGET);

  // If the assignment ends an initialization block, revert to fast case.
  if (expr->ends_initialization_block()) {
    __ push(r0);  // Result of assignment, saved even if not needed.
    __ ldr(ip, MemOperand(sp, kPointerSize));  // Receiver is under value.
    __ push(ip);
    __ CallRuntime(Runtime::kToFastProperties, 1);
    __ pop(r0);
  }

  DropAndApply(1, expr->context(), r0);
}


void FastCodeGenerator::EmitKeyedPropertyAssignment(Assignment* expr) {
  // Assignment to a property, using a keyed store IC.

  // If the assignment starts a block of assignments to the same object,
  // change to slow case to avoid the quadratic behavior of repeatedly
  // adding fast properties.
  if (expr->starts_initialization_block()) {
    __ push(result_register());
    // Receiver is now under the key and value.
    __ ldr(ip, MemOperand(sp, 2 * kPointerSize));
    __ push(ip);
    __ CallRuntime(Runtime::kToSlowProperties, 1);
    __ pop(result_register());
  }

  // Record source code position before IC call.
  SetSourcePosition(expr->position());
  Handle<Code> ic(Builtins::builtin(Builtins::KeyedStoreIC_Initialize));
  __ Call(ic, RelocInfo::CODE_TARGET);

  // If the assignment ends an initialization block, revert to fast case.
  if (expr->ends_initialization_block()) {
    __ push(r0);  // Result of assignment, saved even if not needed.
    // Receiver is under the key and value.
    __ ldr(ip, MemOperand(sp, 2 * kPointerSize));
    __ push(ip);
    __ CallRuntime(Runtime::kToFastProperties, 1);
    __ pop(r0);
  }

  // Receiver and key are still on stack.
  DropAndApply(2, expr->context(), r0);
}


void FastCodeGenerator::VisitProperty(Property* expr) {
  Comment cmnt(masm_, "[ Property");
  Expression* key = expr->key();

  // Evaluate receiver.
  VisitForValue(expr->obj(), kStack);

  if (key->IsPropertyName()) {
    EmitNamedPropertyLoad(expr);
    // Drop receiver left on the stack by IC.
    DropAndApply(1, expr->context(), r0);
  } else {
    VisitForValue(expr->key(), kStack);
    EmitKeyedPropertyLoad(expr);
    // Drop key and receiver left on the stack by IC.
    DropAndApply(2, expr->context(), r0);
  }
}

void FastCodeGenerator::EmitCallWithIC(Call* expr,
                                       Handle<Object> ignored,
                                       RelocInfo::Mode mode) {
  // Code common for calls using the IC.
  ZoneList<Expression*>* args = expr->arguments();
  int arg_count = args->length();
  for (int i = 0; i < arg_count; i++) {
    VisitForValue(args->at(i), kStack);
  }
  // Record source position for debugger.
  SetSourcePosition(expr->position());
  // Call the IC initialization code.
  Handle<Code> ic = CodeGenerator::ComputeCallInitialize(arg_count,
                                                         NOT_IN_LOOP);
  __ Call(ic, mode);
  // Restore context register.
  __ ldr(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
  // Discard the function left on TOS.
  DropAndApply(1, expr->context(), r0);
}


void FastCodeGenerator::EmitCallWithStub(Call* expr) {
  // Code common for calls using the call stub.
  ZoneList<Expression*>* args = expr->arguments();
  int arg_count = args->length();
  for (int i = 0; i < arg_count; i++) {
    VisitForValue(args->at(i), kStack);
  }
  // Record source position for debugger.
  SetSourcePosition(expr->position());
  CallFunctionStub stub(arg_count, NOT_IN_LOOP);
  __ CallStub(&stub);
  // Restore context register.
  __ ldr(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
  // Discard the function left on TOS.
  DropAndApply(1, expr->context(), r0);
}


void FastCodeGenerator::VisitCall(Call* expr) {
  Comment cmnt(masm_, "[ Call");
  Expression* fun = expr->expression();
  Variable* var = fun->AsVariableProxy()->AsVariable();

  if (var != NULL && var->is_possibly_eval()) {
    // Call to the identifier 'eval'.
    UNREACHABLE();
  } else if (var != NULL && !var->is_this() && var->is_global()) {
    // Call to a global variable.
    __ mov(r1, Operand(var->name()));
    // Push global object as receiver for the call IC lookup.
    __ ldr(r0, CodeGenerator::GlobalObject());
    __ stm(db_w, sp, r1.bit() | r0.bit());
    EmitCallWithIC(expr, var->name(), RelocInfo::CODE_TARGET_CONTEXT);
  } else if (var != NULL && var->slot() != NULL &&
             var->slot()->type() == Slot::LOOKUP) {
    // Call to a lookup slot.
    UNREACHABLE();
  } else if (fun->AsProperty() != NULL) {
    // Call to an object property.
    Property* prop = fun->AsProperty();
    Literal* key = prop->key()->AsLiteral();
    if (key != NULL && key->handle()->IsSymbol()) {
      // Call to a named property, use call IC.
      __ mov(r0, Operand(key->handle()));
      __ push(r0);
      VisitForValue(prop->obj(), kStack);
      EmitCallWithIC(expr, key->handle(), RelocInfo::CODE_TARGET);
    } else {
      // Call to a keyed property, use keyed load IC followed by function
      // call.
      VisitForValue(prop->obj(), kStack);
      VisitForValue(prop->key(), kStack);
      // Record source code position for IC call.
      SetSourcePosition(prop->position());
      Handle<Code> ic(Builtins::builtin(Builtins::KeyedLoadIC_Initialize));
      __ Call(ic, RelocInfo::CODE_TARGET);
      // Load receiver object into r1.
      if (prop->is_synthetic()) {
        __ ldr(r1, CodeGenerator::GlobalObject());
        __ ldr(r1, FieldMemOperand(r1, GlobalObject::kGlobalReceiverOffset));
      } else {
        __ ldr(r1, MemOperand(sp, kPointerSize));
      }
      // Overwrite (object, key) with (function, receiver).
      __ str(r0, MemOperand(sp, kPointerSize));
      __ str(r1, MemOperand(sp));
      EmitCallWithStub(expr);
    }
  } else {
    // Call to some other expression.  If the expression is an anonymous
    // function literal not called in a loop, mark it as one that should
    // also use the fast code generator.
    FunctionLiteral* lit = fun->AsFunctionLiteral();
    if (lit != NULL &&
        lit->name()->Equals(Heap::empty_string()) &&
        loop_depth() == 0) {
      lit->set_try_fast_codegen(true);
    }
    VisitForValue(fun, kStack);
    // Load global receiver object.
    __ ldr(r1, CodeGenerator::GlobalObject());
    __ ldr(r1, FieldMemOperand(r1, GlobalObject::kGlobalReceiverOffset));
    __ push(r1);
    // Emit function call.
    EmitCallWithStub(expr);
  }
}


void FastCodeGenerator::VisitCallNew(CallNew* expr) {
  Comment cmnt(masm_, "[ CallNew");
  // According to ECMA-262, section 11.2.2, page 44, the function
  // expression in new calls must be evaluated before the
  // arguments.
  // Push function on the stack.
  VisitForValue(expr->expression(), kStack);

  // Push global object (receiver).
  __ ldr(r0, CodeGenerator::GlobalObject());
  __ push(r0);
  // Push the arguments ("left-to-right") on the stack.
  ZoneList<Expression*>* args = expr->arguments();
  int arg_count = args->length();
  for (int i = 0; i < arg_count; i++) {
    VisitForValue(args->at(i), kStack);
  }

  // Call the construct call builtin that handles allocation and
  // constructor invocation.
  SetSourcePosition(expr->position());

  // Load function, arg_count into r1 and r0.
  __ mov(r0, Operand(arg_count));
  // Function is in sp[arg_count + 1].
  __ ldr(r1, MemOperand(sp, (arg_count + 1) * kPointerSize));

  Handle<Code> construct_builtin(Builtins::builtin(Builtins::JSConstructCall));
  __ Call(construct_builtin, RelocInfo::CONSTRUCT_CALL);

  // Replace function on TOS with result in r0, or pop it.
  DropAndApply(1, expr->context(), r0);
}


void FastCodeGenerator::VisitCallRuntime(CallRuntime* expr) {
  Comment cmnt(masm_, "[ CallRuntime");
  ZoneList<Expression*>* args = expr->arguments();

  if (expr->is_jsruntime()) {
    // Prepare for calling JS runtime function.
    __ mov(r1, Operand(expr->name()));
    __ ldr(r0, CodeGenerator::GlobalObject());
    __ ldr(r0, FieldMemOperand(r0, GlobalObject::kBuiltinsOffset));
    __ stm(db_w, sp, r1.bit() | r0.bit());
  }

  // Push the arguments ("left-to-right").
  int arg_count = args->length();
  for (int i = 0; i < arg_count; i++) {
    VisitForValue(args->at(i), kStack);
  }

  if (expr->is_jsruntime()) {
    // Call the JS runtime function.
    Handle<Code> ic = CodeGenerator::ComputeCallInitialize(arg_count,
                                                           NOT_IN_LOOP);
    __ Call(ic, RelocInfo::CODE_TARGET);
    // Restore context register.
    __ ldr(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
    // Discard the function left on TOS.
    DropAndApply(1, expr->context(), r0);
  } else {
    // Call the C runtime function.
    __ CallRuntime(expr->function(), arg_count);
    Apply(expr->context(), r0);
  }
}


void FastCodeGenerator::VisitUnaryOperation(UnaryOperation* expr) {
  switch (expr->op()) {
    case Token::VOID: {
      Comment cmnt(masm_, "[ UnaryOperation (VOID)");
      ASSERT_EQ(Expression::kEffect, expr->expression()->context());
      Visit(expr->expression());
      switch (expr->context()) {
        case Expression::kUninitialized:
          UNREACHABLE();
          break;
        case Expression::kEffect:
          break;
        case Expression::kValue:
          __ LoadRoot(result_register(), Heap::kUndefinedValueRootIndex);
          switch (location_) {
            case kAccumulator:
              break;
            case kStack:
              __ push(result_register());
              break;
          }
          break;
        case Expression::kTestValue:
          // Value is false so it's needed.
          __ LoadRoot(result_register(), Heap::kUndefinedValueRootIndex);
          switch (location_) {
            case kAccumulator:
              break;
            case kStack:
              __ push(result_register());
              break;
          }
          // Fall through.
        case Expression::kTest:
        case Expression::kValueTest:
          __ jmp(false_label_);
          break;
      }
      break;
    }

    case Token::NOT: {
      Comment cmnt(masm_, "[ UnaryOperation (NOT)");
      ASSERT_EQ(Expression::kTest, expr->expression()->context());

      Label materialize_true, materialize_false, done;
      // Initially assume a pure test context.  Notice that the labels are
      // swapped.
      Label* if_true = false_label_;
      Label* if_false = true_label_;
      switch (expr->context()) {
        case Expression::kUninitialized:
          UNREACHABLE();
          break;
        case Expression::kEffect:
          if_true = &done;
          if_false = &done;
          break;
        case Expression::kValue:
          if_true = &materialize_false;
          if_false = &materialize_true;
          break;
        case Expression::kTest:
          break;
        case Expression::kValueTest:
          if_false = &materialize_true;
          break;
        case Expression::kTestValue:
          if_true = &materialize_false;
          break;
      }
      VisitForControl(expr->expression(), if_true, if_false);
      Apply(expr->context(), if_false, if_true);  // Labels swapped.
      break;
    }

    case Token::TYPEOF: {
      Comment cmnt(masm_, "[ UnaryOperation (TYPEOF)");
      ASSERT_EQ(Expression::kValue, expr->expression()->context());

      VariableProxy* proxy = expr->expression()->AsVariableProxy();
      if (proxy != NULL &&
          !proxy->var()->is_this() &&
          proxy->var()->is_global()) {
        Comment cmnt(masm_, "Global variable");
        __ ldr(r0, CodeGenerator::GlobalObject());
        __ push(r0);
        __ mov(r2, Operand(proxy->name()));
        Handle<Code> ic(Builtins::builtin(Builtins::LoadIC_Initialize));
        // Use a regular load, not a contextual load, to avoid a reference
        // error.
        __ Call(ic, RelocInfo::CODE_TARGET);
        __ str(r0, MemOperand(sp));
      } else if (proxy != NULL &&
                 proxy->var()->slot() != NULL &&
                 proxy->var()->slot()->type() == Slot::LOOKUP) {
        __ mov(r0, Operand(proxy->name()));
        __ stm(db_w, sp, cp.bit() | r0.bit());
        __ CallRuntime(Runtime::kLoadContextSlotNoReferenceError, 2);
        __ push(r0);
      } else {
        // This expression cannot throw a reference error at the top level.
        VisitForValue(expr->expression(), kStack);
      }

      __ CallRuntime(Runtime::kTypeof, 1);
      Apply(expr->context(), r0);
      break;
    }

    default:
      UNREACHABLE();
  }
}


void FastCodeGenerator::VisitCountOperation(CountOperation* expr) {
  Comment cmnt(masm_, "[ CountOperation");

  // Expression can only be a property, a global or a (parameter or local)
  // slot. Variables with rewrite to .arguments are treated as KEYED_PROPERTY.
  enum LhsKind { VARIABLE, NAMED_PROPERTY, KEYED_PROPERTY };
  LhsKind assign_type = VARIABLE;
  Property* prop = expr->expression()->AsProperty();
  // In case of a property we use the uninitialized expression context
  // of the key to detect a named property.
  if (prop != NULL) {
    assign_type = (prop->key()->context() == Expression::kUninitialized)
        ? NAMED_PROPERTY
        : KEYED_PROPERTY;
  }

  // Evaluate expression and get value.
  if (assign_type == VARIABLE) {
    ASSERT(expr->expression()->AsVariableProxy()->var() != NULL);
    Location saved_location = location_;
    location_ = kStack;
    EmitVariableLoad(expr->expression()->AsVariableProxy()->var(),
                     Expression::kValue);
    location_ = saved_location;
  } else {
    // Reserve space for result of postfix operation.
    if (expr->is_postfix() && expr->context() != Expression::kEffect) {
      ASSERT(expr->context() != Expression::kUninitialized);
      __ mov(ip, Operand(Smi::FromInt(0)));
      __ push(ip);
    }
    VisitForValue(prop->obj(), kStack);
    if (assign_type == NAMED_PROPERTY) {
      EmitNamedPropertyLoad(prop);
    } else {
      VisitForValue(prop->key(), kStack);
      EmitKeyedPropertyLoad(prop);
    }
    __ push(r0);
  }

  // Convert to number.
  __ InvokeBuiltin(Builtins::TO_NUMBER, CALL_JS);

  // Save result for postfix expressions.
  if (expr->is_postfix()) {
    switch (expr->context()) {
      case Expression::kUninitialized:
        UNREACHABLE();
      case Expression::kEffect:
        // Do not save result.
        break;
      case Expression::kValue:
      case Expression::kTest:
      case Expression::kValueTest:
      case Expression::kTestValue:
        // Save the result on the stack. If we have a named or keyed property
        // we store the result under the receiver that is currently on top
        // of the stack.
        switch (assign_type) {
          case VARIABLE:
            __ push(r0);
            break;
          case NAMED_PROPERTY:
            __ str(r0, MemOperand(sp, kPointerSize));
            break;
          case KEYED_PROPERTY:
            __ str(r0, MemOperand(sp, 2 * kPointerSize));
            break;
        }
        break;
    }
  }

  // Call stub for +1/-1.
  __ mov(r1, Operand(expr->op() == Token::INC
                     ? Smi::FromInt(1)
                     : Smi::FromInt(-1)));
  GenericBinaryOpStub stub(Token::ADD, NO_OVERWRITE);
  __ CallStub(&stub);

  // Store the value returned in r0.
  switch (assign_type) {
    case VARIABLE:
      if (expr->is_postfix()) {
        EmitVariableAssignment(expr->expression()->AsVariableProxy()->var(),
                               Expression::kEffect);
        // For all contexts except kEffect: We have the result on
        // top of the stack.
        if (expr->context() != Expression::kEffect) {
          ApplyTOS(expr->context());
        }
      } else {
        EmitVariableAssignment(expr->expression()->AsVariableProxy()->var(),
                               expr->context());
      }
      break;
    case NAMED_PROPERTY: {
      __ mov(r2, Operand(prop->key()->AsLiteral()->handle()));
      Handle<Code> ic(Builtins::builtin(Builtins::StoreIC_Initialize));
      __ Call(ic, RelocInfo::CODE_TARGET);
      if (expr->is_postfix()) {
        __ Drop(1);  // Result is on the stack under the receiver.
        if (expr->context() != Expression::kEffect) {
          ApplyTOS(expr->context());
        }
      } else {
        DropAndApply(1, expr->context(), r0);
      }
      break;
    }
    case KEYED_PROPERTY: {
      Handle<Code> ic(Builtins::builtin(Builtins::KeyedStoreIC_Initialize));
      __ Call(ic, RelocInfo::CODE_TARGET);
      if (expr->is_postfix()) {
        __ Drop(2);  // Result is on the stack under the key and the receiver.
        if (expr->context() != Expression::kEffect) {
          ApplyTOS(expr->context());
        }
      } else {
        DropAndApply(2, expr->context(), r0);
      }
      break;
    }
  }
}


void FastCodeGenerator::VisitBinaryOperation(BinaryOperation* expr) {
  Comment cmnt(masm_, "[ BinaryOperation");
  switch (expr->op()) {
    case Token::COMMA:
      ASSERT_EQ(Expression::kEffect, expr->left()->context());
      ASSERT_EQ(expr->context(), expr->right()->context());
      Visit(expr->left());
      Visit(expr->right());
      break;

    case Token::OR:
    case Token::AND:
      EmitLogicalOperation(expr);
      break;

    case Token::ADD:
    case Token::SUB:
    case Token::DIV:
    case Token::MOD:
    case Token::MUL:
    case Token::BIT_OR:
    case Token::BIT_AND:
    case Token::BIT_XOR:
    case Token::SHL:
    case Token::SHR:
    case Token::SAR:
      VisitForValue(expr->left(), kStack);
      VisitForValue(expr->right(), kAccumulator);
      EmitBinaryOp(expr->op(), expr->context());
      break;

    default:
      UNREACHABLE();
  }
}


void FastCodeGenerator::VisitCompareOperation(CompareOperation* expr) {
  Comment cmnt(masm_, "[ CompareOperation");

  // Always perform the comparison for its control flow.  Pack the result
  // into the expression's context after the comparison is performed.
  Label materialize_true, materialize_false, done;
  // Initially assume we are in a test context.
  Label* if_true = true_label_;
  Label* if_false = false_label_;
  switch (expr->context()) {
    case Expression::kUninitialized:
      UNREACHABLE();
      break;
    case Expression::kEffect:
      if_true = &done;
      if_false = &done;
      break;
    case Expression::kValue:
      if_true = &materialize_true;
      if_false = &materialize_false;
      break;
    case Expression::kTest:
      break;
    case Expression::kValueTest:
      if_true = &materialize_true;
      break;
    case Expression::kTestValue:
      if_false = &materialize_false;
      break;
  }

  VisitForValue(expr->left(), kStack);
  switch (expr->op()) {
    case Token::IN:
      VisitForValue(expr->right(), kStack);
      __ InvokeBuiltin(Builtins::IN, CALL_JS);
      __ LoadRoot(ip, Heap::kTrueValueRootIndex);
      __ cmp(r0, ip);
      __ b(eq, if_true);
      __ jmp(if_false);
      break;

    case Token::INSTANCEOF: {
      VisitForValue(expr->right(), kStack);
      InstanceofStub stub;
      __ CallStub(&stub);
      __ tst(r0, r0);
      __ b(eq, if_true);  // The stub returns 0 for true.
      __ jmp(if_false);
      break;
    }

    default: {
      VisitForValue(expr->right(), kAccumulator);
      Condition cc = eq;
      bool strict = false;
      switch (expr->op()) {
        case Token::EQ_STRICT:
          strict = true;
          // Fall through
        case Token::EQ:
          cc = eq;
          __ pop(r1);
          break;
        case Token::LT:
          cc = lt;
          __ pop(r1);
          break;
        case Token::GT:
          // Reverse left and right sizes to obtain ECMA-262 conversion order.
          cc = lt;
          __ mov(r1, result_register());
          __ pop(r0);
         break;
        case Token::LTE:
          // Reverse left and right sizes to obtain ECMA-262 conversion order.
          cc = ge;
          __ mov(r1, result_register());
          __ pop(r0);
          break;
        case Token::GTE:
          cc = ge;
          __ pop(r1);
          break;
        case Token::IN:
        case Token::INSTANCEOF:
        default:
          UNREACHABLE();
      }

      // The comparison stub expects the smi vs. smi case to be handled
      // before it is called.
      Label slow_case;
      __ orr(r2, r0, Operand(r1));
      __ tst(r2, Operand(kSmiTagMask));
      __ b(ne, &slow_case);
      __ cmp(r1, r0);
      __ b(cc, if_true);
      __ jmp(if_false);

      __ bind(&slow_case);
      CompareStub stub(cc, strict);
      __ CallStub(&stub);
      __ tst(r0, r0);
      __ b(cc, if_true);
      __ jmp(if_false);
    }
  }

  // Convert the result of the comparison into one expected for this
  // expression's context.
  Apply(expr->context(), if_true, if_false);
}


void FastCodeGenerator::VisitThisFunction(ThisFunction* expr) {
  __ ldr(r0, MemOperand(fp, JavaScriptFrameConstants::kFunctionOffset));
  Apply(expr->context(), r0);
}


Register FastCodeGenerator::result_register() { return r0; }


Register FastCodeGenerator::context_register() { return cp; }


void FastCodeGenerator::StoreToFrameField(int frame_offset, Register value) {
  ASSERT_EQ(POINTER_SIZE_ALIGN(frame_offset), frame_offset);
  __ str(value, MemOperand(fp, frame_offset));
}


void FastCodeGenerator::LoadContextField(Register dst, int context_index) {
  __ ldr(dst, CodeGenerator::ContextOperand(cp, context_index));
}


// ----------------------------------------------------------------------------
// Non-local control flow support.

void FastCodeGenerator::EnterFinallyBlock() {
  ASSERT(!result_register().is(r1));
  // Store result register while executing finally block.
  __ push(result_register());
  // Cook return address in link register to stack (smi encoded Code* delta)
  __ sub(r1, lr, Operand(masm_->CodeObject()));
  ASSERT_EQ(1, kSmiTagSize + kSmiShiftSize);
  ASSERT_EQ(0, kSmiTag);
  __ add(r1, r1, Operand(r1));  // Convert to smi.
  __ push(r1);
}


void FastCodeGenerator::ExitFinallyBlock() {
  ASSERT(!result_register().is(r1));
  // Restore result register from stack.
  __ pop(r1);
  // Uncook return address and return.
  __ pop(result_register());
  ASSERT_EQ(1, kSmiTagSize + kSmiShiftSize);
  __ mov(r1, Operand(r1, ASR, 1));  // Un-smi-tag value.
  __ add(pc, r1, Operand(masm_->CodeObject()));
}


#undef __

} }  // namespace v8::internal
