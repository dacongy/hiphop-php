/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2013 Facebook, Inc. (http://www.facebook.com)     |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/
#include "hphp/runtime/vm/jit/hhbctranslator.h"

#include "hphp/util/trace.h"
#include "hphp/runtime/ext/ext_closure.h"
#include "hphp/runtime/ext/ext_continuation.h"
#include "hphp/runtime/vm/jit/translator-runtime.h"
#include "hphp/runtime/vm/jit/translator-x64.h"
#include "hphp/runtime/base/stats.h"
#include "hphp/runtime/vm/unit.h"
#include "hphp/runtime/vm/runtime.h"
#include "hphp/runtime/vm/jit/irfactory.h"
#include "hphp/runtime/vm/jit/codegen.h" // ArrayIdx helpers

// Include last to localize effects to this file
#include "hphp/util/assert_throw.h"

namespace HPHP {
namespace JIT {

TRACE_SET_MOD(hhir);

using namespace HPHP::Transl;

HhbcTranslator::HhbcTranslator(IRFactory& irFactory,
                               Offset startOffset,
                               uint32_t initialSpOffsetFromFp,
                               const Func* func)
  : m_irFactory(irFactory)
  , m_tb(new TraceBuilder(startOffset,
                          initialSpOffsetFromFp,
                          m_irFactory,
                          func))
  , m_bcStateStack {BcState(startOffset, func)}
  , m_startBcOff(startOffset)
  , m_lastBcOff(false)
  , m_hasExit(false)
  , m_stackDeficit(0)
{
  emitMarker();
  auto const fp = gen(DefFP);
  gen(DefSP, StackOffset(initialSpOffsetFromFp), fp);
}

ArrayData* HhbcTranslator::lookupArrayId(int arrId) {
  return curUnit()->lookupArrayId(arrId);
}

StringData* HhbcTranslator::lookupStringId(int strId) {
  return curUnit()->lookupLitstrId(strId);
}

Func* HhbcTranslator::lookupFuncId(int funcId) {
  return curUnit()->lookupFuncId(funcId);
}

PreClass* HhbcTranslator::lookupPreClassId(int preClassId) {
  return curUnit()->lookupPreClassId(preClassId);
}

const NamedEntityPair& HhbcTranslator::lookupNamedEntityPairId(int id) {
  return curUnit()->lookupNamedEntityPairId(id);
}

const NamedEntity* HhbcTranslator::lookupNamedEntityId(int id) {
  return curUnit()->lookupNamedEntityId(id);
}

SSATmp* HhbcTranslator::push(SSATmp* tmp) {
  assert(tmp);
  m_evalStack.push(tmp);
  return tmp;
}

void HhbcTranslator::refineType(SSATmp* tmp, Type type) {
  // If type is more refined than tmp's type, reset tmp's type to type
  IRInstruction* inst = tmp->inst();
  if (type.strictSubtypeOf(tmp->type())) {
    // If tmp is incref or move, then chase down its src
    Opcode opc = inst->op();
    if (opc == Mov || opc == IncRef) {
      refineType(inst->src(0), type);
      tmp->setType(outputType(inst));
    } else if (tmp->type().isNull() && type.isNull()) {
      // Refining Null to Uninit or InitNull is supported
      tmp->setType(type);
    } else {
      // At this point, we have no business refining the type of any
      // instructions other than the following, which all control
      // their destination type via a type parameter.
      //
      // FIXME: I think most of these shouldn't be possible still
      // (except LdStack?).
      assert(opc == LdLoc || opc == LdStack ||
             opc == LdMem || opc == LdProp  ||
             opc == LdRef);
      inst->setTypeParam(type);
      tmp->setType(type);
      assert(outputType(inst) == type);
    }
  }
}

SSATmp* HhbcTranslator::pop(Type type) {
  SSATmp* opnd = m_evalStack.pop();
  if (opnd == nullptr) {
    uint32_t stackOff = m_stackDeficit;
    m_stackDeficit++;
    return gen(LdStack, type, StackOffset(stackOff), m_tb->sp());
  }

  // Refine the type of the temp given the information we have from
  // `type'.  This case can occur if we did an extendStack() and
  // didn't know the type of the intermediate values yet (see below).
  refineType(opnd, type);
  return opnd;
}

void HhbcTranslator::discard(unsigned n) {
  for (unsigned i = 0; i < n; ++i) {
    pop(Type::Gen | Type::Cls);
  }
}

// type is the type expected on the stack.
void HhbcTranslator::popDecRef(Type type) {
  if (SSATmp* src = m_evalStack.pop()) {
    gen(DecRef, src);
    return;
  }

  gen(DecRefStack, StackOffset(m_stackDeficit), type, m_tb->sp());
  m_stackDeficit++;
}

// We don't know what type description to expect for the stack
// locations before index, so we use a generic type when popping the
// intermediate values.  If it ends up creating a new LdStack,
// refineType during a later pop() or top() will fix up the type to
// the known type.
void HhbcTranslator::extendStack(uint32_t index, Type type) {
  if (index == 0) {
    push(pop(type));
    return;
  }

  SSATmp* tmp = pop(Type::Gen | Type::Cls);
  extendStack(index - 1, type);
  push(tmp);
}

SSATmp* HhbcTranslator::top(Type type, uint32_t index) {
  SSATmp* tmp = m_evalStack.top(index);
  if (!tmp) {
    extendStack(index, type);
    tmp = m_evalStack.top(index);
  }
  assert(tmp);
  refineType(tmp, type);
  return tmp;
}

void HhbcTranslator::replace(uint32_t index, SSATmp* tmp) {
  m_evalStack.replace(index, tmp);
}

/*
 * When doing gen-time inlining, we set up a series of IR instructions
 * that looks like this:
 *
 *   fp0  = DefFP
 *   sp0  = DefSP<offset>
 *
 *   // ... normal stuff happens ...
 *   // sp_pre = some SpillStack, or maybe the DefSP
 *
 *   // FPI region:
 *     sp1   = SpillStack sp_pre, ...
 *     sp2   = SpillFrame sp1, ...
 *     // ... possibly more spillstacks due to argument expressions
 *     sp3   = SpillStack sp2, -argCount
 *     fp2   = DefInlineFP<func,retBC,retSP> sp2 sp1
 *     sp4   = ReDefSP<numLocals> fp2
 *
 *         // ... callee body ...
 *
 *           = InlineReturn fp2
 *
 *   sp5  = ReDefSP<returnOffset> fp0 sp1
 *
 * The rest of the code then depends on sp5, and not any of the StkPtr
 * tree going through the callee body.  The sp5 tmp has the same view
 * of the stack as sp1 did, which represents what the stack looks like
 * before the return address is pushed but after the activation record
 * is popped.
 *
 * In DCE we attempt to remove the SpillFrame/InlineReturn/DefInlineFP
 * instructions if they aren't needed.
 */
void HhbcTranslator::beginInlining(unsigned numParams,
                                   const Func* target,
                                   Offset returnBcOffset) {
  assert(!m_fpiStack.empty() &&
    "Inlining does not support calls with the FPush* in a different Tracelet");
  assert(!target->isGenerator() && "Generator stack handling not implemented");

  FTRACE(1, "[[[ begin inlining: {}\n", target->fullName()->data());

  SSATmp* params[numParams];
  for (unsigned i = 0; i < numParams; ++i) {
    params[numParams - i - 1] = popF();
  }

  auto const prevSP    = m_fpiStack.top().first;
  auto const prevSPOff = m_fpiStack.top().second;
  auto const calleeSP  = spillStack();

  DefInlineFPData data;
  data.target   = target;
  data.retBCOff = returnBcOffset;
  data.retSPOff = prevSPOff;
  auto const calleeFP = gen(DefInlineFP, data, calleeSP, prevSP);

  m_bcStateStack.emplace_back(target->base(), target);
  gen(ReDefSP, StackOffset(target->numLocals()), m_tb->fp(), m_tb->sp());

  profileFunctionEntry("Inline");

  for (unsigned i = 0; i < numParams; ++i) {
    gen(StLoc, LocalId(i), calleeFP, params[i]);
  }
  for (unsigned i = numParams; i < target->numLocals(); ++i) {
    /*
     * Here we need to be generating hopefully-dead stores to
     * initialize non-parameter locals to KindOfUnknownin case we have
     * to leave the trace.
     */
    always_assert(0 && "unimplemented");
    gen(StLoc, LocalId(i), calleeFP, m_tb->genDefUninit());
  }

  emitMarker();
}

bool HhbcTranslator::isInlining() const {
  return m_bcStateStack.size() > 1;
}

IRInstruction* HhbcTranslator::makeMarker(Offset bcOff) {
  int32_t stackOff = m_tb->spOffset() +
    m_evalStack.numCells() - m_stackDeficit;

  FTRACE(2, "makeMarker: bc {} sp {} fn {}\n",
         bcOff, stackOff, curFunc()->fullName()->data());

  MarkerData marker;
  marker.bcOff     = bcOff;
  marker.func      = curFunc();
  marker.stackOff  = stackOff;
  return m_irFactory.gen(Marker, marker);
}

void HhbcTranslator::emitMarker() {
  m_tb->add(makeMarker(bcOff()));
}

void HhbcTranslator::profileFunctionEntry(const char* category) {
  static const bool enabled = Stats::enabledAny() &&
                              getenv("HHVM_STATS_FUNCENTRY");
  if (!enabled) return;

  gen(
    IncStatGrouped,
    cns(StringData::GetStaticString("FunctionEntry")),
    cns(StringData::GetStaticString(category)),
    cns(1)
  );
}

void HhbcTranslator::profileInlineFunctionShape(const std::string& str) {
  gen(
    IncStatGrouped,
    cns(StringData::GetStaticString("InlineShape")),
    cns(StringData::GetStaticString(str)),
    cns(1)
  );
}

void HhbcTranslator::profileSmallFunctionShape(const std::string& str) {
  gen(
    IncStatGrouped,
    cns(StringData::GetStaticString("SmallFunctions")),
    cns(StringData::GetStaticString(str)),
    cns(1)
  );
}

void HhbcTranslator::profileFailedInlShape(const std::string& str) {
  gen(
    IncStatGrouped,
    cns(StringData::GetStaticString("FailedInl")),
    cns(StringData::GetStaticString(str)),
    cns(1)
  );
}

void HhbcTranslator::setBcOff(Offset newOff, bool lastBcOff) {
  if (isInlining()) assert(!lastBcOff);

  if (newOff != bcOff()) {
    m_bcStateStack.back().bcOff = newOff;
    emitMarker();
  }
  m_lastBcOff = lastBcOff;
}

void HhbcTranslator::emitPrint() {
  Type type = topC()->type();
  if (type.subtypeOfAny(Type::Int, Type::Bool, Type::Null, Type::Str)) {
    auto const cell = popC();

    Opcode op;
    if (type.isString()) {
      op = PrintStr;
    } else if (type.subtypeOf(Type::Int)) {
      op = PrintInt;
    } else if (type.subtypeOf(Type::Bool)) {
      op = PrintBool;
    } else {
      assert(type.isNull());
      op = Nop;
    }
    // the print helpers decref their arg, so don't decref pop'ed value
    if (op != Nop) {
      gen(op, cell);
    }
    push(cns(1));
  } else {
    emitInterpOne(Type::Int, 1);
  }
}

void HhbcTranslator::emitUnboxRAux() {
  Block* exit = getExitTrace()->front();
  SSATmp* srcBox = popR();
  SSATmp* unboxed = gen(Unbox, exit, srcBox);
  if (unboxed == srcBox) {
    // If the Unbox ended up being a noop, don't bother refcounting
    push(unboxed);
  } else {
    pushIncRef(unboxed);
    gen(DecRef, srcBox);
  }
}

void HhbcTranslator::emitUnboxR() {
  emitUnboxRAux();
}

void HhbcTranslator::emitThis() {
  if (!curClass()) {
    emitInterpOne(Type::Obj, 0); // will throw a fatal
    return;
  }
  pushIncRef(gen(LdThis, getExitSlowTrace(), m_tb->fp()));
}

void HhbcTranslator::emitCheckThis() {
  if (!curClass()) {
    emitInterpOne(Type::None, 0); // will throw a fatal
    return;
  }
  gen(LdThis, getExitSlowTrace(), m_tb->fp());
}

void HhbcTranslator::emitBareThis(int notice) {
  // We just exit the trace in the case $this is null. Before exiting
  // the trace, we could also push null onto the stack and raise a
  // notice if the notice argument is set. By exiting the trace when
  // $this is null, we can be sure in the rest of the trace that we
  // have the this object on top of the stack, and we can eliminate
  // further null checks of this.
  if (!curClass()) {
    emitInterpOne(Type::InitNull, 0); // will raise notice and push null
    return;
  }
  pushIncRef(gen(LdThis, getExitSlowTrace(), m_tb->fp()));
}

void HhbcTranslator::emitArray(int arrayId) {
  push(cns(lookupArrayId(arrayId)));
}

void HhbcTranslator::emitNewArray(int capacity) {
  if (capacity == 0) {
    push(cns(HphpArray::GetStaticEmptyArray()));
  } else {
    push(gen(NewArray, cns(capacity)));
  }
}

void HhbcTranslator::emitNewTuple(int numArgs) {
  // The new_tuple helper function needs array values passed to it
  // via the stack.  We use spillStack() to flush the eval stack and
  // obtain a pointer to the topmost item; if over-flushing becomes
  // a problem then we should refactor the NewTuple opcode to take
  // its values directly as SSA operands.
  SSATmp* sp = spillStack();
  for (int i = 0; i < numArgs; i++) popC();
  push(gen(NewTuple, cns(numArgs), sp));
}

void HhbcTranslator::emitArrayAdd() {
  Type type1 = topC(0)->type();
  Type type2 = topC(1)->type();
  if (!type1.isArray() || !type2.isArray()) {
    // This happens when we have a prior spillstack that optimizes away
    // its spilled values because they were already on the stack. This
    // prevents us from getting to type of the SSATmps popped from the
    // eval stack. Most likely we had an interpone before this
    // instruction.
    emitInterpOne(Type::Arr, 2);
    return;
  }
  SSATmp* tr = popC();
  SSATmp* tl = popC();
  // The ArrayAdd helper decrefs its args, so don't decref pop'ed values.
  push(gen(ArrayAdd, tl, tr));
}

void HhbcTranslator::emitAddElemC() {
  SSATmp* val = popC();
  SSATmp* key = popC();
  SSATmp* arr = popC();
  // the AddElem* instructions decrefs their args, so don't decref
  // pop'ed values. TODO task 1805916: verify that AddElem* increfs
  // their result
  auto kt = key->type();
  Opcode op;
  if (kt.subtypeOf(Type::Int)) {
    op = AddElemIntKey;
  } else if (kt.isString()) {
    op = AddElemStrKey;
  } else {
    PUNT(AddElem-NonIntNonStr);
  }

  push(gen(op, arr, key, val));
}

void HhbcTranslator::emitAddNewElemC() {
  if (!topC(1)->isA(Type::Arr)) {
    return emitInterpOne(Type::Arr, 2, 0);
  }

  auto const val = popC();
  auto const arr = popC();
  // The AddNewElem helper decrefs its args, so don't decref pop'ed values.
  push(gen(AddNewElem, arr, val));
}

void HhbcTranslator::emitNewCol(int type, int numElems) {
  emitInterpOne(Type::Obj, 0);
}

void HhbcTranslator::emitColAddElemC() {
  emitInterpOne(Type::Obj, 3);
}

void HhbcTranslator::emitColAddNewElemC() {
  emitInterpOne(Type::Obj, 2);
}

void HhbcTranslator::emitCns(uint32_t id) {
  StringData* name = curUnit()->lookupLitstrId(id);
  SSATmp* cnsNameTmp = cns(name);
  const TypedValue* tv = Unit::lookupPersistentCns(name);
  SSATmp* result = nullptr;
  Type cnsType = Type::Cell;
  if (tv) {
    switch (tv->m_type) {
      case KindOfUninit:
        // a dynamic system constant. always a slow lookup
        result = gen(LookupCns, cnsType, cnsNameTmp);
        break;
      case KindOfBoolean:
        result = cns((bool)tv->m_data.num);
        break;
      case KindOfInt64:
        result = cns(tv->m_data.num);
        break;
      case KindOfDouble:
        result = cns(tv->m_data.dbl);
        break;
      case KindOfString:
      case KindOfStaticString:
        result = cns(tv->m_data.pstr);
        break;
      default:
        not_reached();
    }
  } else {
    SSATmp* c1 = gen(LdCns, cnsType, cnsNameTmp);
    result = m_tb->cond(
      curFunc(),
      [&] (Block* taken) { // branch
        gen(CheckInit, taken, c1);
      },
      [&] { // Next: LdCns hit in TC
        return c1;
      },
      [&] { // Taken: miss in TC, do lookup & init
        m_tb->hint(Block::Unlikely);
        return gen(LookupCns, getCatchTrace(), cnsType, cnsNameTmp);
      }
    );
  }
  push(result);
}

void HhbcTranslator::emitCnsE(uint32_t id) {
  PUNT(CnsE);
}

void HhbcTranslator::emitCnsU(uint32_t id) {
  PUNT(CnsU);
}

void HhbcTranslator::emitDefCns(uint32_t id) {
  StringData* name = lookupStringId(id);
  SSATmp* val = popC();
  push(gen(DefCns, cns(name), val));
}

void HhbcTranslator::emitConcat() {
  SSATmp* tr = popC();
  SSATmp* tl = popC();
  // the concat helpers decref their args, so don't decref pop'ed values
  push(gen(Concat, tl, tr));
}

void HhbcTranslator::emitDefCls(int cid, Offset after) {
  emitInterpOne(Type::None, 0);
}

void HhbcTranslator::emitDefFunc(int fid) {
  emitInterpOne(Type::None, 0);
}

void HhbcTranslator::emitLateBoundCls() {
  Class* clss = curClass();
  if (!clss) {
    // no static context class, so this will raise an error
    emitInterpOne(Type::Cls, 0);
    return;
  }
  auto const ctx = gen(LdCtx, m_tb->fp(), cns(curFunc()));
  push(gen(LdClsCtx, ctx));
}

void HhbcTranslator::emitSelf() {
  Class* clss = curClass();
  if (clss == nullptr) {
    emitInterpOne(Type::Cls, 0);
  } else {
    push(cns(clss));
  }
}

void HhbcTranslator::emitParent() {
  auto const clss = curClass();
  if (clss == nullptr || clss->parent() == nullptr) {
    emitInterpOne(Type::Cls, 0);
  } else {
    push(cns(clss->parent()));
  }
}

void HhbcTranslator::emitString(int strId) {
  push(cns(lookupStringId(strId)));
}

void HhbcTranslator::emitInt(int64_t val) {
  push(cns(val));
}

void HhbcTranslator::emitDouble(double val) {
  push(cns(val));
}

void HhbcTranslator::emitNullUninit() {
  push(m_tb->genDefUninit());
}

void HhbcTranslator::emitNull() {
  push(m_tb->genDefInitNull());
}

void HhbcTranslator::emitTrue() {
  push(cns(true));
}

void HhbcTranslator::emitFalse() {
  push(cns(false));
}

void HhbcTranslator::emitInitThisLoc(int32_t id) {
  if (!curClass()) {
    // Do nothing if this is null
    return;
  }
  SSATmp* tmpThis = gen(LdThis, getExitSlowTrace(), m_tb->fp());
  gen(StLoc, LocalId(id), m_tb->fp(), gen(IncRef, tmpThis));
}

void HhbcTranslator::emitCGetL(int32_t id) {
  IRTrace* exitTrace = getExitTrace();
  pushIncRef(ldLocInnerWarn(id, exitTrace));
}

void HhbcTranslator::emitCGetL2(int32_t id) {
  IRTrace* exitTrace = getExitTrace();
  IRTrace* catchTrace = getCatchTrace();
  SSATmp* oldTop = pop(Type::Gen);
  pushIncRef(ldLocInnerWarn(id, exitTrace, catchTrace));
  push(oldTop);
}

void HhbcTranslator::emitVGetL(int32_t id) {
  auto value = ldLoc(id);
  if (!value->type().isBoxed()) {
    if (value->isA(Type::Uninit)) {
      value = m_tb->genDefInitNull();
    }
    value = gen(Box, value);
    gen(StLoc, LocalId(id), m_tb->fp(), value);
  }
  pushIncRef(value);
}

void HhbcTranslator::emitUnsetN() {
  // No reason to punt, translator-x64 does emitInterpOne as well
  emitInterpOne(Type::None, 1);
}

void HhbcTranslator::emitUnsetG(const StringData* gblName) {
  // No reason to punt, translator-x64 does emitInterpOne as well
  emitInterpOne(Type::None, 1);
}

void HhbcTranslator::emitUnsetL(int32_t id) {
  auto const prev = ldLoc(id);
  gen(StLoc, LocalId(id), m_tb->fp(), m_tb->genDefUninit());
  gen(DecRef, prev);
}

void HhbcTranslator::emitBindL(int32_t id) {
  auto const newValue = popV();
  // Note that the IncRef must happen first, for correctness in a
  // pseudo-main: the destructor could decref the value again after
  // we've stored it into the local.
  pushIncRef(newValue);
  auto const oldValue = ldLoc(id);
  gen(StLoc, LocalId(id), m_tb->fp(), newValue);
  gen(DecRef, oldValue);
}

void HhbcTranslator::emitSetL(int32_t id) {
  auto const exitTrace = getExitTrace();
  auto const src = popC();
  push(stLoc(id, exitTrace, src));
}

void HhbcTranslator::emitIncDecL(bool pre, bool inc, uint32_t id) {
  IRTrace* exitTrace = getExitTrace();
  auto const src = ldLocInner(id, exitTrace);

  // Inc/Dec of a bool is a no-op.
  if (src->isA(Type::Bool)) {
    push(src);
    return;
  }

  auto const res = emitIncDec(pre, inc, src);
  stLoc(id, exitTrace, res);
}

// only handles integer or double inc/dec
SSATmp* HhbcTranslator::emitIncDec(bool pre, bool inc, SSATmp* src) {
  assert(src->isA(Type::Int) || src->isA(Type::Dbl));
  SSATmp* one = src->isA(Type::Int) ? cns(1) : cns(1.0);
  SSATmp* res = inc ? gen(OpAdd, src, one) : gen(OpSub, src, one);
  // no incref necessary on push since result is an int
  push(pre ? res : src);
  return res;
}

void HhbcTranslator::emitIncDecMem(bool pre,
                                   bool inc,
                                   SSATmp* propAddr,
                                   IRTrace* exitTrace) {
  // Handle only integer inc/dec for now
  SSATmp* src = gen(LdMem, Type::Int, exitTrace, propAddr, cns(0));
  // do the add and store back
  SSATmp* res = emitIncDec(pre, inc, src);
  // don't gen a dec ref or type store
  gen(StMemNT, propAddr, cns(0), res);
}

static bool areBinaryArithTypesSupported(Opcode opc, Type t1, Type t2) {
  switch (opc) {
  case OpAdd:
  case OpSub:
  case OpMul: return t1.subtypeOfAny(Type::Int, Type::Bool, Type::Dbl) &&
                     t2.subtypeOfAny(Type::Int, Type::Bool, Type::Dbl);

  case OpBitAnd:
  case OpBitOr:
  case OpBitXor:
    return t1.subtypeOfAny(Type::Int, Type::Bool) &&
                     t2.subtypeOfAny(Type::Int, Type::Bool);
  default:
    not_reached();
  }
}

void HhbcTranslator::emitSetOpL(Opcode subOpc, uint32_t id) {
  auto const exitTrace = getExitTrace();
  auto const loc       = ldLocInnerWarn(id, exitTrace);

  if (subOpc == Concat) {
    /*
     * The concat helpers decref their args, so don't decref pop'ed values
     * and don't decref the old value held in the local. The concat helpers
     * also incref their results, which will be consumed by the stloc. We
     * need an extra incref for the push onto the stack.
     */
    auto const val    = popC();
    auto const result = gen(Concat, loc, val);
    pushIncRef(stLocNRC(id, exitTrace, result));
    return;
  }

  if (areBinaryArithTypesSupported(subOpc, loc->type(), topC()->type())) {
    auto const val    = popC();
    auto const result = gen(
      subOpc,
      loc->isA(Type::Bool) ? gen(ConvBoolToInt, loc) : loc,
      val->isA(Type::Bool) ? gen(ConvBoolToInt, val) : val
    );
    push(stLoc(id, exitTrace, result));
    return;
  }

  PUNT(SetOpL);
}

void HhbcTranslator::emitClassExists(const StringData* clsName) {
  emitInterpOne(Type::Bool, 2);
}

void HhbcTranslator::emitInterfaceExists(const StringData* ifaceName) {
  emitClassExists(ifaceName);
}

void HhbcTranslator::emitTraitExists(const StringData* traitName) {
  emitClassExists(traitName);
}

void HhbcTranslator::emitStaticLocInit(uint32_t locId, uint32_t litStrId) {
  const StringData* name = lookupStringId(litStrId);
  SSATmp* value = popC();
  SSATmp* box;

  // Closures and generators from closures don't satisfy the "one static per
  // source location" rule that the inline fastpath requires
  if (curFunc()->isClosureBody() || curFunc()->isGeneratorFromClosure()) {
    box = gen(StaticLocInit, cns(name), m_tb->fp(), value);
  } else {
    SSATmp* ch = cns(TargetCache::allocStatic(), Type::CacheHandle);
    SSATmp* cachedBox = nullptr;
    box = m_tb->cond(curFunc(),
      [&](Block* taken) {
        // Careful: cachedBox is only ok to use in the 'next' branch.
        cachedBox = gen(LdStaticLocCached, taken, ch);
      },
      [&] { // next: The local is already initialized
        return gen(IncRef, cachedBox);
      },
      [&] { // taken: We missed in the cache
        m_tb->hint(Block::Unlikely);
        return gen(StaticLocInitCached,
                         cns(name), m_tb->fp(), value, ch);
      }
    );
  }
  gen(StLoc, LocalId(locId), m_tb->fp(), box);
  gen(DecRef, value);
}

void HhbcTranslator::emitReqDoc(const StringData* name) {
  PUNT(ReqDoc);
}

template<class Lambda>
SSATmp* HhbcTranslator::emitIterInitCommon(int offset, Lambda genFunc) {
  SSATmp* src = popC();
  Type type = src->type();
  if (!type.isArray() && type != Type::Obj) {
    PUNT(IterInit);
  }
  SSATmp* res = genFunc(src);
  return emitJmpCondHelper(offset, true, res);
}

void HhbcTranslator::emitIterInit(uint32_t iterId,
                                  int offset,
                                  uint32_t valLocalId) {
  emitIterInitCommon(offset, [=] (SSATmp* src) {
    return gen(
      IterInit,
      Type::Bool,
      src,
      m_tb->fp(),
      cns(iterId),
      cns(valLocalId)
    );
  });
}

void HhbcTranslator::emitIterInitK(uint32_t iterId,
                                   int offset,
                                   uint32_t valLocalId,
                                   uint32_t keyLocalId) {
  emitIterInitCommon(offset, [=] (SSATmp* src) {
    return gen(
      IterInitK,
      Type::Bool,
      src,
      m_tb->fp(),
      cns(iterId),
      cns(valLocalId),
      cns(keyLocalId)
    );
  });
}

void HhbcTranslator::emitIterNext(uint32_t iterId,
                                  int offset,
                                  uint32_t valLocalId) {
  SSATmp* res = gen(
    IterNext,
    Type::Bool,
    m_tb->fp(),
    cns(iterId),
    cns(valLocalId)
  );
  emitJmpCondHelper(offset, false, res);
}

void HhbcTranslator::emitIterNextK(uint32_t iterId,
                                   int offset,
                                   uint32_t valLocalId,
                                   uint32_t keyLocalId) {
  SSATmp* res = gen(
    IterNextK,
    Type::Bool,
    m_tb->fp(),
    cns(iterId),
    cns(valLocalId),
    cns(keyLocalId)
  );
  emitJmpCondHelper(offset, false, res);
}

void HhbcTranslator::emitWIterInit(uint32_t iterId,
                                   int offset,
                                   uint32_t valLocalId) {
  emitIterInitCommon(
    offset, [=] (SSATmp* src) {
      return gen(
        WIterInit,
        Type::Bool,
        src,
        m_tb->fp(),
        cns(iterId),
        cns(valLocalId)
      );
    }
  );
}

void HhbcTranslator::emitWIterInitK(uint32_t iterId,
                                    int offset,
                                    uint32_t valLocalId,
                                    uint32_t keyLocalId) {
  emitIterInitCommon(
    offset, [=] (SSATmp* src) {
      return gen(
        WIterInitK,
        Type::Bool,
        src,
        m_tb->fp(),
        cns(iterId),
        cns(valLocalId),
        cns(keyLocalId)
      );
    }
  );
}

void HhbcTranslator::emitWIterNext(uint32_t iterId,
                                   int offset,
                                   uint32_t valLocalId) {
  SSATmp* res = gen(
    WIterNext,
    Type::Bool,
    m_tb->fp(),
    cns(iterId),
    cns(valLocalId)
  );
  emitJmpCondHelper(offset, false, res);
}

void HhbcTranslator::emitWIterNextK(uint32_t iterId,
                                    int offset,
                                    uint32_t valLocalId,
                                    uint32_t keyLocalId) {
  SSATmp* res = gen(
    WIterNextK,
    Type::Bool,
    m_tb->fp(),
    cns(iterId),
    cns(valLocalId),
    cns(keyLocalId)
  );
  emitJmpCondHelper(offset, false, res);
}

void HhbcTranslator::emitIterFree(uint32_t iterId) {
  gen(IterFree, IterId(iterId), m_tb->fp());
}

void HhbcTranslator::emitDecodeCufIter(uint32_t iterId, int offset) {
  SSATmp* src = popC();
  Type type = src->type();
  if (type.subtypeOfAny(Type::Arr, Type::Str, Type::Obj)) {
    SSATmp* res = gen(DecodeCufIter, Type::Bool,
                      IterId(iterId), src, m_tb->fp());
    gen(DecRef, src);
    emitJmpCondHelper(offset, true, res);
  } else {
    gen(DecRef, src);
    emitJmp(offset, true, false);
  }
}

void HhbcTranslator::emitCIterFree(uint32_t iterId) {
  gen(CIterFree, IterId(iterId), m_tb->fp());
}

typedef std::map<int, int> ContParamMap;
/*
 * mapContParams determines if every named local in origFunc has a
 * corresponding named local in genFunc. If this step succeeds and
 * there's no VarEnv at runtime, the continuation's variables can be
 * filled completely inline in the TC (assuming there aren't too
 * many).
 */
static
bool mapContParams(ContParamMap& map,
                   const Func* origFunc, const Func* genFunc) {
  const StringData* const* varNames = origFunc->localNames();
  for (Id i = 0; i < origFunc->numNamedLocals(); ++i) {
    Id id = genFunc->lookupVarId(varNames[i]);
    if (id != kInvalidId) {
      map[i] = id;
    } else {
      return false;
    }
  }
  return true;
}

void HhbcTranslator::emitCreateCont(Id funNameStrId) {
  gen(ExitOnVarEnv, getExitSlowTrace()->front(), m_tb->fp());

  auto const genName = lookupStringId(funNameStrId);
  auto const origFunc = curFunc();
  auto const genFunc = origFunc->getGeneratorBody(genName);
  auto const origLocals = origFunc->numLocals();

  auto const cont = origFunc->isMethod()
    ? gen(
        CreateContMeth,
        cns(origFunc),
        cns(genFunc),
        gen(LdCtx, m_tb->fp(), cns(curFunc()))
      )
    : gen(
        CreateContFunc,
        cns(origFunc),
        cns(genFunc)
      );

  ContParamMap params;
  if (origLocals <= Translator::kMaxInlineContLocals &&
      mapContParams(params, origFunc, genFunc)) {
    static auto const thisStr = StringData::GetStaticString("this");
    Id thisId = kInvalidId;
    const bool fillThis = origFunc->isMethod() &&
      !origFunc->isStatic() &&
      ((thisId = genFunc->lookupVarId(thisStr)) != kInvalidId) &&
      (origFunc->lookupVarId(thisStr) == kInvalidId);

    SSATmp* contAR = gen(
      LdRaw, Type::PtrToGen, cont, cns(RawMemSlot::ContARPtr));
    for (int i = 0; i < origLocals; ++i) {
      // We must generate an AssertLoc because we don't have tracelet
      // guards on the object type in these outer generator functions.
      gen(AssertLoc, Type::Gen, LocalId(i), m_tb->fp());
      auto const loc = gen(IncRef, ldLoc(i));
      gen(StMem, contAR, cns(-cellsToBytes(params[i] + 1)), loc);
    }
    if (fillThis) {
      assert(thisId != kInvalidId);
      auto const thisObj = gen(IncRef, gen(LdThis, m_tb->fp()));
      gen(StMem, contAR, cns(-cellsToBytes(thisId + 1)), thisObj);
    }
  } else {
    gen(FillContLocals, m_tb->fp(), cns(origFunc),
      cns(genFunc), cont);
  }

  push(cont);
}

void HhbcTranslator::emitContEnter(int32_t returnBcOffset) {
  // The stack should always be clean here; this only appears in generated
  // methods we control.
  assert(m_evalStack.size() == 0);
  assert(m_stackDeficit == 0);

  assert(curClass());
  SSATmp* cont = gen(LdThis, m_tb->fp());
  SSATmp* contAR = gen(
    LdRaw, Type::FramePtr, cont, cns(RawMemSlot::ContARPtr)
  );

  SSATmp* func = gen(LdARFuncPtr, contAR, cns(0));
  SSATmp* funcBody = gen(
    LdRaw, Type::TCA, func, cns(RawMemSlot::ContEntry)
  );

  gen(
    ContEnter,
    contAR,
    funcBody,
    cns(returnBcOffset),
    m_tb->fp()
  );
  assert(m_stackDeficit == 0);
}

void HhbcTranslator::emitContExitImpl() {
  auto const retAddr = gen(LdRetAddr, m_tb->fp());
  auto const fp = gen(FreeActRec, m_tb->fp());
  auto const sp = spillStack();
  gen(RetCtrl, sp, fp, retAddr);
  m_hasExit = true;
}

void HhbcTranslator::emitContExit() {
  gen(ExitWhenSurprised, getExitSlowTrace());
  emitContExitImpl();
}

void HhbcTranslator::emitUnpackCont() {
  gen(LinkContVarEnv, m_tb->fp());
  gen(AssertLoc, Type::Obj, LocalId(0), m_tb->fp());
  auto const cont = ldLoc(0);

  auto const valOffset = cns(CONTOFF(m_received));
  push(gen(LdProp, Type::Cell, cont, valOffset));
  gen(StProp, cont, valOffset, m_tb->genDefNull());

  push(gen(LdRaw, Type::Int, cont, cns(RawMemSlot::ContLabel)));
}

void HhbcTranslator::emitPackCont(int64_t labelId) {
  gen(UnlinkContVarEnv, m_tb->fp());
  gen(AssertLoc, Type::Obj, LocalId(0), m_tb->fp());
  auto const cont = ldLoc(0);
  auto const newVal = popC();
  auto const oldValue = gen(LdProp, Type::Cell, cont, cns(CONTOFF(m_value)));
  gen(StProp, cont, cns(CONTOFF(m_value)), newVal);
  gen(DecRef, oldValue);
  gen(
    StRaw, cont, cns(RawMemSlot::ContLabel), cns(labelId)
  );
}

void HhbcTranslator::emitContRetC() {
  gen(AssertLoc, Type::Obj, LocalId(0), m_tb->fp());
  auto const cont = ldLoc(0);
  gen(ExitWhenSurprised, getExitSlowTrace());
  gen(
    StRaw, cont, cns(RawMemSlot::ContDone), cns(true)
  );
  auto const newVal = popC();
  auto const oldVal = gen(LdProp, Type::Cell, cont, cns(CONTOFF(m_value)));
  gen(StProp, cont, cns(CONTOFF(m_value)), newVal);
  gen(DecRef, oldVal);

  // transfer control
  emitContExitImpl();
}

void HhbcTranslator::emitContNext() {
  assert(curClass());
  SSATmp* cont = gen(LdThis, m_tb->fp());
  gen(ContPreNext, getExitSlowTrace(), cont);
  if (RuntimeOption::EvalHHIRGenerateAsserts) {
    // We're guaranteed to have a Null in m_received at this point
    auto const oldVal = gen(LdProp, Type::Cell, cont, cns(CONTOFF(m_received)));
    gen(DbgAssertType, Type::InitNull, oldVal);
  }
}

void HhbcTranslator::emitContSendImpl(bool raise) {
  assert(curClass());
  SSATmp* cont = gen(LdThis, m_tb->fp());
  gen(ContStartedCheck, getExitSlowTrace(), cont);
  gen(ContPreNext, getExitSlowTrace(), cont);
  gen(AssertLoc, Type::Cell, LocalId(0), m_tb->fp());
  auto const newVal = gen(IncRef, ldLoc(0));
  if (RuntimeOption::EvalHHIRGenerateAsserts) {
    // We're guaranteed to have a Null in m_received at this point
    auto const oldVal = gen(LdProp, Type::Cell, cont, cns(CONTOFF(m_received)));
    gen(DbgAssertType, Type::InitNull, oldVal);
  }
  gen(StProp, cont, cns(CONTOFF(m_received)), newVal);
  if (raise) {
    SSATmp* label = gen(LdRaw, Type::Int, cont, cns(RawMemSlot::ContLabel));
    label = gen(OpSub, label, cns(1));
    gen(StRaw, cont, cns(RawMemSlot::ContLabel), label);
  }
}

void HhbcTranslator::emitContSend() {
  emitContSendImpl(false);
}

void HhbcTranslator::emitContRaise() {
  emitContSendImpl(true);
}

void HhbcTranslator::emitContValid() {
  assert(curClass());
  SSATmp* cont = gen(LdThis, m_tb->fp());
  SSATmp* done = gen(
    LdRaw, Type::Bool, cont, cns(RawMemSlot::ContDone)
  );
  push(gen(OpNot, done));
}

void HhbcTranslator::emitContCurrent() {
  assert(curClass());
  SSATmp* cont = gen(LdThis, m_tb->fp());
  gen(ContStartedCheck, getExitSlowTrace(), cont);
  SSATmp* offset = cns(CONTOFF(m_value));
  SSATmp* value = gen(LdProp, Type::Cell, cont, offset);
  value = gen(IncRef, value);
  push(value);
}

void HhbcTranslator::emitContStopped() {
  assert(curClass());
  SSATmp* cont = gen(LdThis, m_tb->fp());
  gen(
    StRaw, cont, cns(RawMemSlot::ContRunning), cns(false)
  );
}

void HhbcTranslator::emitContHandle() {
  emitInterpOneCF(1);
}

void HhbcTranslator::emitStrlen() {
  Type inType = topC()->type();

  if (inType.isString()) {
    SSATmp* input = popC();
    if (input->isConst()) {
      // static string; fold its strlen operation
      push(cns(input->getValStr()->size()));
    } else {
      push(gen(LdRaw, Type::Int, input, cns(RawMemSlot::StrLen)));
      gen(DecRef, input);
    }
  } else if (inType.isNull()) {
    popC();
    push(cns(0));
  } else if (inType == Type::Bool) {
    // strlen(true) == 1, strlen(false) == 0.
    push(gen(ConvBoolToInt, popC()));
  } else {
    emitInterpOne(Type::Int | Type::InitNull, 1);
  }
}

void HhbcTranslator::emitIncStat(int32_t counter, int32_t value, bool force) {
  if (Stats::enabled() || force) {
    gen(IncStat, cns(counter), cns(value), cns(force));
  }
}

void HhbcTranslator::emitArrayIdx() {
  Type arrType = topC(1)->type();
  Type keyType = topC(2)->type();

  if (UNLIKELY(!arrType.subtypeOf(Type::Arr))) {
    // raise fatal
    emitInterpOne(Type::Cell, 3);
    return;
  }

  if (UNLIKELY(keyType.subtypeOf(Type::Null))) {
    SSATmp* def = popC();
    SSATmp* arr = popC();
    SSATmp* key = popC();

    // if the key is null it will not be found so just return the default
    push(def);
    gen(DecRef, arr);
    gen(DecRef, key);
    return;
  }
  if (UNLIKELY(
        !(keyType.subtypeOf(Type::Int) || keyType.subtypeOf(Type::Str)))) {
    emitInterpOne(Type::Cell, 3);
    return;
  }

  SSATmp* def = popC();
  SSATmp* arr = popC();
  SSATmp* key = popC();

  KeyType arrayKeyType;
  bool checkForInt;
  checkStrictlyInteger(key, arrayKeyType, checkForInt);

  TCA opFunc;
  if (checkForInt) {
    opFunc = (TCA)&arrayIdxSi;
  } else if (IntKey == arrayKeyType) {
    opFunc = (TCA)&arrayIdxI;
  } else {
    assert(StrKey == arrayKeyType);
    opFunc = (TCA)&arrayIdxS;
  }

  push(gen(ArrayIdx, cns(opFunc), arr, key, def));
  gen(DecRef, arr);
  gen(DecRef, key);
}

void HhbcTranslator::emitIncTransCounter() {
  m_tb->gen(IncTransCounter);
}

SSATmp* HhbcTranslator::getStrName(const StringData* knownName) {
  SSATmp* name = popC();
  assert(name->isA(Type::Str) || knownName);
  if (!name->isConst() || !name->isA(Type::Str)) {
    if (knownName) {
      // The SSATmp on the evaluation stack was not a string constant,
      // but the bytecode translator somehow knew the name statically.
      name = cns(knownName);
    }
  } else {
    assert(!knownName || knownName->same(name->getValStr()));
  }
  return name;
}

SSATmp* HhbcTranslator::emitLdClsPropAddrCached(const StringData* propName,
                                                Block* block) {
  SSATmp* cls = popA();
  const StringData* clsName = findClassName(cls);
  assert(clsName);

  SSATmp* prop = getStrName(propName);
  SSATmp* addr = gen(LdClsPropAddrCached,
                     block,
                     cls,
                     prop,
                     cns(clsName),
                     cns(curClass()));
  return addr;
}

SSATmp* HhbcTranslator::emitLdClsPropAddrOrExit(const StringData* propName,
                                                Block* block) {
  if (canUseSPropCache(m_evalStack.top(), propName, curClass())) {
    return emitLdClsPropAddrCached(propName, block);
  }

  if (!block) block = getCatchTrace()->front();

  SSATmp* clsTmp = popA();
  SSATmp* prop = getStrName(propName);
  SSATmp* addr = gen(LdClsPropAddr,
                           block,
                           clsTmp,
                           prop,
                           cns(curClass()));
  gen(DecRef, prop); // safe to do early because prop is a string
  return addr;
}

bool HhbcTranslator::checkSupportedClsProp(const StringData* propName,
                                           Type resultType,
                                           int stkIndex) {
  if (topC(stkIndex + 1)->isA(Type::Str) || propName) {
    return true;
  }
  emitInterpOne(resultType, stkIndex + 2);
  return false;
}

bool HhbcTranslator::checkSupportedGblName(const StringData* gblName,
                                           Type resultType,
                                           int stkIndex) {
  if (topC(stkIndex)->isA(Type::Str) || gblName) {
    return true;
  }
  emitInterpOne(resultType, stkIndex + 1);
  return false;
}

SSATmp* HhbcTranslator::emitLdGblAddr(const StringData* gblName, Block* block) {
  SSATmp* name = getStrName(gblName);
  // Note: Once we use control flow to implement IssetG/EmptyG, we can
  // use a LdGblAddr helper that decrefs name for us
  SSATmp* addr = gen(LdGblAddr, block, name);
  gen(DecRef, name);
  return addr;
}

SSATmp* HhbcTranslator::emitLdGblAddrDef(const StringData* gblName) {
  return gen(LdGblAddrDef, getStrName(gblName));
}

void HhbcTranslator::emitIncDecS(bool pre, bool inc) {
  if (!checkSupportedClsProp(nullptr, Type::Cell, 0)) return;
  IRTrace* exit = getExitSlowTrace();
  emitIncDecMem(pre, inc, emitLdClsPropAddr(nullptr), exit);
}

void HhbcTranslator::emitMInstr(const NormalizedInstruction& ni) {
  VectorTranslator(ni, *this).emit();
}

/*
 * IssetH: return true if var is not uninit and !is_null(var)
 * Unboxes var if necessary when var is not uninit.
 */
void HhbcTranslator::emitIssetL(int32_t id) {
  auto const exitTrace = getExitTrace();
  auto const ld = ldLocInner(id, exitTrace);
  push(gen(IsNType, Type::Null, ld));
}

void HhbcTranslator::emitIssetG(const StringData* gblName) {
  emitIsset(gblName,
            &HhbcTranslator::checkSupportedGblName,
            &HhbcTranslator::emitLdGblAddr);
}

void HhbcTranslator::emitIssetS(const StringData* propName) {
  emitIsset(propName,
            &HhbcTranslator::checkSupportedClsProp,
            &HhbcTranslator::emitLdClsPropAddrOrExit);
}

void HhbcTranslator::emitEmptyL(int32_t id) {
  auto const exitTrace = getExitTrace();
  auto const ld = ldLocInner(id, exitTrace);
  push(gen(OpNot, gen(ConvCellToBool, ld)));
}

void HhbcTranslator::emitEmptyG(const StringData* gblName) {
  emitEmpty(gblName,
            &HhbcTranslator::checkSupportedGblName,
            &HhbcTranslator::emitLdGblAddr);
}

void HhbcTranslator::emitEmptyS(const StringData* propName) {
  emitEmpty(propName,
            &HhbcTranslator::checkSupportedClsProp,
            &HhbcTranslator::emitLdClsPropAddrOrExit);
}

void HhbcTranslator::emitIsTypeC(Type t) {
  SSATmp* src = popC();
  push(gen(IsType, t, src));
  gen(DecRef, src);
}

void HhbcTranslator::emitIsTypeL(Type t, int id) {
  IRTrace* exitTrace = getExitTrace();
  push(gen(IsType, t, ldLocInnerWarn(id, exitTrace)));
}

void HhbcTranslator::emitIsNullL(int id)   { emitIsTypeL(Type::Null, id);}
void HhbcTranslator::emitIsArrayL(int id)  { emitIsTypeL(Type::Arr, id); }
void HhbcTranslator::emitIsStringL(int id) { emitIsTypeL(Type::Str, id); }
void HhbcTranslator::emitIsObjectL(int id) { emitIsTypeL(Type::Obj, id); }
void HhbcTranslator::emitIsIntL(int id)    { emitIsTypeL(Type::Int, id); }
void HhbcTranslator::emitIsBoolL(int id)   { emitIsTypeL(Type::Bool, id);}
void HhbcTranslator::emitIsDoubleL(int id) { emitIsTypeL(Type::Dbl, id); }
void HhbcTranslator::emitIsNullC()   { emitIsTypeC(Type::Null);}
void HhbcTranslator::emitIsArrayC()  { emitIsTypeC(Type::Arr); }
void HhbcTranslator::emitIsStringC() { emitIsTypeC(Type::Str); }
void HhbcTranslator::emitIsObjectC() { emitIsTypeC(Type::Obj); }
void HhbcTranslator::emitIsIntC()    { emitIsTypeC(Type::Int); }
void HhbcTranslator::emitIsBoolC()   { emitIsTypeC(Type::Bool);}
void HhbcTranslator::emitIsDoubleC() { emitIsTypeC(Type::Dbl); }

void HhbcTranslator::emitPopC() {
  popDecRef(Type::Cell);
}

void HhbcTranslator::emitPopV() {
  popDecRef(Type::BoxedCell);
}

void HhbcTranslator::emitPopR() {
  popDecRef(Type::Gen);
}

void HhbcTranslator::emitDup() {
  pushIncRef(topC());
}

void HhbcTranslator::emitJmp(int32_t offset,
                             bool  breakTracelet,
                             bool  noSurprise) {
  // If surprise flags are set, exit trace and handle surprise
  bool backward = (offset - (int32_t)bcOff()) < 0;
  if (backward && !noSurprise) {
    gen(ExitWhenSurprised, getExitSlowTrace());
  }
  if (!breakTracelet) return;
  gen(Jmp_, getExitTrace(offset));
}

SSATmp* HhbcTranslator::emitJmpCondHelper(int32_t offset,
                                          bool negate,
                                          SSATmp* src) {
  // Spill everything on main trace if all paths will exit.
  if (m_lastBcOff) spillStack();

  auto const target  = getExitTrace(offset);
  auto const boolSrc = gen(ConvCellToBool, src);
  gen(DecRef, src);
  return gen(negate ? JmpZero : JmpNZero, target, boolSrc);
}

void HhbcTranslator::emitJmpZ(Offset taken) {
  auto const src = popC();
  emitJmpCondHelper(taken, true, src);
}

void HhbcTranslator::emitJmpNZ(Offset taken) {
  auto const src = popC();
  emitJmpCondHelper(taken, false, src);
}

void HhbcTranslator::emitCmp(Opcode opc) {
  IRTrace* catchTrace = nullptr;
  if (cmpOpTypesMayReenter(opc, topC(0)->type(), topC(1)->type())) {
    catchTrace = getCatchTrace();
  }
  // src2 opc src1
  SSATmp* src1 = popC();
  SSATmp* src2 = popC();
  push(gen(opc, catchTrace, src2, src1));
  gen(DecRef, src2);
  gen(DecRef, src1);
}

void HhbcTranslator::emitClsCnsD(int32_t cnsNameId, int32_t clsNameId) {
  auto const clsCnsName = ClsCnsName { lookupStringId(clsNameId),
                                       lookupStringId(cnsNameId) };

  // If we have to side exit, do the target cache lookup before
  // chaining to another Tracelet so forward progress still happens.
  auto const sideExit = makeSideExit(
    nextBcOff(),
    [&] (IRTrace* t) {
      return genFor(t, LookupClsCns, Type::Cell, clsCnsName);
    }
  );

  auto const cns = gen(LdClsCns, clsCnsName, Type::Uncounted);
  gen(CheckInit, sideExit, cns);
  push(cns);
}

void HhbcTranslator::emitAKExists() {
  SSATmp* arr = popC();
  SSATmp* key = popC();

  if (!arr->isA(Type::Arr) && !arr->isA(Type::Obj)) {
    PUNT(AKExists_badArray);
  }
  if (!key->isString() && !key->isA(Type::Int) && !key->isA(Type::Null)) {
    PUNT(AKExists_badKey);
  }

  push(gen(AKExists, arr, key));
  gen(DecRef, arr);
  gen(DecRef, key);
}

void HhbcTranslator::emitFPassR() {
  emitUnboxRAux();
}

void HhbcTranslator::emitFPassCOp() {
}

void HhbcTranslator::emitFPassV() {
  Block* exit = getExitTrace()->front();
  SSATmp* tmp = popV();
  pushIncRef(gen(Unbox, exit, tmp));
  gen(DecRef, tmp);
}

void HhbcTranslator::emitFPushCufIter(int32_t numParams,
                                      int32_t itId) {
  auto sp = spillStack();
  m_fpiStack.emplace(sp, m_tb->spOffset());
  gen(CufIterSpillFrame,
      FPushCufData(numParams, itId),
      sp, m_tb->fp());
}

void HhbcTranslator::emitFPushCufOp(Op op, Class* cls, StringData* invName,
                                    const Func* callee, int numArgs) {
  const Func* curFunc = this->curFunc();
  const bool safe = op == OpFPushCufSafe;
  const bool forward = op == OpFPushCufF;

  if (!callee) {
    SSATmp* callable = topC(safe ? 1 : 0);
    // The most common type for the callable in this case is Arr. We
    // can't really do better than the interpreter here, so punt.
    SPUNT(StringData::GetStaticString(
            folly::format("FPushCuf-{}",
                          callable->type().toString()).str())
          ->data());
  }

  SSATmp* ctx;
  SSATmp* safeFlag = cns(true); // This is always true until the slow exits
                                // below are implemented
  SSATmp* func = cns(callee);
  if (cls) {
    if (forward) {
      ctx = gen(LdCtx, m_tb->fp(), cns(curFunc));
      ctx = gen(GetCtxFwdCall, ctx, cns(callee));
    } else {
      ctx = genClsMethodCtx(callee, cls);
    }
    if (!TargetCache::isPersistentHandle(cls->m_cachedOffset)) {
      // The miss path is complicated and rare. Punt for now.
      gen(LdClsCachedSafe, getExitSlowTrace(), cns(cls->name()));
    }
  } else {
    ctx = m_tb->genDefInitNull();
    if (!TargetCache::isPersistentHandle(callee->getCachedOffset())) {
      // The miss path is complicated and rare. Punt for now.
      func = gen(LdFuncCachedSafe, getExitSlowTrace(),
                       cns(callee->name()));
    }
  }

  SSATmp* defaultVal = safe ? popC() : nullptr;
  popDecRef(Type::Cell); // callable
  if (safe) {
    push(defaultVal);
    push(safeFlag);
  }

  emitFPushActRec(func, ctx, numArgs, invName);
}

void HhbcTranslator::emitNativeImpl() {
  gen(NativeImpl, cns(curFunc()), m_tb->fp());
  SSATmp* sp = gen(RetAdjustStack, m_tb->fp());
  SSATmp* retAddr = gen(LdRetAddr, m_tb->fp());
  SSATmp* fp = gen(FreeActRec, m_tb->fp());
  gen(RetCtrl, sp, fp, retAddr);

  // Flag that this trace has a Ret instruction so no ExitTrace is needed
  m_hasExit = true;
}

void HhbcTranslator::emitFPushActRec(SSATmp* func,
                                     SSATmp* objOrClass,
                                     int32_t numArgs,
                                     const StringData* invName) {
  /*
   * Before allocating an ActRec, we do a spillStack so we'll have a
   * StkPtr that represents what the stack will look like after the
   * ActRec is popped.
   */
  auto actualStack = spillStack();
  auto returnSp = actualStack;

  /*
   * XXX. In a generator, we can't use ReDefSP to restore the stack
   * pointer from the frame pointer if we inline the callee.  (This is
   * because we don't really pay attention to usedefs for allocating
   * registers to stack pointers, and rVmFp and rVmSp are not related
   * to each other in a generator frame.)
   *
   * Instead, save it somewhere so we can move it back after.  This
   * instruction will be dce'd if we don't inline the callee.
   *
   * TODO(#2288359): freeing up the special-ness of %rbx should
   * allow us to avoid this sort of thing.
   */
  if (curFunc()->isGenerator()) {
    returnSp = gen(StashGeneratorSP, m_tb->sp());
  }

  m_fpiStack.emplace(returnSp, m_tb->spOffset());

  ActRecInfo info;
  info.numArgs = numArgs;
  info.invName = invName;
  gen(
    SpillFrame,
    info,
    // Using actualStack instead of returnSp so SpillFrame still gets
    // the src in rVmSp.  (TODO(#2288359).)
    actualStack,
    m_tb->fp(),
    func,
    objOrClass
  );
  assert(m_stackDeficit == 0);
}

void HhbcTranslator::emitFPushCtorCommon(SSATmp* cls,
                                         SSATmp* obj,
                                         const Func* func,
                                         int32_t numParams,
                                         IRTrace* catchTrace) {
  push(obj);
  SSATmp* fn = nullptr;
  if (func) {
    fn = cns(func);
  } else {
    fn = gen(LdClsCtor, catchTrace, cls);
  }
  SSATmp* obj2 = gen(IncRef, obj);
  int32_t numArgsAndCtorFlag = ActRec::encodeNumArgs(numParams, true);
  emitFPushActRec(fn, obj2, numArgsAndCtorFlag, nullptr);
}

void HhbcTranslator::emitFPushCtor(int32_t numParams) {
  IRTrace* catchTrace = getCatchTrace();
  SSATmp* cls = popA();
  SSATmp* obj = gen(IncRef, gen(AllocObj, cls));
  emitFPushCtorCommon(cls, obj, nullptr, numParams, catchTrace);
}

static bool canInstantiateClass(const Class* cls) {
  return cls &&
    !(cls->attrs() & (AttrAbstract | AttrInterface | AttrTrait));
}

void HhbcTranslator::emitFPushCtorD(int32_t numParams, int32_t classNameStrId) {
  const StringData* className = lookupStringId(classNameStrId);
  // The code generated for the catch trace depends on the environment at the
  // call so we can't share them between instructions.
  IRTrace* catchTrace1 = getCatchTrace();
  IRTrace* catchTrace2 = getCatchTrace();

  const Class* cls = Unit::lookupUniqueClass(className);
  bool uniqueCls = classIsUnique(cls);
  bool persistentCls = TargetCache::classIsPersistent(cls);
  bool canInstantiate = canInstantiateClass(cls);
  bool fastAlloc = !RuntimeOption::EnableObjDestructCall &&
    persistentCls && canInstantiate;

  const Func* func = uniqueCls ? cls->getCtor() : nullptr;
  if (func && !(func->attrs() & AttrPublic)) {
    Class* ctx = arGetContextClass(curFrame());
    if (!ctx) {
      func = nullptr;
    } else if (ctx != cls) {
      if ((func->attrs() & AttrPrivate) ||
        !(ctx->classof(cls) || cls->classof(ctx))) {
        func = nullptr;
      }
    }
  }

  SSATmp* clss = nullptr;
  if (persistentCls) {
    clss = cns(cls);
  } else {
    clss = gen(LdClsCached, catchTrace1, cns(className));
  }

  SSATmp* obj = nullptr;
  if (fastAlloc) {
    obj = gen(IncRef, gen(AllocObjFast, clss));
  } else {
    obj = gen(IncRef, gen(AllocObj, clss));
  }

  emitFPushCtorCommon(clss, obj, func, numParams, catchTrace2);
}

/*
 * The CreateCl opcode is specified as not being allowed before the
 * class it creates exists, and closure classes are always unique.
 *
 * This means even if we're not in RepoAuthoritative mode, as long as
 * this code is reachable it will always use the same closure Class*,
 * so we can just burn it into the TC without using TargetCache.
 */
void HhbcTranslator::emitCreateCl(int32_t numParams, int32_t funNameStrId) {
  auto const sp = spillStack();
  auto const cls = Unit::lookupUniqueClass(lookupStringId(funNameStrId));
  assert(cls && (cls->attrs() & AttrUnique));

  auto const closure = gen(
    CreateCl,
    cns(cls),
    cns(numParams),
    m_tb->fp(),
    sp
  );

  discard(numParams);
  push(closure);
}

void HhbcTranslator::emitFPushFuncD(int32_t numParams, int32_t funcId) {
  const NamedEntityPair& nep = lookupNamedEntityPairId(funcId);
  const StringData* name = nep.first;
  const Func* func       = Unit::lookupFunc(nep.second);
  if (!func) {
    // function lookup failed so just do the same as FPushFunc
    emitFPushFunc(numParams, cns(name));
    return;
  }
  func->validate();

  const bool immutable = func->isNameBindingImmutable(curUnit());

  IRTrace* catchTrace = nullptr;
  if (!immutable) {
    catchTrace = getCatchTrace();  // LdFuncCached can throw
  }
  SSATmp* ssaFunc = immutable ? cns(func)
                              : gen(LdFuncCached, catchTrace, cns(name));
  emitFPushActRec(ssaFunc,
                  m_tb->genDefInitNull(),
                  numParams,
                  nullptr);
}

void HhbcTranslator::emitFPushFuncU(int32_t numParams,
                                    int32_t funcId,
                                    int32_t fallbackFuncId) {
  PUNT(FPushFuncU);
}

void HhbcTranslator::emitFPushFunc(int32_t numParams) {
  // input must be a string or an object implementing __invoke();
  // otherwise fatal
  SSATmp* funcName = popC();
  if (!funcName->isString()) {
    PUNT(FPushFunc_not_Str);
  }
  emitFPushFunc(numParams, funcName);
}

void HhbcTranslator::emitFPushFunc(int32_t numParams, SSATmp* funcName) {
  emitFPushActRec(gen(LdFunc, getCatchTrace(), funcName),
                  m_tb->genDefInitNull(),
                  numParams,
                  nullptr);
}

void HhbcTranslator::emitFPushObjMethodD(int32_t numParams,
                                         int32_t methodNameStrId,
                                         const Class* baseClass) {
  const StringData* methodName = lookupStringId(methodNameStrId);
  bool magicCall = false;
  const Func* func = HPHP::Transl::lookupImmutableMethod(baseClass,
                                                             methodName,
                                                             magicCall,
                                                         /* staticLookup: */
                                                             false);
  SSATmp* obj = popC();
  SSATmp* objOrCls = obj;

  if (!func) {
    if (baseClass && !(baseClass->attrs() & AttrInterface)) {
      MethodLookup::LookupResult res =
        g_vmContext->lookupObjMethod(func, baseClass, methodName, false);
      if ((res == MethodLookup::MethodFoundWithThis ||
           res == MethodLookup::MethodFoundNoThis) &&
          !func->isAbstract()) {
        /*
         * If we found the func in baseClass, then either:
         *  a) its private, and this is always going to be the
         *     called function. This case is handled further down.
         * OR
         *  b) any derived class must have a func that matches in staticness
         *     and is at least as accessible (and in particular, you can't
         *     override a public/protected method with a private method).
         *     In this case, we emit code to dynamically lookup the method
         *     given the Object and the method slot, which is the same as func's.
         */
        if (!(func->attrs() & AttrPrivate)) {
          SSATmp* clsTmp = gen(LdObjClass, obj);
          SSATmp* funcTmp = gen(
            LdClsMethod, clsTmp, cns(func->methodSlot())
          );
          if (res == MethodLookup::MethodFoundNoThis) {
            gen(DecRef, obj);
            objOrCls = clsTmp;
          }
          emitFPushActRec(funcTmp, objOrCls, numParams,
                          magicCall ? methodName : nullptr);
          return;
        }
      } else {
        // method lookup did not find anything
        func = nullptr; // force lookup
      }
    }
  }

  if (func != nullptr) {
    if (func->attrs() & AttrStatic) {
      assert(baseClass);  // This assert may be too strong, but be aggressive
      // static function: store base class into this slot instead of obj
      // and decref the obj that was pushed as the this pointer since
      // the obj won't be in the actrec and thus MethodCache::lookup won't
      // decref it
      gen(DecRef, obj);
      objOrCls = cns(baseClass);
    }
    emitFPushActRec(cns(func),
                    objOrCls,
                    numParams,
                    magicCall ? methodName : nullptr);
  } else {
    emitFPushActRec(m_tb->genDefNull(),
                    obj,
                    numParams,
                    nullptr);
    auto const actRec = spillStack();
    auto const objCls = gen(LdObjClass, obj);

    // This is special. We need to move the stackpointer incase LdObjMethod
    // calls a destructor. Otherwise it would clobber the ActRec we just pushed.
    emitMarker();

    gen(LdObjMethod,
              objCls,
              cns(methodName),
              actRec);
  }
}

SSATmp* HhbcTranslator::genClsMethodCtx(const Func* callee, const Class* cls) {
  bool mightNotBeStatic = false;
  assert(callee);
  if (!(callee->attrs() & AttrStatic) &&
      !(curFunc()->attrs() & AttrStatic) &&
      curClass() &&
      curClass()->classof(cls)) {
    mightNotBeStatic = true;
  }

  if (!mightNotBeStatic) {
    // static function: ctx is just the Class*. LdCls will simplify to a
    // DefConst or LdClsCached.
    return gen(LdCls, cns(cls->name()), cns(curClass()));
  }
  if (m_tb->isThisAvailable()) {
    // might not be a static call and $this is available, so we know it's
    // definitely not static
    assert(curClass());
    return gen(IncRef, gen(LdThis, m_tb->fp()));
  }
  // might be a non-static call. we have to inspect the func at runtime
  PUNT(getClsMethodCtx-MightNotBeStatic);
}

void HhbcTranslator::emitFPushClsMethodD(int32_t numParams,
                                         int32_t methodNameStrId,
                                         int32_t clssNamedEntityPairId) {

  const StringData* methodName = lookupStringId(methodNameStrId);
  const NamedEntityPair& np = lookupNamedEntityPairId(clssNamedEntityPairId);
  const StringData* className = np.first;
  const Class* baseClass = Unit::lookupUniqueClass(np.second);
  bool magicCall = false;
  const Func* func = HPHP::Transl::lookupImmutableMethod(baseClass,
                                                             methodName,
                                                             magicCall,
                                                         /* staticLookup: */
                                                             true);
  if (func) {
    SSATmp* objOrCls = genClsMethodCtx(func, baseClass);
    emitFPushActRec(cns(func),
                    objOrCls,
                    numParams,
                    func && magicCall ? methodName : nullptr);
  } else {
    // lookup static method & class in the target cache
    SSATmp* stack = spillStack();
    IRTrace* exitTrace = getExitSlowTrace();
    SSATmp* funcClassTmp =
      gen(LdClsMethodCache,
                exitTrace,
                cns(className),
                cns(methodName),
                cns(np.second),
                m_tb->fp(),
                stack);
    emitFPushActRec(funcClassTmp,
                    m_tb->genDefInitNull(),
                    numParams,
                    nullptr);
  }
}

void HhbcTranslator::emitFPushClsMethodF(int32_t           numParams,
                                         const Class*      cls,
                                         const StringData* methName) {

  assert(cls);
  assert(methName && methName->isStatic());

  Block* exitBlock = getExitSlowTrace()->front();

  UNUSED SSATmp* clsVal  = popC();
  UNUSED SSATmp* methVal = popC();

  bool magicCall = false;
  const Func* func = lookupImmutableMethod(cls, methName, magicCall,
                                           true /* staticLookup */);
  SSATmp* curCtxTmp = gen(LdCtx, m_tb->fp(), cns(curFunc()));
  if (func) {
    SSATmp*   funcTmp = cns(func);
    SSATmp* newCtxTmp = gen(GetCtxFwdCall, curCtxTmp, funcTmp);

    emitFPushActRec(funcTmp, newCtxTmp, numParams,
                    (magicCall ? methName : nullptr));

  } else {
    SSATmp* funcCtxTmp = gen(LdClsMethodFCache, exitBlock,
                                   cns(cls),
                                   cns(methName),
                                   curCtxTmp,
                                   m_tb->fp());
    emitFPushActRec(funcCtxTmp,
                    m_tb->genDefInitNull(),
                    numParams,
                    (magicCall ? methName : nullptr));
  }
}

void HhbcTranslator::emitFCallArray(const Offset pcOffset,
                                    const Offset after) {
  SSATmp* stack = spillStack();
  gen(CallArray, CallArrayData(pcOffset, after), stack);
}

void HhbcTranslator::emitFCall(uint32_t numParams,
                               Offset returnBcOffset,
                               const Func* callee) {
  SSATmp* params[numParams + 3];
  std::memset(params, 0, sizeof params);
  for (uint32_t i = 0; i < numParams; i++) {
    params[numParams + 3 - i - 1] = popF();
  }
  params[0] = spillStack();
  params[1] = cns(returnBcOffset);
  params[2] = callee ? cns(callee) : m_tb->genDefNull();
  SSATmp** decayedPtr = params;
  gen(Call, std::make_pair(numParams + 3, decayedPtr));

  if (!m_fpiStack.empty()) {
    m_fpiStack.pop();
  }
}

void HhbcTranslator::emitFCallBuiltin(uint32_t numArgs,
                                      uint32_t numNonDefault,
                                      int32_t funcId) {
  const NamedEntity* ne = lookupNamedEntityId(funcId);
  const Func* callee = Unit::lookupFunc(ne);

  callee->validate();

  // spill args to stack. We need to spill these for two resons:
  // 1. some of the arguments may be passed by reference, for which
  //    case we will pass a stack address.
  // 2. type conversions of the arguments (using tvCast* helpers)
  //    may throw an exception, so we either need to have the VM stack
  //    in a clean state at that point or give each helper a catch
  //    trace. Since we have to spillstack anyway, the catch trace
  //    would be overkill.
  spillStack();

  // Convert types if needed.
  for (int i = 0; i < numNonDefault; i++) {
    const Func::ParamInfo& pi = callee->params()[i];
    switch (pi.builtinType()) {
      case KindOfBoolean:
      case KindOfInt64:
      case KindOfArray:
      case KindOfObject:
      case KindOfString:
        gen(
          CastStk,
          Type::fromDataType(pi.builtinType(), KindOfInvalid),
          StackOffset(numArgs - i - 1),
          m_tb->sp()
        );
        break;
      case KindOfDouble: not_reached();
      case KindOfUnknown: break;
      default:            not_reached();
    }
  }

  // Pass arguments for CallBuiltin.
  const int argsSize = numArgs + 2;
  SSATmp* args[argsSize];
  args[0] = cns(callee);
  args[1] = m_tb->sp();
  for (int i = numArgs - 1; i >= 0; i--) {
    const Func::ParamInfo& pi = callee->params()[i];
    switch (pi.builtinType()) {
      case KindOfBoolean:
      case KindOfInt64:
        args[i + 2] = top(Type::fromDataType(pi.builtinType(), KindOfInvalid),
                          numArgs - i - 1);
        break;
      case KindOfDouble: assert(false);
      default:
        args[i + 2] = ldStackAddr(numArgs - i - 1);
        break;
    }
  }

  // Generate call and set return type
  auto const ret = gen(
    CallBuiltin,
    Type::fromDataTypeWithRef(callee->returnType(),
                              (callee->attrs() & ClassInfo::IsReference)),
    std::make_pair(argsSize, (SSATmp**)&args)
  );

  // Decref and free args
  for (int i = 0; i < numArgs; i++) {
    auto const arg = popR();
    if (i >= numArgs - numNonDefault) {
      gen(DecRef, arg);
    }
  }

  push(ret);
}

void HhbcTranslator::emitRetFromInlined(Type type) {
  SSATmp* retVal = pop(type);

  assert(!(curFunc()->attrs() & AttrMayUseVV));
  assert(!curFunc()->isPseudoMain());
  assert(!m_fpiStack.empty());

  emitDecRefLocalsInline(retVal);

  /*
   * Pop the ActRec and restore the stack and frame pointers.  It's
   * important that this does endInlining before pushing the return
   * value so stack offsets are properly tracked.
   */
  gen(InlineReturn, m_tb->fp());

  // Return to the caller function.  Careful between here and the
  // emitMarker() below, where the caller state isn't entirely set up.
  m_bcStateStack.pop_back();
  m_fpiStack.pop();

  // See the comment in beginInlining about generator frames.
  if (curFunc()->isGenerator()) {
    gen(ReDefGeneratorSP, StackOffset(m_tb->spOffset()), m_tb->sp());
  } else {
    gen(ReDefSP,
        StackOffset(m_tb->spOffset()), m_tb->fp(), m_tb->sp());
  }

  /*
   * After the end of inlining, we are restoring to a previously
   * defined stack that we know is entirely materialized.  TODO:
   * explain this better.
   *
   * The push of the return value below is not yet materialized.
   */
  assert(m_evalStack.numCells() == 0);
  m_stackDeficit = 0;

  FTRACE(1, "]]] end inlining: {}\n", curFunc()->fullName()->data());
  push(retVal);

  emitMarker();
}

SSATmp* HhbcTranslator::emitDecRefLocalsInline(SSATmp* retVal) {
  SSATmp* retValSrcLoc = nullptr;
  Opcode  retValSrcOpc = Nop; // Nop flags the ref-count opt is impossible
  IRInstruction* retValSrcInstr = retVal->inst();
  const Func* curFunc = this->curFunc();

  /*
   * In case retVal comes from a local, the logic below tweaks the code
   * so that retVal is DecRef'd and the corresponding local's SSATmp is
   * returned. This enables the ref-count optimization to eliminate the
   * IncRef/DecRef pair in the main trace.
   */
  if (retValSrcInstr->op() == IncRef) {
    retValSrcLoc = retValSrcInstr->src(0);
    retValSrcOpc = retValSrcLoc->inst()->op();
    if (retValSrcOpc != LdLoc && retValSrcOpc != LdThis) {
      retValSrcLoc = nullptr;
      retValSrcOpc = Nop;
    }
  }

  if (curFunc->mayHaveThis()) {
    if (retValSrcLoc && retValSrcOpc == LdThis) {
      gen(DecRef, retVal);
    } else {
      gen(DecRefThis, m_tb->fp());
    }
  }

  /*
   * Note: this is currently off for isInlining() because the shuffle
   * was preventing a decref elimination due to ordering.  Currently
   * we don't inline anything with parameters, though, so it doesn't
   * matter.  This will need to be revisted then.
   */
  int retValLocId = (!isInlining() && retValSrcLoc && retValSrcOpc == LdLoc) ?
    retValSrcLoc->inst()->extra<LocalId>()->locId : -1;
  for (int id = curFunc->numLocals() - 1; id >= 0; --id) {
    if (retValLocId == id) {
      gen(DecRef, retVal);
      continue;
    }
    gen(DecRefLoc, Type::Gen, LocalId(id), m_tb->fp());
  }

  return retValSrcLoc ? retValSrcLoc : retVal;
}

void HhbcTranslator::emitRet(Type type, bool freeInline) {
  if (isInlining()) {
    return emitRetFromInlined(type);
  }

  const Func* curFunc = this->curFunc();
  bool mayUseVV = (curFunc->attrs() & AttrMayUseVV);

  gen(ExitWhenSurprised, getExitSlowTrace());
  if (mayUseVV) {
    // Note: this has to be the first thing, because we cannot bail after
    //       we start decRefing locs because then there'll be no corresponding
    //       bytecode boundaries until the end of RetC
    gen(ReleaseVVOrExit, getExitSlowTrace(), m_tb->fp());
  }
  SSATmp* retVal = pop(type);

  SSATmp* sp;
  if (freeInline) {
    SSATmp* useRet = emitDecRefLocalsInline(retVal);
    gen(StRetVal, m_tb->fp(), useRet);
    sp = gen(RetAdjustStack, m_tb->fp());
  } else {
    if (curFunc->mayHaveThis()) {
      gen(DecRefThis, m_tb->fp());
    }
    sp = gen(GenericRetDecRefs, m_tb->fp(), cns(curFunc->numLocals()));
    gen(StRetVal, m_tb->fp(), retVal);
  }

  // Free ActRec, and return control to caller.
  SSATmp* retAddr = gen(LdRetAddr, m_tb->fp());
  SSATmp* fp = gen(FreeActRec, m_tb->fp());
  gen(RetCtrl, sp, fp, retAddr);

  // Flag that this trace has a Ret instruction, so that no ExitTrace is needed
  m_hasExit = true;
}

void HhbcTranslator::emitSwitch(const ImmVector& iv,
                                int64_t base,
                                bool bounded) {
  int nTargets = bounded ? iv.size() - 2 : iv.size();

  SSATmp* const switchVal = popC();
  Type type = switchVal->type();
  assert(IMPLIES(!type.equals(Type::Int), bounded));
  assert(IMPLIES(bounded, iv.size() > 2));
  SSATmp* index;
  SSATmp* ssabase = cns(base);
  SSATmp* ssatargets = cns(nTargets);

  Offset defaultOff = bcOff() + iv.vec32()[iv.size() - 1];
  Offset zeroOff = 0;
  if (base <= 0 && (base + nTargets) > 0) {
    zeroOff = bcOff() + iv.vec32()[0 - base];
  } else {
    zeroOff = defaultOff;
  }

  if (type.subtypeOf(Type::Null)) {
    gen(Jmp_, getExitTrace(zeroOff));
    return;
  } else if (type.subtypeOf(Type::Bool)) {
    Offset nonZeroOff = bcOff() + iv.vec32()[iv.size() - 2];
    gen(JmpNZero, getExitTrace(nonZeroOff), switchVal);
    gen(Jmp_, getExitTrace(zeroOff));
    return;
  } else if (type.subtypeOf(Type::Int)) {
    // No special treatment needed
    index = switchVal;
  } else if (type.subtypeOf(Type::Dbl)) {
    // switch(Double|String|Obj)Helper do bounds-checking for us, so
    // we need to make sure the default case is in the jump table,
    // and don't emit our own bounds-checking code
    bounded = false;
    index = gen(LdSwitchDblIndex,
                      switchVal, ssabase, ssatargets);
  } else if (type.subtypeOf(Type::Str)) {
    bounded = false;
    index = gen(LdSwitchStrIndex,
                      switchVal, ssabase, ssatargets);
  } else if (type.subtypeOf(Type::Obj)) {
    // switchObjHelper can throw exceptions and reenter the VM
    IRTrace* catchTrace = nullptr;
    if (type.subtypeOf(Type::Obj)) {
      catchTrace = getCatchTrace();
    }
    bounded = false;
    index = gen(LdSwitchObjIndex, catchTrace, switchVal, ssabase, ssatargets);
  } else if (type.subtypeOf(Type::Arr)) {
    gen(DecRef, switchVal);
    gen(Jmp_, getExitTrace(defaultOff));
    return;
  } else {
    PUNT(Switch-UnknownType);
  }

  std::vector<Offset> targets(iv.size());
  for (int i = 0; i < iv.size(); i++) {
    targets[i] = bcOff() + iv.vec32()[i];
  }

  JmpSwitchData data;
  data.func        = curFunc();
  data.base        = base;
  data.bounded     = bounded;
  data.cases       = iv.size();
  data.defaultOff  = defaultOff;
  data.targets     = &targets[0];

  auto const stack = spillStack();
  gen(SyncABIRegs, m_tb->fp(), stack);

  gen(JmpSwitchDest, data, index);
  m_hasExit = true;
}

void HhbcTranslator::emitSSwitch(const ImmVector& iv) {
  const int numCases = iv.size() - 1;

  /*
   * We use a fast path translation with a hashtable if none of the
   * cases are numeric strings and if the input is actually a string.
   *
   * Otherwise we do a linear search through the cases calling string
   * conversion routines.
   */
  const bool fastPath =
    topC()->isA(Type::Str) &&
    std::none_of(iv.strvec(), iv.strvec() + numCases,
      [&](const StrVecItem& item) {
        return curUnit()->lookupLitstrId(item.str)->isNumeric();
      }
    );

  IRTrace* catchTrace = nullptr;
  // The slow path can throw exceptions and reenter the VM.
  if (!fastPath) catchTrace = getCatchTrace();

  auto const testVal = popC();

  std::vector<LdSSwitchData::Elm> cases(numCases);
  for (int i = 0; i < numCases; ++i) {
    auto const& kv = iv.strvec()[i];
    cases[i].str  = curUnit()->lookupLitstrId(kv.str);
    cases[i].dest = bcOff() + kv.dest;
  }

  LdSSwitchData data;
  data.func       = curFunc();
  data.numCases   = numCases;
  data.cases      = &cases[0];
  data.defaultOff = bcOff() + iv.strvec()[iv.size() - 1].dest;

  SSATmp* dest = gen(fastPath ? LdSSwitchDestFast
                              : LdSSwitchDestSlow,
                     catchTrace,
                     data,
                     testVal);
  gen(DecRef, testVal);
  auto const stack = spillStack();
  gen(SyncABIRegs, m_tb->fp(), stack);
  gen(JmpIndirect, dest);
  m_hasExit = true;
}

void HhbcTranslator::emitRetC(bool freeInline) {
  emitRet(Type::Cell, freeInline);
}

void HhbcTranslator::emitRetV(bool freeInline) {
  emitRet(Type::BoxedCell, freeInline);
}

void HhbcTranslator::setThisAvailable() {
  m_tb->setThisAvailable();
}

void HhbcTranslator::guardTypeLocal(uint32_t locId, Type type) {
  gen(GuardLoc, type, LocalId(locId), m_tb->fp());
}

void HhbcTranslator::guardTypeLocation(const Location& loc, Type type) {
  assert(type.subtypeOf(Type::Gen | Type::Cls));

  if (loc.isStack()) {
    guardTypeStack(loc.offset, type);
  } else if (loc.isLocal()) {
    assert(type.not(Type::Cls));
    guardTypeLocal(loc.offset, type);
  } else {
    not_reached();
  }
}

void HhbcTranslator::checkTypeLocal(uint32_t locId, Type type,
                                    Offset dest /* = -1 */) {
  gen(CheckLoc, type, LocalId(locId), getExitTrace(dest), m_tb->fp());
}

void HhbcTranslator::assertTypeLocal(uint32_t locId, Type type) {
  gen(AssertLoc, type, LocalId(locId), m_tb->fp());
}

void HhbcTranslator::overrideTypeLocal(uint32_t locId, Type type) {
  gen(OverrideLoc, type, LocalId(locId), m_tb->fp());
}

void HhbcTranslator::checkTypeLocation(const Location& loc, Type type,
                                       Offset dest) {
  assert(type.subtypeOf(Type::Gen));

  if (loc.isStack()) {
    checkTypeStack(loc.offset, type, dest);
  } else if (loc.isLocal()) {
    checkTypeLocal(loc.offset, type, dest);
  } else {
    not_reached();
  }
}

void HhbcTranslator::assertTypeLocation(const Location& loc, Type type) {
  assert(type.subtypeOf(Type::Gen | Type::Cls));

  if (loc.isStack()) {
    assertTypeStack(loc.offset, type);
  } else if (loc.isLocal()) {
    assert(type.not(Type::Cls));
    assertTypeLocal(loc.offset, type);
  } else {
    not_reached();
  }
}

void HhbcTranslator::guardTypeStack(uint32_t stackIndex, Type type) {
  // Should not generate guards for class; instead assert their type
  if (type.subtypeOf(Type::Cls)) {
    assertTypeStack(stackIndex, type);
    return;
  }

  assert(m_evalStack.size() == 0);
  assert(m_stackDeficit == 0); // This should only be called at the beginning
                               // of a trace, with a clean stack.
  gen(GuardStk, type, StackOffset(stackIndex), m_tb->sp());
}

void HhbcTranslator::checkTypeStack(uint32_t idx, Type type, Offset dest) {
  auto exitTrace = getExitTrace(dest);
  if (idx < m_evalStack.size()) {
    FTRACE(1, "checkTypeStack(){}: generating CheckType for {}\n",
           idx, type.toString());
    SSATmp* tmp = m_evalStack.top(idx);
    assert(tmp);
    m_evalStack.replace(idx, gen(CheckType, type, exitTrace, tmp));
  } else {
    FTRACE(1, "checkTypeStack({}): no tmp: {}\n", idx, type.toString());
    gen(CheckStk, type, exitTrace,
        StackOffset(idx - m_evalStack.size () + m_stackDeficit), m_tb->sp());
  }
}

void HhbcTranslator::checkTypeTopOfStack(Type type, Offset nextByteCode) {
  checkTypeStack(0, type, nextByteCode);
}

void HhbcTranslator::assertTypeStack(uint32_t idx, Type type) {
  if (idx < m_evalStack.size()) {
    SSATmp* tmp = m_evalStack.top(idx);
    assert(tmp);
    m_evalStack.replace(idx, gen(AssertType, type, tmp));
  } else {
    gen(AssertStk, type,
        StackOffset(idx - m_evalStack.size() + m_stackDeficit),
        m_tb->sp());
  }
}

void HhbcTranslator::assertString(const Location& loc, const StringData* str) {
  auto idx = loc.offset;

  if (loc.isStack()) {
    if (idx < m_evalStack.size()) {
      DEBUG_ONLY SSATmp* oldStr = m_evalStack.top(idx);
      assert(oldStr->type().maybe(Type::Str));
      m_evalStack.replace(idx, cns(str));
    } else {
      gen(AssertStkVal, StackOffset(idx - m_evalStack.size() + m_stackDeficit),
          m_tb->sp(), cns(str));
    }
  } else if (loc.isLocal()) {
    assert(m_tb->getLocalType(loc.offset).maybe(Type::Str));
    m_tb->setLocalValue(idx, cns(str));
  } else {
    not_reached();
  }
}

/*
 * Creates a RuntimeType struct from a program location. This needs access to
 * more than just the location's type because RuntimeType includes known
 * constant values.
 */
RuntimeType HhbcTranslator::rttFromLocation(const Location& loc) {
  Type t;
  SSATmp* val;
  switch (loc.space) {
    case Location::Stack: {
      auto i = loc.offset;
      assert(i >= 0);
      if (i < m_evalStack.size()) {
        val = m_evalStack.top(i);
        t = val->type();
      } else {
        auto stackVal = getStackValue(m_tb->sp(),
                                      i - m_evalStack.size() + m_stackDeficit);
        val = stackVal.value;
        t = stackVal.knownType;
      }
    } break;
    case Location::Local: {
      auto l = loc.offset;
      val = m_tb->getLocalValue(l);
      t = val ? val->type() : m_tb->getLocalType(l);
    } break;
    case Location::Litstr:
      return RuntimeType(curUnit()->lookupLitstrId(loc.offset));
    case Location::Litint:
      return RuntimeType(loc.offset);
    case Location::This:
      return RuntimeType(KindOfObject, KindOfInvalid, curFunc()->cls());
    case Location::Invalid:
    case Location::Iter:
      not_reached();
  }

  assert(IMPLIES(val, val->type().equals(t)));
  if (val && val->isConst()) {
    // RuntimeType holds constant Bool, Int, Str, and Cls.
    if (val->type().isBool())    return RuntimeType(val->getValBool());
    if (val->type().isInt())     return RuntimeType(val->getValInt());
    if (val->type().isString())  return RuntimeType(val->getValStr());
    if (val->type().isCls())     return RuntimeType(val->getValClass());
  }
  return t.toRuntimeType();
}

static uint64_t packBitVec(const vector<bool>& bits, unsigned i) {
  uint64_t retval = 0;
  assert(i % 64 == 0);
  assert(i < bits.size());
  while (i < bits.size()) {
    retval |= bits[i] << (i % 64);
    if ((++i % 64) == 0) {
      break;
    }
  }
  return retval;
}

void HhbcTranslator::guardRefs(int64_t entryArDelta,
                               const vector<bool>& mask,
                               const vector<bool>& vals) {
  int32_t actRecOff = cellsToBytes(entryArDelta);
  SSATmp* funcPtr = gen(LdARFuncPtr, m_tb->sp(), cns(actRecOff));
  SSATmp* nParams = nullptr;

  for (unsigned i = 0; i < mask.size(); i += 64) {
    assert(i < vals.size());

    uint64_t mask64 = packBitVec(mask, i);
    if (mask64 == 0) {
      continue;
    }
    uint64_t vals64 = packBitVec(vals, i);

    if (i == 0) {
      nParams = cns(64);
    } else if (i == 64) {
      nParams = gen(
        LdRaw, Type::Int, funcPtr, cns(RawMemSlot::FuncNumParams)
      );
    }
    SSATmp* maskTmp = !(mask64>>32) ? cns(mask64) : m_tb->genLdConst(mask64);
    SSATmp* valsTmp = !(vals64>>32) ? cns(vals64) : m_tb->genLdConst(vals64);
    gen(
      GuardRefs,
      funcPtr,
      nParams,
      cns(i),
      maskTmp,
      valsTmp
    );
  }
}

void HhbcTranslator::emitVerifyParamType(int32_t paramId) {
  const Func* func = curFunc();
  const TypeConstraint& tc = func->params()[paramId].typeConstraint();
  auto locVal = ldLoc(paramId);
  Type locType = locVal->type().unbox();
  assert(locType.isKnownDataType());

  if (tc.nullable() && locType.isNull()) {
    return;
  }
  if (tc.isCallable()) {
    locVal = gen(Unbox, getExitTrace(), locVal);
    gen(VerifyParamCallable, getCatchTrace(), locVal, cns(paramId));
    return;
  }

  // For non-object guards, we rely on what we know from the tracelet
  // guards and never have to do runtime checks.
  if (!tc.isObjectOrTypedef()) {
    if (locVal->type().isBoxed()) {
      locVal = gen(LdRef, locVal->type().innerType(), getExitTrace(), locVal);
    }
    if (!tc.checkPrimitive(locType.toDataType())) {
      gen(VerifyParamFail, getCatchTrace(), cns(paramId));
      return;
    }
    return;
  }

  /*
   * If the parameter is an object, we check the object in one of
   * various ways (similar to instance of).  If the parameter is not
   * an object, it still might pass the VerifyParamType if the
   * constraint is a typedef.
   *
   * For now we just interp that case.
   */
  if (!locType.isObj()) {
    emitInterpOne(Type::None, 0);
    return;
  }

  const StringData* clsName;
  const Class* knownConstraint = nullptr;
  if (!tc.isSelf() && !tc.isParent()) {
    clsName = tc.typeName();
    knownConstraint = Unit::lookupClass(clsName);
  } else {
    if (tc.isSelf()) {
      tc.selfToClass(curFunc(), &knownConstraint);
    } else if (tc.isParent()) {
      tc.parentToClass(curFunc(), &knownConstraint);
    }
    if (knownConstraint) {
      clsName = knownConstraint->preClass()->name();
    } else {
      // The hint was self or parent and there's no corresponding
      // class for the current func. This typehint will always fail.
      gen(VerifyParamFail, getCatchTrace(), cns(paramId));
      return;
    }
  }
  assert(clsName);
  // We can only burn in the Class* if it's unique or in the
  // inheritance hierarchy of our context. It's ok if the class isn't
  // defined yet - all paths below are tolerant of a null constraint.
  if (!classIsUniqueOrCtxParent(knownConstraint)) knownConstraint = nullptr;

  Class::initInstanceBits();
  bool haveBit = Class::haveInstanceBit(clsName);
  SSATmp* constraint = knownConstraint ? cns(knownConstraint)
                                       : gen(LdClsCachedSafe, cns(clsName));
  locVal = gen(Unbox, getExitTrace(), locVal);
  SSATmp* objClass = gen(LdObjClass, locVal);
  if (haveBit || classIsUniqueNormalClass(knownConstraint)) {
    SSATmp* isInstance = haveBit
      ? gen(InstanceOfBitmask, objClass, cns(clsName))
      : gen(ExtendsClass, objClass, constraint);
    m_tb->ifThen(curFunc(),
      [&](Block* taken) {
        gen(JmpZero, taken, isInstance);
      },
      [&] { // taken: the param type does not match
        m_tb->hint(Block::Unlikely);
        gen(VerifyParamFail, getCatchTrace(), cns(paramId));
      }
    );
  } else {
    gen(VerifyParamCls,
        getCatchTrace(),
        objClass,
        constraint,
        cns(paramId),
        cns(uintptr_t(&tc)));
  }
}

void HhbcTranslator::emitInstanceOfD(int classNameStrId) {
  const StringData* className = lookupStringId(classNameStrId);
  SSATmp* src = popC();

  /*
   * InstanceOfD is always false if it's not an object.
   *
   * We're prepared to generate translations for known non-object
   * types, but if it's Gen/Cell we're going to PUNT because it's
   * natural to translate that case with control flow TODO(#2020251)
   */
  if (Type::Obj.strictSubtypeOf(src->type())) {
    PUNT(InstanceOfD_MaybeObj);
  }
  if (!src->isA(Type::Obj)) {
    bool res = (src->isA(Type::Arr) && interface_supports_array(className));
    push(cns(res));
    gen(DecRef, src);
    return;
  }

  SSATmp* objClass     = gen(LdObjClass, src);
  SSATmp* ssaClassName = cns(className);

  Class::initInstanceBits();
  const bool haveBit = Class::haveInstanceBit(className);

  Class* const maybeCls = Unit::lookupUniqueClass(className);
  const bool isNormalClass = classIsUniqueNormalClass(maybeCls);
  const bool isUnique = classIsUnique(maybeCls);

  /*
   * If the class is unique or a parent of the current context, we
   * don't need to load it out of target cache because it must
   * already exist and be defined.
   *
   * Otherwise, we only use LdClsCachedSafe---instanceof with an
   * undefined class doesn't invoke autoload.
   */
  SSATmp* checkClass =
    isUnique || (maybeCls && curClass() && curClass()->classof(maybeCls))
      ? cns(maybeCls)
      : gen(LdClsCachedSafe, ssaClassName);

  push(
    haveBit ? gen(InstanceOfBitmask,
                        objClass,
                        ssaClassName) :
    isUnique && isNormalClass ? gen(ExtendsClass,
                                          objClass,
                                          checkClass) :
    gen(InstanceOf,
              objClass,
              checkClass,
              cns(maybeCls && !isNormalClass))
  );
  gen(DecRef, src);
}

void HhbcTranslator::emitCastArray() {
  // Turns the castArray BC operation into a type specialized
  // IR operation. The IR operation might end up being simplified
  // into a constant, but if not, it simply turns into a helper
  // call when translated to machine code. The main benefit from
  // separate IR instructions is that they can have different flags,
  // principally to distinguish the instructions that (may) hold on to a
  // reference to argument, from instructions that do not.

  // In the future, if this instruction occurs in a hot trace,
  // it might be better to expand it into a series of primitive
  // IR instructions so that the object allocation is exposed to
  // the optimizer and becomes eligible for removal if it does not
  // escape the trace.

  SSATmp* src = popC();
  Type fromType = src->type();
  if (fromType.isArray()) {
    push(src);
  } else if (fromType.isNull()) {
    push(cns(HphpArray::GetStaticEmptyArray()));
  } else if (fromType.isBool()) {
    push(gen(ConvBoolToArr, src));
  } else if (fromType.isDbl()) {
    push(gen(ConvDblToArr, src));
  } else if (fromType.isInt()) {
    push(gen(ConvIntToArr, src));
  } else if (fromType.isString()) {
    push(gen(ConvStrToArr, src));
  } else if (fromType.isObj()) {
    push(gen(ConvObjToArr, src));
  } else {
    push(gen(ConvCellToArr, src));
  }
}

void HhbcTranslator::emitCastBool() {
  auto const src = popC();
  push(gen(ConvCellToBool, src));
  gen(DecRef, src);
}

void HhbcTranslator::emitCastDouble() {
  IRTrace* catchTrace = getCatchTrace();
  SSATmp* src = popC();
  Type fromType = src->type();
  if (fromType.isDbl()) {
    push(src);
  } else if (fromType.isNull()) {
    push(cns(0.0));
  } else if (fromType.isArray()) {
    push(gen(ConvArrToDbl, src));
    gen(DecRef, src);
  } else if (fromType.isBool()) {
    push(gen(ConvBoolToDbl, src));
  } else if (fromType.isInt()) {
    push(gen(ConvIntToDbl, src));
  } else if (fromType.isString()) {
    push(gen(ConvStrToDbl, src));
  } else if (fromType.isObj()) {
    push(gen(ConvObjToDbl, catchTrace, src));
  } else {
    push(gen(ConvCellToDbl, catchTrace, src));
  }
}

void HhbcTranslator::emitCastInt() {
  IRTrace* catchTrace = getCatchTrace();
  SSATmp* src = popC();
  Type fromType = src->type();
  if (fromType.isInt()) {
    push(src);
  } else if (fromType.isNull()) {
    push(cns(0));
  } else if (fromType.isArray()) {
    push(gen(ConvArrToInt, src));
    gen(DecRef, src);
  } else if (fromType.isBool()) {
    push(gen(ConvBoolToInt, src));
  } else if (fromType.isDbl()) {
    push(gen(ConvDblToInt, src));
  } else if (fromType.isString()) {
    push(gen(ConvStrToInt, src));
    gen(DecRef, src);
  } else if (fromType.isObj()) {
    push(gen(ConvObjToInt, catchTrace, src));
  } else {
    push(gen(ConvCellToInt, catchTrace, src));
  }
}

void HhbcTranslator::emitCastObject() {
  SSATmp* src = popC();
  Type srcType = src->type();
  if (srcType.isObj()) {
    push(src);
  } else {
    push(gen(ConvCellToObj, src));
  }
}

void HhbcTranslator::emitCastString() {
  IRTrace* catchTrace = getCatchTrace();
  SSATmp* src = popC();
  Type fromType = src->type();
  if (fromType.isString()) {
    push(src);
  } else if (fromType.isNull()) {
    push(cns(StringData::GetStaticString("")));
  } else if (fromType.isArray()) {
    push(cns(StringData::GetStaticString("Array")));
    gen(DecRef, src);
  } else if (fromType.isBool()) {
    push(gen(ConvBoolToStr, src));
  } else if (fromType.isDbl()) {
    push(gen(ConvDblToStr, src));
  } else if (fromType.isInt()) {
    push(gen(ConvIntToStr, src));
  } else if (fromType.isObj()) {
    push(gen(ConvObjToStr, catchTrace, src));
  } else {
    push(gen(ConvCellToStr, catchTrace, src));
  }
}

static
bool isSupportedAGet(SSATmp* classSrc, const StringData* clsName) {
  return (classSrc->isA(Type::Obj) || classSrc->isA(Type::Str) || clsName);
}

void HhbcTranslator::emitAGet(SSATmp* classSrc, const StringData* clsName) {
  if (classSrc->isA(Type::Str)) {
    push(gen(LdCls, classSrc, cns(curClass())));
  } else if (classSrc->isA(Type::Obj)) {
    push(gen(LdObjClass, classSrc));
  } else if (clsName) {
    push(gen(LdCls, cns(clsName), cns(curClass())));
  } else {
    not_reached();
  }
}

void HhbcTranslator::emitAGetC(const StringData* clsName) {
  if (isSupportedAGet(topC(), clsName)) {
    SSATmp* src = popC();
    emitAGet(src, clsName);
    gen(DecRef, src);
  } else {
    emitInterpOne(Type::Cls, 1);
  }
}

void HhbcTranslator::emitAGetL(int id, const StringData* clsName) {
  auto const src = ldLocInner(id, getExitTrace());
  if (isSupportedAGet(src, clsName)) {
    emitAGet(src, clsName);
  } else {
    PUNT(AGetL); // need to teach interpone about local uses
  }
}

void HhbcTranslator::emitBindMem(SSATmp* ptr, SSATmp* src) {
  SSATmp* prevValue = gen(LdMem, ptr->type().deref(), ptr, cns(0));
  pushIncRef(src);
  gen(StMem, ptr, cns(0), src);
  if (isRefCounted(src) && src->type().canRunDtor()) {
    Block* exitBlock = getExitTrace(nextBcOff())->front();
    exitBlock->prepend(m_irFactory.gen(DecRef, prevValue));
    gen(DecRefNZOrBranch, exitBlock, prevValue);
  } else {
    gen(DecRef, prevValue);
  }
}

template<class CheckSupportedFun, class EmitLdAddrFun>
void HhbcTranslator::emitBind(const StringData* name,
                              CheckSupportedFun checkSupported,
                              EmitLdAddrFun emitLdAddr) {
  if (!(this->*checkSupported)(name, topV(0)->type(), 1)) return;
  SSATmp* src = popV();
  emitBindMem((this->*emitLdAddr)(name), src);
}

void HhbcTranslator::emitSetMem(SSATmp* ptr, SSATmp* src) {
  emitBindMem(gen(UnboxPtr, ptr), src);
}

template<class CheckSupportedFun, class EmitLdAddrFun>
void HhbcTranslator::emitSet(const StringData* name,
                             CheckSupportedFun checkSupported,
                             EmitLdAddrFun emitLdAddr) {
  if (!(this->*checkSupported)(name, topC(0)->type(), 1)) return;
  SSATmp* src = popC();
  emitSetMem((this->*emitLdAddr)(name), src);
}

void HhbcTranslator::emitVGetMem(SSATmp* ptr) {
  pushIncRef(
    gen(LdMem, Type::BoxedCell, gen(BoxPtr, ptr), cns(0))
  );
}

template<class CheckSupportedFun, class EmitLdAddrFun>
void HhbcTranslator::emitVGet(const StringData* name,
                              CheckSupportedFun checkSupported,
                              EmitLdAddrFun emitLdAddr) {
  if (!(this->*checkSupported)(name, Type::BoxedCell, 0)) return;
  emitVGetMem((this->*emitLdAddr)(name));
}

template<class CheckSupportedFun, class EmitLdAddrFun>
void HhbcTranslator::emitIsset(const StringData* name,
                               CheckSupportedFun checkSupported,
                               EmitLdAddrFun emitLdAddr) {
  if (!(this->*checkSupported)(name, Type::Bool, 0)) return;
  SSATmp* ptr = nullptr;
  SSATmp* result = m_tb->cond(curFunc(),
                        [&] (Block* taken) { // branch
                          ptr = (this->*emitLdAddr)(name, taken);
                        },
                        [&] { // Next: property or global is defined
                          return gen(IsNTypeMem, Type::Null,
                                           gen(UnboxPtr, ptr));
                        },
                        [&] { // Taken
                          return cns(false);
                        }
  );
  push(result);
}

void HhbcTranslator::emitEmptyMem(SSATmp* ptr) {
  SSATmp* ld = gen(LdMem, Type::Cell, gen(UnboxPtr, ptr), cns(0));
  push(gen(OpNot, gen(ConvCellToBool, ld)));
}

template<class CheckSupportedFun, class EmitLdAddrFun>
void HhbcTranslator::emitEmpty(const StringData* name,
                               CheckSupportedFun checkSupported,
                               EmitLdAddrFun emitLdAddr) {
  if (!(this->*checkSupported)(name, Type::Bool, 0)) return;
  SSATmp* ptr = nullptr;
  SSATmp* result = m_tb->cond(curFunc(),
                        [&] (Block* taken) {
                          ptr = (this->*emitLdAddr)(name, taken);
                        },
                        [&] { // Next: property or global is defined
                          SSATmp* ld = gen(
                            LdMem,
                            Type::Cell,
                            gen(UnboxPtr, ptr),
                            cns(0)
                          );
                          return gen(OpNot, gen(ConvCellToBool, ld));
                        },
                        [&] { // Taken
                          return cns(true);
                        }
  );
  push(result);
}

void HhbcTranslator::emitBindG(const StringData* gblName) {
  emitBind(gblName,
           &HhbcTranslator::checkSupportedGblName,
           &HhbcTranslator::emitLdGblAddrDef);
}

void HhbcTranslator::emitBindS(const StringData* propName) {
  emitBind(propName,
           &HhbcTranslator::checkSupportedClsProp,
           &HhbcTranslator::emitLdClsPropAddr);
}

void HhbcTranslator::emitVGetG(const StringData* gblName) {
  emitVGet(gblName,
           &HhbcTranslator::checkSupportedGblName,
           &HhbcTranslator::emitLdGblAddrDef);
}

void HhbcTranslator::emitVGetS(const StringData* propName) {
  emitVGet(propName,
           &HhbcTranslator::checkSupportedClsProp,
           &HhbcTranslator::emitLdClsPropAddr);
}

void HhbcTranslator::emitSetG(const StringData* gblName) {
  emitSet(gblName,
          &HhbcTranslator::checkSupportedGblName,
          &HhbcTranslator::emitLdGblAddrDef);
}

void HhbcTranslator::emitSetS(const StringData* propName) {
  emitSet(propName,
          &HhbcTranslator::checkSupportedClsProp,
          &HhbcTranslator::emitLdClsPropAddr);
}

static Type getResultType(Type resultType, bool isInferedType) {
  assert(!isInferedType || resultType.isKnownUnboxedDataType());
  if (resultType.equals(Type::None)) {
    // result type neither predicted nor inferred
    return Type::Cell;
  }
  assert(resultType.isKnownUnboxedDataType());
  return resultType;
}

template<class CheckSupportedFun, class EmitLdAddrFun>
void HhbcTranslator::emitCGet(const StringData* name,
                              Type resultType,
                              bool isInferedType,
                              bool exitOnFailure,
                              CheckSupportedFun checkSupported,
                              EmitLdAddrFun emitLdAddr) {
  resultType = getResultType(resultType, isInferedType);
  if (!(this->*checkSupported)(name, resultType, 0)) return;
  IRTrace* exit = (isInferedType || resultType.equals(Type::Cell))
                ? nullptr : getExitSlowTrace();
  SSATmp* ptr = (this->*emitLdAddr)(name,
                                    exitOnFailure
                                      ? getExitSlowTrace()->front()
                                      : nullptr);
  if (!isInferedType) ptr = gen(UnboxPtr, ptr);
  pushIncRef(gen(LdMem, resultType, exit, ptr, cns(0)));
}

void HhbcTranslator::emitCGetG(const StringData* gblName,
                               Type resultType,
                               bool isInferedType) {
  emitCGet(gblName, resultType, isInferedType, true,
           &HhbcTranslator::checkSupportedGblName,
           &HhbcTranslator::emitLdGblAddr);
}

void HhbcTranslator::emitCGetS(const StringData* propName,
                               Type resultType,
                               bool isInferedType) {
  emitCGet(propName, resultType, isInferedType, false,
           &HhbcTranslator::checkSupportedClsProp,
           &HhbcTranslator::emitLdClsPropAddrOrExit);
}

void HhbcTranslator::emitBinaryArith(Opcode opc) {
  bool isBitOp = (opc == OpBitAnd || opc == OpBitOr || opc == OpBitXor);
  Type type1 = topC(0)->type();
  Type type2 = topC(1)->type();
  if (areBinaryArithTypesSupported(opc, type1, type2)) {
    SSATmp* tr = popC();
    SSATmp* tl = popC();
    tr = (tr->isA(Type::Bool) ? gen(ConvBoolToInt, tr) : tr);
    tl = (tl->isA(Type::Bool) ? gen(ConvBoolToInt, tl) : tl);
    push(gen(opc, tl, tr));
  } else {
    Type type = Type::Int;
    if (isBitOp) {
      if (type1.isString() && type2.isString()) {
        type = Type::Str;
      } else if ((type1.needsReg() && (type2.needsReg() || type2.isString()))
                 || (type2.needsReg() && type1.isString())) {
        // both types might be strings, but can't tell
        type = Type::Cell;
      } else {
        type = Type::Int;
      }
    } else {
      // either an int or a dbl, but can't tell
      type = Type::Cell;
    }
    emitInterpOne(type, 2);
  }
}

void HhbcTranslator::emitNot() {
  SSATmp* src = popC();
  push(gen(OpNot, gen(ConvCellToBool, src)));
  gen(DecRef, src);
}

#define BINOP(Opp) \
void HhbcTranslator::emit ## Opp() {  \
  emitBinaryArith(Op ## Opp);         \
}

BINOP(Add)
BINOP(Sub)
BINOP(Mul)
BINOP(BitAnd)
BINOP(BitOr)
BINOP(BitXor)

#undef BINOP

void HhbcTranslator::emitDiv() {
  emitInterpOne(Type::Cell, 2);
}

void HhbcTranslator::emitMod() {
  // XXX: Disabled until t2299606 is fixed
  PUNT(emitMod);

  auto tl = topC(1)->type();
  auto tr = topC(0)->type();
  auto isInty = [&](Type t) {
    return t.subtypeOf(Type::Null | Type::Bool | Type::Int);
  };
  if (!(isInty(tl) && isInty(tr))) {
    emitInterpOne(Type::Cell, 2);
    return;
  }
  SSATmp* r = popC();
  SSATmp* l = popC();
  // Exit path spills an additional false
  auto exitSpillValues = peekSpillValues();
  exitSpillValues.push_back(cns(false));

  // Generate an exit for the rare case that r is zero.  Interpreting
  // will raise a notice and produce the boolean false.  Punch out
  // here and resume after the Mod instruction; this should be rare.
  auto const exit = getExitTraceWarn(
    nextBcOff(),
    exitSpillValues,
    StringData::GetStaticString(Strings::DIVISION_BY_ZERO)
  );
  gen(JmpZero, exit, r);
  push(gen(OpMod, l, r));
}

void HhbcTranslator::emitBitNot() {
  Type srcType = topC()->type();
  if (srcType.subtypeOf(Type::Int)) {
    SSATmp* src = popC();
    push(gen(OpBitNot, src));
  } else {
    Type resultType = Type::Int;
    if (srcType.isString()) {
      resultType = Type::Str;
    } else if (srcType.needsReg()) {
      resultType = Type::Cell;
    }
    emitInterpOne(resultType, 1);
  }
}

void HhbcTranslator::emitXor() {
  SSATmp* btr = popC();
  SSATmp* btl = popC();
  SSATmp* tr = gen(ConvCellToBool, btr);
  SSATmp* tl = gen(ConvCellToBool, btl);
  push(gen(ConvCellToBool, gen(OpLogicXor, tl, tr)));
  gen(DecRef, btl);
  gen(DecRef, btr);
}

/**
 * Emit InterpOne instruction.
 *   - 'type' is the return type of the value the instruction pushes on
 *            the stack if any (or Type:None if none)
 *   - 'numPopped' is the number of cells that this instruction pops
 *   - 'numExtraPushed' is the number of cells this instruction pushes on
 *            the stack, in addition to the cell corresponding to 'type'
 */
void HhbcTranslator::emitInterpOne(Type type, int numPopped,
                                   int numExtraPushed) {
  // We're calling into the interpreter so we want the stack synced to memory.
  SSATmp* sp = spillStack();
  // discard the top elements of the stack, which are consumed by this instr
  discard(numPopped);
  assert(numPopped == m_stackDeficit);
  int numPushed = (type == Type::None ? 0 : 1) + numExtraPushed;
  gen(
    InterpOne,
    type,
    m_tb->fp(),
    sp,
    cns(bcOff()),
    cns(numPopped - numPushed)
  );
  m_stackDeficit = 0;
}

void HhbcTranslator::emitInterpOneCF(int numPopped) {
  // We're calling into the interpreter so we want the stack synced to memory.
  SSATmp* sp = spillStack();
  // discard the top elements of the stack, which are consumed by this instr
  discard(numPopped);
  assert(numPopped == m_stackDeficit);
  gen(InterpOneCF, m_tb->fp(), sp, cns(bcOff()));
  m_stackDeficit = 0;
  m_hasExit = true;
}

std::string HhbcTranslator::showStack() const {
  if (isInlining()) {
    return folly::format("{:*^60}\n",
                         " I don't understand inlining stacks yet ").str();
  }
  std::ostringstream out;
  auto header = [&](const std::string& str) {
    out << folly::format("+{:-^62}+\n", str);
  };

  const int32_t stackDepth = m_tb->spOffset() - curFunc()->numLocals() +
    m_evalStack.size() - m_stackDeficit;
  auto spOffset = stackDepth;
  auto elem = [&](const std::string& str) {
    out << folly::format("| {:<60} |\n",
                         folly::format("{:>2}: {}",
                                       stackDepth - spOffset, str));
    assert(spOffset > 0);
    --spOffset;
  };
  auto fpiStack = m_fpiStack;
  auto checkFpi = [&]() {
    if (!fpiStack.empty() &&
        spOffset - kNumActRecCells == fpiStack.top().second) {
      for (unsigned i = 0; i < kNumActRecCells; ++i) elem("ActRec");
      fpiStack.pop();
      return true;
    }
    return false;
  };

  header(folly::format(" {} stack element(s); m_evalStack: ",
                       stackDepth).str());
  for (unsigned i = 0; i < m_evalStack.size(); ++i) {
    while (checkFpi());
    SSATmp* value = m_evalStack.top(i);
    elem(value->inst()->toString());
  }

  header(" in-memory ");
  for (unsigned i = m_stackDeficit; spOffset > 0; ) {
    assert(i < curFunc()->maxStackCells());
    if (checkFpi()) {
      i += kNumActRecCells;
      continue;
    }

    auto stkVal = getStackValue(m_tb->sp(), i);
    std::ostringstream elemStr;
    if (stkVal.knownType.equals(Type::None)) elem("unknown");
    else if (stkVal.value) elem(stkVal.value->inst()->toString());
    else elem(stkVal.knownType.toString());

    ++i;
  }

  header("");
  return out.str();
}

/*
 * Get SSATmps representing all the information on the virtual eval
 * stack in preparation for a spill or exit trace. Top of stack will
 * be at index 0.
 *
 * Doesn't actually remove these values from the eval stack.
 */
std::vector<SSATmp*> HhbcTranslator::peekSpillValues() const {
  std::vector<SSATmp*> ret;
  ret.reserve(m_evalStack.size());
  for (int i = 0; i < m_evalStack.size(); ++i) {
    SSATmp* elem = m_evalStack.top(i);
    ret.push_back(elem);
  }
  return ret;
}

IRTrace* HhbcTranslator::getExitTrace(Offset targetBcOff /* = -1 */) {
  auto spillValues = peekSpillValues();
  return getExitTrace(targetBcOff, spillValues);
}

IRTrace* HhbcTranslator::getExitTrace(Offset targetBcOff,
                                    std::vector<SSATmp*>& spillValues) {
  if (targetBcOff == -1) targetBcOff = bcOff();
  return getExitTraceImpl(targetBcOff, ExitFlag::None, spillValues,
    CustomExit{});
}

IRTrace* HhbcTranslator::getExitTraceWarn(Offset targetBcOff,
                                        std::vector<SSATmp*>& spillValues,
                                        const StringData* warning) {
  assert(targetBcOff != -1);
  return getExitTraceImpl(targetBcOff, ExitFlag::None, spillValues,
    [&](IRTrace* t) -> SSATmp* {
      genFor(t, RaiseWarning, cns(warning));
      return nullptr;
    }
  );
}

template<class ExitLambda>
IRTrace* HhbcTranslator::makeSideExit(Offset targetBcOff, ExitLambda exit) {
  auto spillValues = peekSpillValues();
  return getExitTraceImpl(targetBcOff,
                          ExitFlag::DelayedMarker,
                          spillValues,
                          exit);
}

IRTrace* HhbcTranslator::getExitSlowTrace() {
  auto spillValues = peekSpillValues();
  return getExitTraceImpl(bcOff(), ExitFlag::NoIR, spillValues,
    CustomExit{});
}

IRTrace* HhbcTranslator::getExitTraceImpl(Offset targetBcOff,
                                        ExitFlag flag,
                                        std::vector<SSATmp*>& stackValues,
                                        const CustomExit& customFn) {
  auto const exit = m_tb->makeExitTrace(targetBcOff);

  MarkerData exitMarker;
  exitMarker.bcOff    = targetBcOff;
  exitMarker.stackOff = m_tb->spOffset() +
                          stackValues.size() - m_stackDeficit;
  exitMarker.func     = curFunc();

  MarkerData currentMarker;
  currentMarker.bcOff     = bcOff();
  currentMarker.func      = curFunc();
  currentMarker.stackOff  = m_tb->spOffset() +
                              m_evalStack.numCells() - m_stackDeficit;

  genFor(exit, Marker,
         flag == ExitFlag::DelayedMarker ? currentMarker : exitMarker);

  // The value we use for stack is going to depend on whether we have
  // to spillstack or what.
  auto stack = m_tb->sp();

  // TODO(#2404447) move this conditional to the simplifier?
  if (m_stackDeficit != 0 || !stackValues.empty()) {
    stackValues.insert(
      stackValues.begin(),
      { m_tb->sp(), cns(int64_t(m_stackDeficit)) }
    );
    stack = genFor(exit,
      SpillStack, std::make_pair(stackValues.size(), &stackValues[0])
    );
  }

  if (customFn) {
    stack = genFor(exit, ExceptionBarrier, stack);
    auto const customTmp = customFn(exit);
    if (customTmp) {
      SSATmp* spill2[] = { stack, cns(0), customTmp };
      stack = genFor(exit,
        SpillStack, std::make_pair(sizeof spill2 / sizeof spill2[0], spill2)
      );
      exitMarker.stackOff += 1;
    }
  }

  if (flag == ExitFlag::DelayedMarker) {
    genFor(exit, Marker, exitMarker);
  }

  genFor(exit, SyncABIRegs, m_tb->fp(), stack);

  if (flag == ExitFlag::NoIR) {
    genFor(exit,
      targetBcOff == m_startBcOff ? ReqRetranslateNoIR : ReqBindJmpNoIR,
      BCOffset(targetBcOff)
    );
    return exit;
  }

  if (bcOff() == m_startBcOff && targetBcOff == m_startBcOff) {
    genFor(exit, ReqRetranslate);
  } else {
    genFor(exit, ReqBindJmp, BCOffset(targetBcOff));
  }

  return exit;
}

/*
 * Create a catch trace for the current state of the eval stack. This is a
 * trace intended to be invoked by the unwinder while unwinding a frame
 * containing a call to C++ from translated code. When attached to an
 * instruction as its taken field, code will be generated and the trace will be
 * registered with the unwinder automatically.
 */
IRTrace* HhbcTranslator::getCatchTrace() {
  auto exit = m_tb->makeExitTrace(bcOff());
  assert(exit->blocks().size() == 1);

  genFor(exit, BeginCatch);
  exit->front()->push_back(makeMarker(bcOff()));
  auto sp = emitSpillStack(exit, m_tb->sp(), peekSpillValues());
  genFor(exit, EndCatch, sp);

  assert(exit->blocks().size() == 1);
  return exit;
}

SSATmp* HhbcTranslator::emitSpillStack(IRTrace* t, SSATmp* sp,
                                       const std::vector<SSATmp*>& spillVals) {
  std::vector<SSATmp*> ssaArgs{ sp, cns(int64_t(m_stackDeficit)) };
  ssaArgs.insert(ssaArgs.end(), spillVals.begin(), spillVals.end());

  auto args = std::make_pair(ssaArgs.size(), &ssaArgs[0]);
  if (t->isMain()) {
    return gen(SpillStack, args);
  } else {
    return genFor(t, SpillStack, args);
  }
}

SSATmp* HhbcTranslator::spillStack() {
  auto newSp =
    emitSpillStack(m_tb->trace(), m_tb->sp(), peekSpillValues());
  m_evalStack.clear();
  m_stackDeficit = 0;
  return newSp;
}

void HhbcTranslator::exceptionBarrier() {
  auto const sp = spillStack();
  gen(ExceptionBarrier, sp);
}

SSATmp* HhbcTranslator::ldStackAddr(int32_t offset) {
  // You're almost certainly doing it wrong if you want to get the address of a
  // stack cell that's in m_evalStack.
  assert(offset >= (int32_t)m_evalStack.numCells());
  return gen(
    LdStackAddr,
    Type::PtrToGen,
    StackOffset(offset + m_stackDeficit - m_evalStack.numCells()),
    m_tb->sp()
  );
}

SSATmp* HhbcTranslator::ldLoc(uint32_t locId) {
  return gen(
    LdLoc,
    Type::Gen,
    LocalId(locId),
    m_tb->fp()
  );
}

SSATmp* HhbcTranslator::ldLocAddr(uint32_t locId) {
  return gen(
    LdLocAddr,
    Type::PtrToGen,
    LocalId(locId),
    m_tb->fp()
  );
}

/*
 * Load a local, and if it's boxed dereference to get the inner cell.
 *
 * Note: For boxed values, this will generate a LdRef instruction which
 *       takes the given exit trace in case the inner type doesn't match
 *       the tracked type for this local.  This check may be optimized away
 *       if we can determine that the inner type must match the tracked type.
 */
SSATmp* HhbcTranslator::ldLocInner(uint32_t locId, IRTrace* exitTrace) {
  auto loc = ldLoc(locId);
  assert((loc->type().isBoxed() || loc->type().notBoxed()) &&
         "Currently we don't handle traces where locals are maybeBoxed");
  return loc->type().isBoxed()
    ? gen(LdRef, loc->type().innerType(), exitTrace, loc)
    : loc;
}

/*
 * This is a wrapper to ldLocInner that also emits the RaiseUninitLoc if the
 * local is uninitialized. The catchTrace argument may be provided if the
 * caller requires the catch trace to be generated at a point earlier than when
 * it calls this function.
 */
SSATmp* HhbcTranslator::ldLocInnerWarn(uint32_t id, IRTrace* target,
                                       IRTrace* catchTrace /*   = nullptr */) {
  if (!catchTrace) catchTrace = getCatchTrace();
  auto const locVal = ldLocInner(id, target);

  if (locVal->type().subtypeOf(Type::Uninit)) {
    gen(RaiseUninitLoc, catchTrace, cns(curFunc()->localVarName(id)));
    return m_tb->genDefInitNull();
  }

  return locVal;
}

/*
 * Store to a local, if it's boxed set the value on the inner cell.
 *
 * Returns the value that was stored to the local, after incrementing
 * its reference count.
 *
 * Pre: !newVal->type().isBoxed() && !newVal->type().maybeBoxed()
 * Pre: exitTrace != nullptr if the local may be boxed
 */
SSATmp* HhbcTranslator::stLocImpl(uint32_t id,
                                  IRTrace* exitTrace,
                                  SSATmp* newVal,
                                  bool doRefCount) {
  assert(!newVal->type().maybeBoxed());

  auto const oldLoc = ldLoc(id);
  if (!(oldLoc->type().isBoxed() || oldLoc->type().notBoxed())) {
    PUNT(stLocImpl-maybeBoxedValue);
  }

  if (oldLoc->type().notBoxed()) {
    gen(StLoc, LocalId(id), m_tb->fp(), newVal);
    auto const ret = doRefCount ? gen(IncRef, newVal) : newVal;
    if (doRefCount) {
      gen(DecRef, oldLoc);
    }
    return ret;
  }

  // It's important that the IncRef happens after the LdRef, since the
  // LdRef is also a guard on the inner type and may side-exit.
  assert(exitTrace);
  auto const innerCell = gen(
    LdRef, oldLoc->type().innerType(), exitTrace, oldLoc
  );
  auto const ret = doRefCount ? gen(IncRef, newVal) : newVal;
  gen(StRef, oldLoc, newVal);
  if (doRefCount) {
    gen(DecRef, innerCell);
  }

  return ret;
}

SSATmp* HhbcTranslator::stLoc(uint32_t id, IRTrace* exit, SSATmp* newVal) {
  const bool doRefCount = true;
  return stLocImpl(id, exit, newVal, doRefCount);
}

SSATmp* HhbcTranslator::stLocNRC(uint32_t id, IRTrace* exit, SSATmp* newVal) {
  const bool doRefCount = false;
  return stLocImpl(id, exit, newVal, doRefCount);
}

void HhbcTranslator::end() {
  if (m_hasExit) return;

  auto const nextSk = curSrcKey().advanced(curUnit());
  auto const nextPc = nextSk.offset();
  if (nextPc >= curFunc()->past()) {
    // We have fallen off the end of the func's bytecodes. This happens
    // when the function's bytecodes end with an unconditional
    // backwards jump so that nextPc is out of bounds and causes an
    // assertion failure in unit.cpp. The common case for this comes
    // from the default value funclets, which are placed after the end
    // of the function, with an unconditional branch back to the start
    // of the function. So you should see this in any function with
    // default params.
    return;
  }
  setBcOff(nextPc, true);
  auto const sp = spillStack();
  gen(SyncABIRegs, m_tb->fp(), sp);
  gen(ReqBindJmp, BCOffset(nextPc));
}


void HhbcTranslator::checkStrictlyInteger(
    SSATmp*& key, KeyType& keyType, bool& checkForInt) {
  checkForInt = false;
  if (key->isA(Type::Int)) {
    keyType = IntKey;
  } else {
    assert(key->isA(Type::Str));
    keyType = StrKey;
    if (key->isConst()) {
      int64_t i;
      if (key->getValStr()->isStrictlyInteger(i)) {
        keyType = IntKey;
        key = cns(i);
      }
    } else {
      checkForInt = true;
    }
  }
}

}} // namespace HPHP::JIT
