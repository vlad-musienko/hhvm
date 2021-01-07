/*

  +----------------------------------------------------------------------+
  | HipHop for PHP                                                       |
  +----------------------------------------------------------------------+
  | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
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

#include "hphp/runtime/base/bespoke/monotype-dict.h"

#include "hphp/runtime/base/array-data-defs.h"
#include "hphp/runtime/base/bespoke-array.h"
#include "hphp/runtime/base/memory-manager.h"
#include "hphp/runtime/base/memory-manager-defs.h"
#include "hphp/runtime/base/mixed-array-defs.h"
#include "hphp/runtime/base/runtime-option.h"
#include "hphp/runtime/base/static-string-table.h"

#include "hphp/runtime/vm/jit/mcgen-translate.h"
#include "hphp/runtime/vm/jit/type.h"
#include "hphp/runtime/vm/vm-regs.h"

#include <algorithm>
#include <atomic>

namespace HPHP { namespace bespoke {

TRACE_SET_MOD(bespoke);

//////////////////////////////////////////////////////////////////////////////

namespace {

struct StaticStrPtr {
  bool operator==(const StaticStrPtr& o) const { return value == o.value; }
  bool operator!=(const StaticStrPtr& o) const { return value != o.value; }
  const StringData* value;
};

uint16_t packSizeIndexAndAuxBits(uint8_t index, uint8_t aux) {
  return (static_cast<uint16_t>(index) << 8) | aux;
}

constexpr LayoutIndex kBaseLayoutIndex = {1 << 11};

constexpr size_t kEmptySizeIndex = 0;
static_assert(kSizeIndex2Size[kEmptySizeIndex] == sizeof(EmptyMonotypeDict));

std::aligned_storage<sizeof(EmptyMonotypeDict), 16>::type s_emptyDict;
std::aligned_storage<sizeof(EmptyMonotypeDict), 16>::type s_emptyDArray;
std::aligned_storage<sizeof(EmptyMonotypeDict), 16>::type s_emptyMarkedDict;
std::aligned_storage<sizeof(EmptyMonotypeDict), 16>::type s_emptyMarkedDArray;


const LayoutFunctions* emptyVtable() {
  static auto const result = fromArray<EmptyMonotypeDict>();
  return &result;
}

const LayoutFunctions* intVtable() {
  static auto const result = fromArray<MonotypeDict<int64_t>>();
  return &result;
}

const LayoutFunctions* strVtable() {
  static auto const result = fromArray<MonotypeDict<StringData*>>();
  return &result;
}

const LayoutFunctions* staticStrVtable() {
  static auto const result = fromArray<MonotypeDict<StaticStrPtr>>();
  return &result;
}

constexpr DataType kEmptyDataType = static_cast<DataType>(1);
constexpr DataType kAbstractDataTypeMask = static_cast<DataType>(0x80);

using StringDict = MonotypeDict<StringData*>;

constexpr LayoutIndex getEmptyLayoutIndex() {
  auto constexpr offset = 4 * (1 << 8);
  auto constexpr base = kBaseLayoutIndex.raw;
  static_assert((StringDict::intKeyMask().raw & (base + offset)) == 0);
  return LayoutIndex{uint16_t(base + offset)};
}
constexpr LayoutIndex getIntLayoutIndex(DataType type) {
  auto constexpr offset = 2 * (1 << 8);
  auto constexpr base = kBaseLayoutIndex.raw;
  static_assert((StringDict::intKeyMask().raw & (base + offset)) != 0);
  return LayoutIndex{uint16_t(base + offset + uint8_t(type))};
}
constexpr LayoutIndex getStrLayoutIndex(DataType type) {
  auto constexpr offset = 1 * (1 << 8);
  auto constexpr base = kBaseLayoutIndex.raw;
  static_assert((StringDict::intKeyMask().raw & (base + offset)) == 0);
  return LayoutIndex{uint16_t(base + offset + uint8_t(type))};
}
constexpr LayoutIndex getStaticStrLayoutIndex(DataType type) {
  auto constexpr offset = 0 * (1 << 8);
  auto constexpr base = kBaseLayoutIndex.raw;
  static_assert((StringDict::intKeyMask().raw & (base + offset)) == 0);
  return LayoutIndex{uint16_t(base + offset + uint8_t(type))};
}

const LayoutFunctions* getVtableForKeyTypes(KeyTypes kt) {
  switch (kt) {
    case KeyTypes::Ints:          return intVtable();
    case KeyTypes::Strings:       return strVtable();
    case KeyTypes::StaticStrings: return staticStrVtable();
    default: always_assert(false);
  }
}

Layout::LayoutSet getTopMonotypeDictParents(KeyTypes kt) {
  return {kt == KeyTypes::StaticStrings
    ? TopMonotypeDictLayout::Index(KeyTypes::Strings)
    : AbstractLayout::GetBespokeTopIndex()};
}

Layout::LayoutSet getEmptyOrMonotypeDictParents(KeyTypes kt, DataType type) {
  if (kt == KeyTypes::StaticStrings) {
    return {TopMonotypeDictLayout::Index(kt),
            EmptyOrMonotypeDictLayout::Index(KeyTypes::Strings, type)};
  }
  return {TopMonotypeDictLayout::Index(kt)};
}

Layout::LayoutSet getAllEmptyOrMonotypeDictLayouts() {
  Layout::LayoutSet result;
  using L = EmptyOrMonotypeDictLayout;
#define DT(name, value) {                                     \
    auto const type = KindOf##name;                           \
    if (type == dt_modulo_persistence(type)) {                \
      result.insert(L::Index(KeyTypes::Ints, type));          \
      result.insert(L::Index(KeyTypes::Strings, type));       \
      result.insert(L::Index(KeyTypes::StaticStrings, type)); \
    }                                                         \
  }
  DATATYPES
#undef DT
  return result;
}

LayoutIndex getMonotypeParentLayout(KeyTypes kt, DataType dt) {
  if (!hasPersistentFlavor(dt) || isRefcountedType(dt)) {
    return EmptyOrMonotypeDictLayout::Index(kt, dt_modulo_persistence(dt));
  }

  return MonotypeDictLayout::Index(kt, dt_modulo_persistence(dt));
}

}

//////////////////////////////////////////////////////////////////////////////

EmptyMonotypeDict* EmptyMonotypeDict::As(ArrayData* ad) {
  auto const result = reinterpret_cast<EmptyMonotypeDict*>(ad);
  assertx(result->checkInvariants());
  return result;
}
const EmptyMonotypeDict* EmptyMonotypeDict::As(const ArrayData* ad) {
  return As(const_cast<ArrayData*>(ad));
}
EmptyMonotypeDict* EmptyMonotypeDict::GetDict(bool legacy) {
  auto const mem = legacy ? &s_emptyMarkedDict : &s_emptyDict;
  return reinterpret_cast<EmptyMonotypeDict*>(mem);
}
EmptyMonotypeDict* EmptyMonotypeDict::GetDArray(bool legacy) {
  auto const mem = legacy ? &s_emptyMarkedDArray : &s_emptyDArray;
  return reinterpret_cast<EmptyMonotypeDict*>(mem);
}

bool EmptyMonotypeDict::checkInvariants() const {
  assertx(isStatic());
  assertx(m_size == 0);
  assertx(isDArray() || isDictType());
  assertx(layoutIndex() == getEmptyLayoutIndex());
  return true;
}

//////////////////////////////////////////////////////////////////////////////

size_t EmptyMonotypeDict::HeapSize(const Self*) {
  return sizeof(EmptyMonotypeDict);
}
void EmptyMonotypeDict::Scan(const Self* ad, type_scan::Scanner& scanner) {
}
ArrayData* EmptyMonotypeDict::EscalateToVanilla(
    const Self* ad, const char* reason) {
  auto const legacy = ad->isLegacyArray();
  return ad->isDictType()
    ? (legacy ? staticEmptyMarkedDictArray() : staticEmptyDictArray())
    : (legacy ? staticEmptyMarkedDArray() : staticEmptyDArray());
}
void EmptyMonotypeDict::ConvertToUncounted(
    Self* ad, DataWalker::PointerMap* seen) {
}
void EmptyMonotypeDict::ReleaseUncounted(Self* ad) {
}
void EmptyMonotypeDict::Release(Self* ad) {
  tl_heap->objFreeIndex(ad, kEmptySizeIndex);
}

//////////////////////////////////////////////////////////////////////////////
// Accessors

bool EmptyMonotypeDict::IsVectorData(const Self* ad) {
  return true;
}
TypedValue EmptyMonotypeDict::NvGetInt(const Self* ad, int64_t k) {
  return make_tv<KindOfUninit>();
}
TypedValue EmptyMonotypeDict::NvGetStr(const Self* ad, const StringData* k) {
  return make_tv<KindOfUninit>();
}
TypedValue EmptyMonotypeDict::GetPosKey(const Self* ad, ssize_t pos) {
  always_assert(false);
}
TypedValue EmptyMonotypeDict::GetPosVal(const Self* ad, ssize_t pos) {
  always_assert(false);
}
ssize_t EmptyMonotypeDict::GetIntPos(const Self* ad, int64_t k) {
  return 0;
}
ssize_t EmptyMonotypeDict::GetStrPos(const Self* ad, const StringData* k) {
  return 0;
}

ssize_t EmptyMonotypeDict::IterBegin(const Self* ad) {
  return 0;
}
ssize_t EmptyMonotypeDict::IterLast(const Self* ad) {
  return 0;
}
ssize_t EmptyMonotypeDict::IterEnd(const Self* ad) {
  return 0;
}
ssize_t EmptyMonotypeDict::IterAdvance(const Self* ad, ssize_t prev) {
  return 0;
}
ssize_t EmptyMonotypeDict::IterRewind(const Self* ad, ssize_t prev) {
  return 0;
}

//////////////////////////////////////////////////////////////////////////////
// Mutations

namespace {
template <typename Key>
ArrayData* makeStrMonotypeDict(
    HeaderKind kind, bool legacy, StringData* k, TypedValue v) {
  auto const mad = MonotypeDict<Key>::MakeReserve(kind, legacy, 1, type(v));
  auto const result = MonotypeDict<Key>::SetStr(mad, k, v);
  assertx(result == mad);
  return result;
}
}

arr_lval EmptyMonotypeDict::LvalInt(Self* ad, int64_t k) {
  throwOOBArrayKeyException(k, ad);
}
arr_lval EmptyMonotypeDict::LvalStr(Self* ad, StringData* k) {
  throwOOBArrayKeyException(k, ad);
}
tv_lval EmptyMonotypeDict::ElemInt(
    tv_lval lval, int64_t k, bool throwOnMissing) {
  if (throwOnMissing) throwOOBArrayKeyException(k, lval.val().parr);
  return const_cast<TypedValue*>(&immutable_null_base);
}
tv_lval EmptyMonotypeDict::ElemStr(
    tv_lval lval, StringData* k, bool throwOnMissing) {
  throwOOBArrayKeyException(k, lval.val().parr);
  return const_cast<TypedValue*>(&immutable_null_base);
}

ArrayData* EmptyMonotypeDict::SetInt(Self* ad, int64_t k, TypedValue v) {
  auto const mad = MonotypeDict<int64_t>::MakeReserve(
      ad->m_kind, ad->isLegacyArray(), 1, type(v));
  auto const result = MonotypeDict<int64_t>::SetInt(mad, k, v);
  assertx(result == mad);
  return result;
}
ArrayData* EmptyMonotypeDict::SetIntMove(Self* ad, int64_t k, TypedValue v) {
  auto const mad = SetInt(ad, k, v);
  tvDecRefGen(v);
  return mad;
}
ArrayData* EmptyMonotypeDict::SetStr(Self* ad, StringData* k, TypedValue v) {
  auto const legacy = ad->isLegacyArray();
  return k->isStatic()
    ? makeStrMonotypeDict<StaticStrPtr>(ad->m_kind, legacy, k, v)
    : makeStrMonotypeDict<StringData*>(ad->m_kind, legacy, k, v);
}
ArrayData* EmptyMonotypeDict::SetStrMove(Self* ad, StringData* k, TypedValue v) {
  auto const mad = SetStr(ad, k, v);
  tvDecRefGen(v);
  return mad;
}
ArrayData* EmptyMonotypeDict::RemoveInt(Self* ad, int64_t k) {
  return ad;
}
ArrayData* EmptyMonotypeDict::RemoveStr(Self* ad, const StringData* k) {
  return ad;
}

ArrayData* EmptyMonotypeDict::Append(Self* ad, TypedValue v) {
  return SetInt(ad, 0, v);
}
ArrayData* EmptyMonotypeDict::AppendMove(Self* ad, TypedValue v) {
  return SetIntMove(ad, 0, v);
}
ArrayData* EmptyMonotypeDict::Pop(Self* ad, Variant& ret) {
  ret = uninit_null();
  return ad;
}

//////////////////////////////////////////////////////////////////////////////

ArrayData* EmptyMonotypeDict::ToDVArray(Self* ad, bool copy) {
  return GetDArray(false);
}
ArrayData* EmptyMonotypeDict::ToHackArr(Self* ad, bool copy) {
  return GetDict(false);
}
ArrayData* EmptyMonotypeDict::PreSort(Self* ead, SortFunction sf) {
  always_assert(false);
}
ArrayData* EmptyMonotypeDict::PostSort(Self* ead, ArrayData* vad) {
  always_assert(false);
}
ArrayData* EmptyMonotypeDict::SetLegacyArray(Self* ad, bool copy, bool legacy) {
  return ad->isDArray() ? GetDArray(legacy) : GetDict(legacy);
}

//////////////////////////////////////////////////////////////////////////////

namespace {

constexpr uint8_t kArrSize = 16;
constexpr uint8_t kElmSize = 16;
constexpr uint8_t kIndexSize = 2;

constexpr size_t kMinNumElms = 6;
constexpr size_t kMinNumIndices = 8;
constexpr uint8_t kMinSizeIndex = 7;

constexpr size_t kMaxNumElms = 0xc000;
constexpr size_t kMinSize = kArrSize +
                            kElmSize * kMinNumElms +
                            kIndexSize * kMinNumIndices;

constexpr bool isValidSizeIndex(uint8_t index) {
  assertx(index >= kMinSizeIndex);
  assertx((index - kMinSizeIndex) % kSizeClassesPerDoubling == 0);
  return true;
}
constexpr size_t scaleBySizeIndex(size_t base, uint8_t index) {
  assertx(isValidSizeIndex(index));
  return base << ((index - kMinSizeIndex) / kSizeClassesPerDoubling);
}

static_assert(kMaxNumElms % kMinNumElms == 0);
static_assert(scaleBySizeIndex(1, kMinSizeIndex) == 1);
static_assert(kSizeIndex2Size[kMinSizeIndex] == kMinSize);

template <typename Key>
constexpr LayoutIndex getLayoutIndex(DataType type) {
  if constexpr (std::is_same<Key, int64_t>::value) {
    return getIntLayoutIndex(type);
  } else if constexpr (std::is_same<Key, StringData*>::value) {
    return getStrLayoutIndex(type);
  } else {
    static_assert(std::is_same<Key, StaticStrPtr>::value);
    return getStaticStrLayoutIndex(type);
  }
}

template <typename Key>
constexpr strhash_t getHash(Key key) {
  if constexpr (std::is_same<Key, int64_t>::value) {
    return hash_int64(key) | STRHASH_MSB;
  } else if constexpr (std::is_same<Key, StringData*>::value) {
    return key->hash();
  } else {
    static_assert(std::is_same<Key, StaticStrPtr>::value);
    return key.value->hashStatic();
  }
}

template <typename Key>
constexpr Key getTombstone() {
  if constexpr (std::is_same<Key, int64_t>::value) {
    return std::numeric_limits<Key>::min();
  } else {
    static_assert(std::is_same<Key, StringData*>::value ||
                  std::is_same<Key, StaticStrPtr>::value);
    return Key { nullptr };
  }
}

template <typename Key>
void incRefKey(Key key) {
  if constexpr (std::is_same<Key, StringData*>::value) {
    key->incRefCount();
  }
}

template <typename Key>
void decRefKey(Key key) {
  if constexpr (std::is_same<Key, StringData*>::value) {
    decRefStr(key);
  }
}

// NOTE: Coercion may fail, for both int and string `input`, in which case the
// functions below will return the "tombstone" key for the Key type.
//
// If coercion fails for an int input, the key is DEFINITELY not in the array.
// If coercion fails for a string input, the input may be a non-static  copy
// of a static string (if Key == StaticStr). Otherwise, it's missing.
//
// Callers can use `fallbackForStaticStr` to check the static/non-static case.

template <typename Key>
Key coerceKey(int64_t input) {
  if constexpr (std::is_same<Key, int64_t>::value) {
    return input;
  }
  return getTombstone<Key>();
}

template <typename Key>
Key coerceKey(const StringData* input) {
  if constexpr (std::is_same<Key, StringData*>::value) {
    return const_cast<StringData*>(input);
  } else if constexpr (std::is_same<Key, StaticStrPtr>::value) {
    return input->isStatic() ? Key { input } : getTombstone<Key>();
  }
  return getTombstone<Key>();
}

template <typename Key>
bool fallbackForStaticStr(const StringData* input) {
  if constexpr (std::is_same<Key, StaticStrPtr>::value) {
    return !input->isStatic();
  }
  return false;
}

template <typename Key>
bool keysEqual(Key a, Key b) {
  if constexpr (std::is_same<Key, StringData*>::value) {
    return a == b || a->same(b);
  } else {
    static_assert(std::is_same<Key, int64_t>::value ||
                  std::is_same<Key, StaticStrPtr>::value);
    return a == b;
  }
}

}

//////////////////////////////////////////////////////////////////////////////

template <typename Key>
uint8_t MonotypeDict<Key>::ComputeSizeIndex(size_t size) {
  assertx(0 < size);
  assertx(size <= kMaxNumElms);
  auto capacity = kMinNumElms;
  auto result = kMinSizeIndex;
  while (capacity < size) {
    capacity *= 2;
    result += kSizeClassesPerDoubling;
  }
  return result;
}

template <typename Key> template <bool Static>
MonotypeDict<Key>* MonotypeDict<Key>::MakeReserve(
    HeaderKind kind, bool legacy, size_t capacity, DataType dt) {
  auto const index = ComputeSizeIndex(capacity);
  auto const alloc = [&]{
    if (!Static) return tl_heap->objMallocIndex(index);
    auto const size = MemoryManager::sizeIndex2Size(index);
    return RO::EvalLowStaticArrays ? low_malloc(size) : uncounted_malloc(size);
  }();

  auto const mad = static_cast<MonotypeDict<Key>*>(alloc);
  auto const aux = packSizeIndexAndAuxBits(
      index, legacy ? ArrayData::kLegacyArray : 0);

  mad->initHeader_16(kind, OneReference, aux);
  mad->setLayoutIndex(getLayoutIndex<Key>(dt));
  mad->m_extra_lo16 = 0;
  mad->m_size = 0;
  mad->initHash();

  assertx(mad->checkInvariants());
  return mad;
}

template <typename Key>
MonotypeDict<Key>* MonotypeDict<Key>::MakeFromVanilla(
    ArrayData* ad, DataType dt) {
  assertx(ad->size() <= kMaxNumElms);
  assertx(ad->hasVanillaMixedLayout());
  auto const kind = ad->isDArray() ? HeaderKind::BespokeDArray
                                   : HeaderKind::BespokeDict;
  auto result = ad->isStatic()
    ? MakeReserve<true>(kind, ad->isLegacyArray(), ad->size(), dt)
    : MakeReserve<false>(kind, ad->isLegacyArray(), ad->size(), dt);

  MixedArray::IterateKV(MixedArray::asMixed(ad), [&](auto k, auto v) {
    auto const next = tvIsString(k) ? SetStr(result, val(k).pstr, v)
                                    : SetInt(result, val(k).num, v);
    assertx(result == next);
    result = As(next);
  });

  if (ad->isStatic()) {
    auto const aux = packSizeIndexAndAuxBits(
      result->sizeIndex(), result->auxBits());
    result->initHeader_16(kind, StaticValue, aux);
  }

  assertx(result->checkInvariants());
  return result;
}

template <typename Key>
MonotypeDict<Key>* MonotypeDict<Key>::As(ArrayData* ad) {
  auto const result = reinterpret_cast<MonotypeDict<Key>*>(ad);
  assertx(result->checkInvariants());
  return result;
}
template <typename Key>
const MonotypeDict<Key>* MonotypeDict<Key>::As(const ArrayData* a) {
  return As(const_cast<ArrayData*>(a));
}

template <typename Key>
bool MonotypeDict<Key>::checkInvariants() const {
  static_assert(kArrSize == sizeof(*this));
  static_assert(kElmSize == sizeof(Elm));
  static_assert(kIndexSize == sizeof(Index));
  assertx(isRealType(type()));
  assertx(isDArray() || isDictType());
  assertx(isValidSizeIndex(sizeIndex()));
  assertx(size() <= used());
  assertx(IMPLIES(!isZombie(), used() <= numElms()));

  // We may call StringDict's methods on a dict of static strings.
  assertx(layoutIndex() == getLayoutIndex<Key>(type()) ||
          (std::is_same<Self, StringDict>::value &&
           layoutIndex() == getLayoutIndex<StaticStrPtr>(type())));

  return true;
}

//////////////////////////////////////////////////////////////////////////////

template <typename Key> template <typename Result>
Result MonotypeDict<Key>::find(Key key, strhash_t hash) {
  auto constexpr IsAdd = std::is_same<Result, Add>::value;
  auto constexpr IsGet = std::is_same<Result, Get>::value;
  auto constexpr IsRemove = std::is_same<Result, Remove>::value;
  auto constexpr IsUpdate = std::is_same<Result, Update>::value;
  static_assert(IsAdd || IsGet || IsRemove || IsUpdate);
  assertx(IsAdd || key != getTombstone<Key>());

  auto const data = indices();
  auto const mask = numIndices() - 1;
  auto i = uint32_t(hash) & mask;
  auto delta = 1;
  auto first = 0;

  for (auto index = data[i]; index != kEmptyIndex;
       i = (i + delta) & mask, delta++, index = data[i]) {
    if constexpr (IsAdd) continue;

    if (index == kTombstoneIndex) {
      if constexpr (IsUpdate) {
        if (!first) first = i + 1;
      }
      continue;
    }

    auto const elm = elmAtIndex(index);
    if (keysEqual(elm->key, key)) {
      if constexpr (IsGet) return {elm};
      if constexpr (IsRemove) return {safe_cast<ssize_t>(i)};
      if constexpr (IsUpdate) return {elm, &data[i]};
    }
  }

  if constexpr (IsAdd) return {&data[i], safe_cast<size_t>(delta)};
  if constexpr (IsGet) return {nullptr};
  if constexpr (IsRemove) return {-1};
  if constexpr (IsUpdate) return {nullptr, &data[first ? first - 1 : i]};
}

template <typename Key>
typename MonotypeDict<Key>::Add
MonotypeDict<Key>::findForAdd(strhash_t hash) {
  return find<Add>(getTombstone<Key>(), hash);
}

template <typename Key>
const typename MonotypeDict<Key>::Elm*
MonotypeDict<Key>::findForGet(Key key, strhash_t hash) const {
  auto const mad = const_cast<MonotypeDict<Key>*>(this);
  return mad->template find<Get>(key, hash).elm;
}

template <typename Key>
TypedValue MonotypeDict<Key>::getImpl(Key key) const {
  if (key == getTombstone<Key>()) return make_tv<KindOfUninit>();
  auto const result = findForGet(key, getHash(key));
  return result ? make_tv_of_type(result->val, type())
                : make_tv<KindOfUninit>();
}

template <typename Key>
ssize_t MonotypeDict<Key>::getPosImpl(Key key) const {
  if (key == getTombstone<Key>()) return used();
  auto const result = findForGet(key, getHash(key));
  return result ? ptrdiff_t(result - elms()) / sizeof(Elm) : used();
}

template <typename Key>
ArrayData* MonotypeDict<Key>::removeImpl(Key key) {
  if (key == getTombstone<Key>()) return this;
  auto const hash_pos = find<Remove>(key, getHash(key)).hash_pos;
  if (hash_pos < 0) return this;

  auto const mad = cowCheck() ? copy() : this;
  auto& index = mad->indices()[hash_pos];
  auto const elm = mad->elmAtIndex(index);
  assertx(keysEqual(elm->key, key));

  decRefKey(elm->key);
  tvDecRefGen(make_tv_of_type(elm->val, type()));
  elm->key = getTombstone<Key>();
  index = kTombstoneIndex;
  mad->m_size--;
  return mad;
}
template <typename Key>
arr_lval MonotypeDict<Key>::lvalDispatch(int64_t k) {
  return LvalInt(this, k);
}

template <typename Key>
arr_lval MonotypeDict<Key>::lvalDispatch(StringData* k) {
  return LvalStr(this, k);
}

template <typename Key> template <typename K>
arr_lval MonotypeDict<Key>::elemImpl(Key key, K k, bool throwOnMissing) {
  if (key == getTombstone<Key>()) {
    if (throwOnMissing) throwOOBArrayKeyException(k, this);
    return {this, const_cast<TypedValue*>(&immutable_null_base)};
  }

  auto const dt = type();
  if (dt == KindOfClsMeth) return lvalDispatch(k);

  auto const old = findForGet(key, getHash(key));
  if (old == nullptr) {
    if (throwOnMissing) throwOOBArrayKeyException(k, this);
    return {this, const_cast<TypedValue*>(&immutable_null_base)};
  }

  auto const mad = cowCheck() ? copy() : this;
  auto const elm = old - elms() + mad->elms();
  assertx(keysEqual(elm->key, key));
  mad->setLayoutIndex(getLayoutIndex<Key>(dt_modulo_persistence(dt)));

  static_assert(folly::kIsLittleEndian);
  auto const type_ptr = reinterpret_cast<DataType*>(&mad->m_extra_hi16);
  assertx(*type_ptr == mad->type());
  return arr_lval{mad, type_ptr, const_cast<Value*>(&elm->val)};
}

template <typename Key> template <bool Move, typename K>
ArrayData* MonotypeDict<Key>::setImpl(Key key, K k, TypedValue v) {
  auto const dt = type();
  if (key == getTombstone<Key>() || used() == kMaxNumElms ||
      !equivDataTypes(dt, v.type())) {
    auto const ad = escalateWithCapacity(size() + 1);
    auto const result = ad->set(k, v);
    assertx(ad == result);
    if constexpr (Move) {
      if (decReleaseCheck()) Release(this);
      tvDecRefGen(v);
    }
    return result;
  }
  if constexpr (!Move) {
    tvIncRefGen(v);
  }

  // Handle escalation, both from a persistent value type to a counted one,
  // and from a dict with static string keys to one with string keys.
  auto const result = prepareForInsert();
  if (dt != v.type()) {
    result->setLayoutIndex(getLayoutIndex<Key>(dt_with_rc(dt)));
  } else if constexpr (std::is_same<Self, StringDict>::value) {
    result->setLayoutIndex(getLayoutIndex<Key>(dt));
  }

  auto const update = result->template find<Update>(key, getHash(key));
  if (update.elm != nullptr) {
    tvDecRefGen(make_tv_of_type(update.elm->val, dt));
    update.elm->val = v.val();
  } else {
    incRefKey(key);
    *update.index = safe_cast<Index>(result->used());
    *result->elmAtIndex(*update.index) = { key, v.val() };
    result->m_extra_lo16++;
    result->m_size++;
  }
  if constexpr (Move) {
    if (result != this && decReleaseCheck()) Release(this);
  }
  return result;
}

template <typename Key>
template <typename KeyFn, typename CountedFn,
          typename MaybeCountedFn, typename UncountedFn>
void MonotypeDict<Key>::forEachElm(
    KeyFn k, CountedFn c, MaybeCountedFn m, UncountedFn u) const {
  auto const dt = type();
  auto const limit = used();
  if (m_size == limit) {
    if (isRefcountedType(dt)) {
      if (hasPersistentFlavor(dt)) {
        for (auto i = 0; i < limit; i++) {
          auto const elm = elmAtIndex(i);
          k(i, elm->key);
          m(make_tv_of_type(elm->val, dt));
        }
      } else {
        for (auto i = 0; i < limit; i++) {
          auto const elm = elmAtIndex(i);
          k(i, elm->key);
          c(make_tv_of_type(elm->val, dt));
        }
      }
    } else {
      for (auto i = 0; i < limit; i++) {
        auto const elm = elmAtIndex(i);
        k(i, elm->key);
        u(make_tv_of_type(elm->val, dt));
      }
    }
  } else {
    if (isRefcountedType(dt)) {
      if (hasPersistentFlavor(dt)) {
        for (auto i = 0; i < limit; i++) {
          auto const elm = elmAtIndex(i);
          if (elm->key == getTombstone<Key>()) continue;
          k(i, elm->key);
          m(make_tv_of_type(elm->val, dt));
        }
      } else {
        for (auto i = 0; i < limit; i++) {
          auto const elm = elmAtIndex(i);
          if (elm->key == getTombstone<Key>()) continue;
          k(i, elm->key);
          c(make_tv_of_type(elm->val, dt));
        }
      }
    } else {
      for (auto i = 0; i < limit; i++) {
        auto const elm = elmAtIndex(i);
        if (elm->key == getTombstone<Key>()) continue;
        k(i, elm->key);
        u(make_tv_of_type(elm->val, dt));
      }
    }
  }
}

template <typename Key> template <typename ElmFn>
void MonotypeDict<Key>::forEachElm(ElmFn e) const {
  auto const limit = used();
  if (m_size == limit) {
      for (auto i = 0; i < limit; i++) {
        e(i, elmAtIndex(i));
      }
  } else {
    for (auto i = 0; i < limit; i++) {
      auto const elm = elmAtIndex(i);
      if (elm->key != getTombstone<Key>()) e(i, elm);
    }
  }
}

template <typename Key>
void MonotypeDict<Key>::incRefElms() {
  forEachElm(
    [&](auto i, auto k) { incRefKey(k); },
    [&](auto v) { reinterpret_cast<Countable*>(val(v).pcnt)->incRefCount(); },
    [&](auto v) { val(v).pcnt->incRefCount(); },
    [&](auto v) {}
  );
}

template <typename Key>
void MonotypeDict<Key>::decRefElms() {
  auto const dt = type();
  forEachElm(
    [&](auto i, auto k) { decRefKey(k); },
    [&](auto v) {
      auto const countable = reinterpret_cast<Countable*>(val(v).pcnt);
      if (countable->decReleaseCheck()) destructorForType(dt)(countable);
    },
    [&](auto v) {
      auto const countable = val(v).pcnt;
      if (countable->decReleaseCheck()) destructorForType(dt)(countable);
    },
    [&](auto v) {}
  );
}

template <typename Key>
void MonotypeDict<Key>::copyHash(const Self* other) {
  static_assert(kMinNumIndices * sizeof(Index) % 16 == 0);
  assertx(uintptr_t(indices()) % 16 == 0);

  memcpy16_inline(indices(), other->indices(), numIndices() * sizeof(Index));
}

template <typename Key>
void MonotypeDict<Key>::initHash() {
  static_assert(kEmptyIndex == Index(-1));
  static_assert(kMinNumIndices * sizeof(Index) % 16 == 0);
  assertx(uintptr_t(indices()) % 16 == 0);

  auto const data = indices();
  auto cur = reinterpret_cast<int64_t*>(data);
  auto end = reinterpret_cast<int64_t*>(data + numIndices());
  for (; cur < end; cur++) *cur = -1;
}

template <typename Key>
MonotypeDict<Key>* MonotypeDict<Key>::copy() {
  auto const mem = tl_heap->objMallocIndex(sizeIndex());
  auto const ad = reinterpret_cast<MonotypeDict<Key>*>(mem);

  // Adding 24 bytes so that we can copy elements in 32-byte groups.
  // We might overwrite ad's indices here, but they're not initialized yet.
  auto const bytes = sizeof(*this) + sizeof(Elm) * used() + 24;
  bcopy32_inline(ad, this, bytes);

  auto const aux = packSizeIndexAndAuxBits(sizeIndex(), auxBits());
  ad->initHeader_16(m_kind, OneReference, aux);
  ad->copyHash(this);
  ad->incRefElms();

  assertx(ad->checkInvariants());
  return ad;
}

template <typename Key>
MonotypeDict<Key>* MonotypeDict<Key>::prepareForInsert() {
  auto const copy = cowCheck();
  if (used() == numElms()) {
    return resize(sizeIndex() + kSizeClassesPerDoubling, copy);
  }
  return copy ? this->copy() : this;
}

template <typename Key>
MonotypeDict<Key>* MonotypeDict<Key>::resize(uint8_t index, bool copy) {
  auto const mem = tl_heap->objMallocIndex(index);
  auto const ad = reinterpret_cast<MonotypeDict<Key>*>(mem);

  // Adding 24 bytes so that we can copy elements in 32-byte groups.
  // We might overwrite ad's indices here, but they're not initialized yet.
  auto const bytes = sizeof(*this) + sizeof(Elm) * used() + 24;
  bcopy32_inline(ad, this, bytes);

  auto const aux = packSizeIndexAndAuxBits(index, auxBits());
  ad->initHeader_16(m_kind, OneReference, aux);
  ad->initHash();

  // We don't want to check the chain condition on each insert, so we use a
  // kind of approximate max by taking bitwise-ORs.
  size_t chain = 0;
  ad->forEachElm([&](auto i, auto elm) {
    auto const add = ad->findForAdd(getHash(elm->key));
    chain |= add.chain_length;
    *add.index = i;
  });

  if (chain > 2048 && chain > folly::nextPowTwo(size_t(RO::MaxArrayChain))) {
    tl_heap->objFreeIndex(ad, index);
    raise_error("Array is too unbalanced (%u)", RO::MaxArrayChain + 1);
  }

  if (copy) {
    ad->incRefElms();
  } else {
    setZombie();
  }

  assertx(ad->checkInvariants());
  return ad;
}

template <typename Key>
ArrayData* MonotypeDict<Key>::escalateWithCapacity(size_t capacity) const {
  assertx(capacity >= size());
  auto const space = capacity - size() + used();
  auto ad = isDictType() ? MixedArray::MakeReserveDict(space)
                         : MixedArray::MakeReserveDArray(space);
  ad->setLegacyArrayInPlace(isLegacyArray());

  auto const dt = type();
  for (auto elm = elms(), end = elm + used(); elm < end; elm++) {
    // To support local iteration (where the base is not const), iterator
    // indices must match between this array and its vanilla counterpart.
    if (elm->key == getTombstone<Key>()) {
      MixedArray::AppendTombstoneInPlace(ad);
      continue;
    }

    auto const tv = make_tv_of_type(elm->val, dt);
    auto const result = [&]{
      if constexpr (std::is_same<Key, int64_t>::value) {
        return MixedArray::SetInt(ad, elm->key, tv);
      } else if constexpr (std::is_same<Key, StringData*>::value) {
        return MixedArray::SetStr(ad, elm->key, tv);
      } else {
        static_assert(std::is_same<Key, StaticStrPtr>::value);
        auto const key = const_cast<StringData*>(elm->key.value);
        return MixedArray::SetStr(ad, key, tv);
      }
    }();
    assertx(ad == result);
    ad = result;
  }

  assertx(ad->size() == size());
  assertx(ad->iter_end() == iter_end());

  return ad;
}

//////////////////////////////////////////////////////////////////////////////

template <typename Key>
typename MonotypeDict<Key>::Elm* MonotypeDict<Key>::elms() {
  static_assert(sizeof(*this) == entriesOffset());
  return reinterpret_cast<Elm*>(reinterpret_cast<char*>(this + 1));
}

template <typename Key>
const typename MonotypeDict<Key>::Elm* MonotypeDict<Key>::elms() const {
  return const_cast<MonotypeDict<Key>*>(this)->elms();
}

template <typename Key>
typename MonotypeDict<Key>::Elm* MonotypeDict<Key>::elmAtIndex(Index index) {
  assertx(0 <= index);
  assertx(index < numElms());
  return &elms()[index];
}

template <typename Key>
const typename MonotypeDict<Key>::Elm*
MonotypeDict<Key>::elmAtIndex(Index index) const {
  return const_cast<MonotypeDict<Key>*>(this)->elmAtIndex(index);
}

template <typename Key>
typename MonotypeDict<Key>::Index* MonotypeDict<Key>::indices() {
  auto const elms = sizeof(Elm) * numElms();
  return reinterpret_cast<Index*>(reinterpret_cast<char*>(this + 1) + elms);
}

template <typename Key>
const typename MonotypeDict<Key>::Index* MonotypeDict<Key>::indices() const {
  return const_cast<MonotypeDict<Key>*>(this)->indices();
}

template <typename Key>
DataType MonotypeDict<Key>::type() const {
  return DataType(int8_t(m_extra_hi16 & 0xff));
}

template <typename Key>
uint32_t MonotypeDict<Key>::used() const {
  return m_extra_lo16;
}

template <typename Key>
uint8_t MonotypeDict<Key>::sizeIndex() const {
  return m_aux16 >> 8;
}

template <typename Key>
size_t MonotypeDict<Key>::numElms() const {
  return scaleBySizeIndex(kMinNumElms, sizeIndex());
}

template <typename Key>
size_t MonotypeDict<Key>::numIndices() const {
  return scaleBySizeIndex(kMinNumIndices, sizeIndex());
}

template <typename Key>
void MonotypeDict<Key>::setZombie() {
  m_extra_lo16 = uint16_t(-1);
}

template <typename Key>
bool MonotypeDict<Key>::isZombie() const {
  return m_extra_lo16 == uint16_t(-1);
}

//////////////////////////////////////////////////////////////////////////////

template <typename Key>
size_t MonotypeDict<Key>::HeapSize(const Self* mad) {
  return scaleBySizeIndex(kMinSize, mad->sizeIndex());
}

template <typename Key>
void MonotypeDict<Key>::Scan(const Self* mad, type_scan::Scanner& scanner) {
  auto const dt = mad->type();
  mad->forEachElm([&](auto i, auto elm) {
    if constexpr (std::is_same<Key, StringData*>::value) scanner.scan(elm->key);
    if (isRefcountedType(dt)) scanner.scan(elm->val.pcnt);
  });
}

template <typename Key>
ArrayData* MonotypeDict<Key>::EscalateToVanilla(
    const Self* mad, const char* reason) {
  return mad->escalateWithCapacity(mad->size());
}

template <typename Key>
void MonotypeDict<Key>::ConvertToUncounted(
    Self* mad, DataWalker::PointerMap* seen) {
  auto const dt = mad->type();

  mad->forEachElm([&](auto i, auto elm) {
    auto const elm_mut = const_cast<Elm*>(elm);
    if constexpr (std::is_same<Key, StringData*>::value) {
      auto tv = make_tv<KindOfString>(elm_mut->key);
      ConvertTvToUncounted(&tv, seen);
      elm_mut->key = val(tv).pstr;
    }
    auto dt_mut = dt;
    ConvertTvToUncounted(tv_lval(&dt_mut, &elm_mut->val), seen);
    assertx(equivDataTypes(dt_mut, dt));
  });

  auto const newType = static_cast<data_type_t>(dt) & kHasPersistentMask
    ? dt_with_persistence(dt)
    : dt;
  mad->setLayoutIndex(getLayoutIndex<Key>(newType));
}

template <typename Key>
void MonotypeDict<Key>::ReleaseUncounted(Self* mad) {
  auto const dt = mad->type();

  mad->forEachElm([&](auto i, auto elm) {
    if constexpr (std::is_same<Key, StringData*>::value) {
      if (elm->key->isUncounted()) StringData::ReleaseUncounted(elm->key);
    }
    auto tv = make_tv_of_type(elm->val, dt);
    ReleaseUncountedTv(&tv);
  });
}

template <typename Key>
void MonotypeDict<Key>::Release(Self* mad) {
  mad->fixCountForRelease();
  assertx(mad->isRefCounted());
  assertx(mad->hasExactlyOneRef());
  if (!mad->isZombie()) mad->decRefElms();
  tl_heap->objFreeIndex(mad, mad->sizeIndex());
}

//////////////////////////////////////////////////////////////////////////////

template <typename Key>
bool MonotypeDict<Key>::IsVectorData(const Self* mad) {
  if (mad->empty()) return true;
  if constexpr (std::is_same<Key, int64_t>::value) {
    auto next = 0;
    auto const limit = mad->used();
    for (auto i = 0; i < limit; i++) {
      auto const elm = mad->elmAtIndex(i);
      if (elm->key == getTombstone<Key>()) continue;
      if (elm->key != next) return false;
      next++;
    }
    return true;
  }
  return false;
}

template <typename Key>
TypedValue MonotypeDict<Key>::NvGetInt(const Self* mad, int64_t k) {
  return mad->getImpl(coerceKey<Key>(k));
}

template <typename Key>
TypedValue MonotypeDict<Key>::NvGetStr(const Self* mad, const StringData* k) {
  if (fallbackForStaticStr<Key>(k)) {
    return StringDict::NvGetStr(reinterpret_cast<const StringDict*>(mad), k);
  }
  return mad->getImpl(coerceKey<Key>(k));
}

template <typename Key>
TypedValue MonotypeDict<Key>::GetPosKey(const Self* mad, ssize_t pos) {
  auto const elm = mad->elmAtIndex(pos);
  assertx(elm->key != getTombstone<Key>());
  if constexpr (std::is_same<Key, int64_t>::value) {
    return make_tv<KindOfInt64>(elm->key);
  } else if constexpr (std::is_same<Key, StringData*>::value) {
    return make_tv<KindOfString>(elm->key);
  } else {
    static_assert(std::is_same<Key, StaticStrPtr>::value);
    auto const key = const_cast<StringData*>(elm->key.value);
    return make_tv<KindOfPersistentString>(key);
  }
}

template <typename Key>
TypedValue MonotypeDict<Key>::GetPosVal(const Self* mad, ssize_t pos) {
  auto const elm = mad->elmAtIndex(pos);
  assertx(elm->key != getTombstone<Key>());
  return make_tv_of_type(elm->val, mad->type());
}

template <typename Key>
ssize_t MonotypeDict<Key>::GetIntPos(const Self* mad, int64_t k) {
  return mad->getPosImpl(coerceKey<Key>(k));
}

template <typename Key>
ssize_t MonotypeDict<Key>::GetStrPos(const Self* mad, const StringData* k) {
  if (fallbackForStaticStr<Key>(k)) {
    return StringDict::GetStrPos(reinterpret_cast<const StringDict*>(mad), k);
  }
  return mad->getPosImpl(coerceKey<Key>(k));
}

template <typename Key>
ssize_t MonotypeDict<Key>::IterBegin(const Self* mad) {
  return IterAdvance(mad, -1);
}
template <typename Key>
ssize_t MonotypeDict<Key>::IterLast(const Self* mad) {
  return IterRewind(mad, mad->used());
}
template <typename Key>
ssize_t MonotypeDict<Key>::IterEnd(const Self* mad) {
  return mad->used();
}
template <typename Key>
ssize_t MonotypeDict<Key>::IterAdvance(const Self* mad, ssize_t pos) {
  auto const limit = mad->used();
  for (pos++; pos < limit; pos++) {
    if (mad->elmAtIndex(pos)->key != getTombstone<Key>()) return pos;
  }
  return limit;
}
template <typename Key>
ssize_t MonotypeDict<Key>::IterRewind(const Self* mad, ssize_t pos) {
  for (pos--; pos >= 0; pos--) {
    if (mad->elmAtIndex(pos)->key != getTombstone<Key>()) return pos;
  }
  return mad->used();
}

//////////////////////////////////////////////////////////////////////////////

template <typename Key>
arr_lval MonotypeDict<Key>::LvalInt(Self* mad, int64_t k) {
  auto const vad = EscalateToVanilla(mad, __func__);
  auto const result = vad->lval(k);
  assertx(result.arr == vad);
  return result;
}

template <typename Key>
arr_lval MonotypeDict<Key>::LvalStr(Self* mad, StringData* k) {
  auto const vad = EscalateToVanilla(mad, __func__);
  auto const result = vad->lval(k);
  assertx(result.arr == vad);
  return result;
}

template <typename Key>
tv_lval MonotypeDict<Key>::ElemInt(
    tv_lval lvalIn, int64_t k, bool throwOnMissing) {
  auto const madIn = As(lvalIn.val().parr);
  auto const lval = madIn->elemImpl(coerceKey<Key>(k), k, throwOnMissing);
  if (lval.arr != madIn) {
    lvalIn.type() = dt_with_rc(lvalIn.type());
    lvalIn.val().parr = lval.arr;
    if (madIn->decReleaseCheck()) Release(madIn);
  }
  return lval;
}

template <typename Key>
tv_lval MonotypeDict<Key>::ElemStr(
    tv_lval lvalIn, StringData* k, bool throwOnMissing) {
  if (fallbackForStaticStr<Key>(k)) {
    return StringDict::ElemStr(lvalIn, k, throwOnMissing);
  }
  auto const madIn = As(lvalIn.val().parr);
  auto const lval = madIn->elemImpl(coerceKey<Key>(k), k, throwOnMissing);
  if (lval.arr != madIn) {
    lvalIn.type() = dt_with_rc(lvalIn.type());
    lvalIn.val().parr = lval.arr;
    if (madIn->decReleaseCheck()) Release(madIn);
  }
  return lval;
}

template <typename Key>
ArrayData* MonotypeDict<Key>::SetInt(Self* mad, int64_t k, TypedValue v) {
  return mad->setImpl<false>(coerceKey<Key>(k), k, v);
}

template <typename Key>
ArrayData* MonotypeDict<Key>::SetIntMove(Self* mad, int64_t k, TypedValue v) {
  return mad->setImpl<true>(coerceKey<Key>(k), k, v);
}

template <typename Key>
ArrayData* MonotypeDict<Key>::SetStr(Self* mad, StringData* k, TypedValue v) {
  if (fallbackForStaticStr<Key>(k)) {
    return StringDict::SetStr(reinterpret_cast<StringDict*>(mad), k, v);
  }
  return mad->setImpl<false>(coerceKey<Key>(k), k, v);
}

template <typename Key>
ArrayData* MonotypeDict<Key>::SetStrMove(Self* mad, StringData* k, TypedValue v) {
  if (fallbackForStaticStr<Key>(k)) {
    return StringDict::SetStrMove(reinterpret_cast<StringDict*>(mad), k, v);
  }
  return mad->setImpl<true>(coerceKey<Key>(k), k, v);
}

template <typename Key>
ArrayData* MonotypeDict<Key>::RemoveInt(Self* mad, int64_t k) {
  return mad->removeImpl(coerceKey<Key>(k));
}

template <typename Key>
ArrayData* MonotypeDict<Key>::RemoveStr(Self* mad, const StringData* k) {
  if (fallbackForStaticStr<Key>(k)) {
    return StringDict::RemoveStr(reinterpret_cast<StringDict*>(mad), k);
  }
  return mad->removeImpl(coerceKey<Key>(k));
}

template <typename Key> template <bool Move>
ArrayData* MonotypeDict<Key>::appendImpl(TypedValue v) {
  auto nextKI = int64_t{0};
  if constexpr (std::is_same<Key, int64_t>::value) {
    forEachElm([&](auto i, auto elm) {
      if (elm->key >= nextKI && nextKI >= 0) {
        nextKI = static_cast<uint64_t>(elm->key) + 1;
      }
    });
  }
  if (UNLIKELY(nextKI < 0)) {
    raise_warning("Cannot add element to the array as the next element is "
                  "already occupied");
    return this;
  }
  return Move ? SetIntMove(this, nextKI, v) : SetInt(this, nextKI, v);
}

template <typename Key>
ArrayData* MonotypeDict<Key>::Append(Self* mad, TypedValue v) {
  return mad->appendImpl<false>(v);
}

template <typename Key>
ArrayData* MonotypeDict<Key>::AppendMove(Self* mad, TypedValue v) {
  return mad->appendImpl<true>(v);
}

template <typename Key>
ArrayData* MonotypeDict<Key>::Pop(Self* mad, Variant& value) {
  if (mad->empty()) {
    value = uninit_null();
    return mad;
  }
  auto const pos = IterLast(mad);
  auto const key = GetPosKey(mad, pos);
  value = tvAsCVarRef(GetPosVal(mad, pos));
  return tvIsString(key) ? RemoveStr(mad, val(key).pstr)
                         : RemoveInt(mad, val(key).num);
}

template <typename Key>
ArrayData* MonotypeDict<Key>::ToDVArray(Self* madIn, bool copy) {
  if (madIn->isDArray()) return madIn;
  auto const mad = copy ? madIn->copy() : madIn;
  mad->m_kind = HeaderKind::BespokeDArray;
  assertx(mad->checkInvariants());
  return mad;
}

template <typename Key>
ArrayData* MonotypeDict<Key>::ToHackArr(Self* madIn, bool copy) {
  if (madIn->isDictType()) return madIn;
  auto const mad = copy ? madIn->copy() : madIn;
  mad->m_kind = HeaderKind::BespokeDict;
  mad->setLegacyArrayInPlace(false);
  assertx(mad->checkInvariants());
  return mad;
}

template <typename Key>
ArrayData* MonotypeDict<Key>::PreSort(Self* mad, SortFunction sf) {
  return mad->escalateWithCapacity(mad->size());
}

// Some sort types can change the keys of a dict or darray.
namespace {
ArrayData* MonotypeDictPostSort(MixedArray* mad, DataType dt) {
  auto const keys = mad->keyTypes();
  auto const result = [&]() -> ArrayData* {
    if (keys.mustBeStaticStrs()) {
      return MonotypeDict<StaticStrPtr>::MakeFromVanilla(mad, dt);
    } else if (keys.mustBeStrs()) {
      return MonotypeDict<StringData*>::MakeFromVanilla(mad, dt);
    } else if (keys.mustBeInts()) {
      return MonotypeDict<int64_t>::MakeFromVanilla(mad, dt);
    }
    return nullptr;
  }();
  if (!result) return mad;
  MixedArray::Release(mad);
  return result;
}
}

template <typename Key>
ArrayData* MonotypeDict<Key>::PostSort(Self* mad, ArrayData* vad) {
  return MonotypeDictPostSort(MixedArray::asMixed(vad), mad->type());
}

template <typename Key>
ArrayData* MonotypeDict<Key>::SetLegacyArray(
    Self* madIn, bool copy, bool legacy) {
  auto const mad = copy ? madIn->copy() : madIn;
  mad->setLegacyArrayInPlace(legacy);
  return mad;
}

//////////////////////////////////////////////////////////////////////////////

using namespace jit;

namespace {
Type applicableKeyType(KeyTypes kt) {
  switch (kt) {
    case KeyTypes::Empty:
      return TBottom;
    case KeyTypes::Ints:
      return TInt;
    case KeyTypes::StaticStrings:
    case KeyTypes::Strings:
      return TStr;
    case KeyTypes::Any:
      return TInt | TStr;
  }
  not_reached();
}

Type resKeyType(KeyTypes kt) {
  switch (kt) {
    case KeyTypes::Empty:
      return TBottom;
    case KeyTypes::Ints:
      return TInt;
    case KeyTypes::StaticStrings:
      return TStaticStr;
    case KeyTypes::Strings:
      return TStr;
    case KeyTypes::Any:
      return TInt | TStr;
  }
  not_reached();
}
}

std::pair<Type, bool> TopMonotypeDictLayout::elemType(Type key) const {
  auto const validKey = key.maybe(applicableKeyType(m_keyType));
  return {validKey ? TInitCell : TBottom, false};
}

std::pair<Type, bool> TopMonotypeDictLayout::firstLastType(
    bool isFirst, bool isKey) const {
  return {isKey ? resKeyType(m_keyType) : TInitCell, false};
}

Type TopMonotypeDictLayout::iterPosType(Type pos, bool isKey) const {
  return isKey ? resKeyType(m_keyType) : TInitCell;
}

std::pair<Type, bool> EmptyMonotypeDictLayout::elemType(Type key) const {
  return {TBottom, false};
}

std::pair<Type, bool> EmptyMonotypeDictLayout::firstLastType(
    bool isFirst, bool isKey) const {
  return {TBottom, false};
}

Type EmptyMonotypeDictLayout::iterPosType(Type pos, bool isKey) const {
  return TBottom;
}

std::pair<Type, bool> EmptyOrMonotypeDictLayout::elemType(Type key) const {
  auto const validKey = key.maybe(applicableKeyType(m_keyType));
  return {validKey ? Type(m_valType) : TBottom, false};
}

std::pair<Type, bool> EmptyOrMonotypeDictLayout::firstLastType(
    bool isFirst, bool isKey) const {
  return {isKey ? resKeyType(m_keyType) : Type(m_valType), false};
}

Type EmptyOrMonotypeDictLayout::iterPosType(Type pos, bool isKey) const {
  return isKey ? resKeyType(m_keyType) : Type(m_valType);
}

std::pair<Type, bool> MonotypeDictLayout::elemType(Type key) const {
  auto const validKey = key.maybe(applicableKeyType(m_keyType));
  return {validKey ? Type(m_valType) : TBottom, false};
}

std::pair<Type, bool> MonotypeDictLayout::firstLastType(
    bool isFirst, bool isKey) const {
  return {isKey ? resKeyType(m_keyType) : Type(m_valType), false};
}

Type MonotypeDictLayout::iterPosType(Type pos, bool isKey) const {
  return isKey ? resKeyType(m_keyType) : Type(m_valType);
}

//////////////////////////////////////////////////////////////////////////////

void EmptyMonotypeDict::InitializeLayouts() {
  auto const base = Layout::ReserveIndices(1 << 11);
  always_assert(base == kBaseLayoutIndex);

  new TopMonotypeDictLayout(KeyTypes::Ints);
  new TopMonotypeDictLayout(KeyTypes::Strings);
  new TopMonotypeDictLayout(KeyTypes::StaticStrings);

#define DT(name, value) {                                           \
    auto const type = KindOf##name;                                 \
    if (type == dt_modulo_persistence(type)) {                      \
      new EmptyOrMonotypeDictLayout(KeyTypes::Ints, type);          \
      new EmptyOrMonotypeDictLayout(KeyTypes::Strings, type);       \
      new EmptyOrMonotypeDictLayout(KeyTypes::StaticStrings, type); \
    }                                                               \
  }
DATATYPES
#undef DT

  new EmptyMonotypeDictLayout();

#define DT(name, value) {                                    \
    auto const type = KindOf##name;                          \
    if (type == dt_modulo_persistence(type)) {               \
      new MonotypeDictLayout(KeyTypes::Ints, type);          \
      new MonotypeDictLayout(KeyTypes::Strings, type);       \
      new MonotypeDictLayout(KeyTypes::StaticStrings, type); \
    }                                                        \
  }
DATATYPES
#undef DT

#define DT(name, value) {                                    \
    auto const type = KindOf##name;                          \
    if (type != dt_modulo_persistence(type)) {               \
      new MonotypeDictLayout(KeyTypes::Ints, type);          \
      new MonotypeDictLayout(KeyTypes::Strings, type);       \
      new MonotypeDictLayout(KeyTypes::StaticStrings, type); \
    }                                                        \
  }
DATATYPES
#undef DT

  auto const init = [&](EmptyMonotypeDict* ad, HeaderKind kind, bool legacy) {
    ad->initHeader_16(kind, StaticValue, legacy ? kLegacyArray : 0);
    ad->setLayoutIndex(getEmptyLayoutIndex());
    ad->m_size = 0;
  };
  init(GetDict(false), HeaderKind::BespokeDict, false);
  init(GetDArray(false), HeaderKind::BespokeDArray, false);
  init(GetDict(true), HeaderKind::BespokeDict, true);
  init(GetDArray(true), HeaderKind::BespokeDArray, true);
}

TopMonotypeDictLayout::TopMonotypeDictLayout(KeyTypes kt)
  : AbstractLayout(
      Index(kt), folly::sformat("MonotypeDict<Empty|{},Top>", show(kt)),
      getTopMonotypeDictParents(kt))
  , m_keyType(kt)
{}

LayoutIndex TopMonotypeDictLayout::Index(KeyTypes kt) {
  auto const t = int8_t(kEmptyDataType) ^ int8_t(kAbstractDataTypeMask);
  return MonotypeDictLayout::Index(kt, static_cast<DataType>(t));
}

EmptyOrMonotypeDictLayout::EmptyOrMonotypeDictLayout(KeyTypes kt, DataType type)
  : AbstractLayout(
      Index(kt, type),
      folly::sformat("MonotypeDict<Empty|{},{}>", show(kt), tname(type)),
      getEmptyOrMonotypeDictParents(kt, type))
  , m_keyType(kt)
  , m_valType(type)
{}

LayoutIndex EmptyOrMonotypeDictLayout::Index(KeyTypes kt, DataType type) {
  assertx(type == dt_modulo_persistence(type));
  auto const t = int8_t(type) ^ int8_t(kAbstractDataTypeMask);
  return MonotypeDictLayout::Index(kt, static_cast<DataType>(t));
}

EmptyMonotypeDictLayout::EmptyMonotypeDictLayout()
  : ConcreteLayout(
      Index(), "MonotypeDict<Empty>", emptyVtable(),
      {getAllEmptyOrMonotypeDictLayouts()})
{}

LayoutIndex EmptyMonotypeDictLayout::Index() {
  return getEmptyLayoutIndex();
}

MonotypeDictLayout::MonotypeDictLayout(KeyTypes kt, DataType type)
  : ConcreteLayout(
      Index(kt, type),
      folly::sformat("MonotypeDict<{},{}>", show(kt), tname(type)),
      getVtableForKeyTypes(kt),
      {getMonotypeParentLayout(kt, type)})
  , m_keyType(kt)
  , m_valType(type)
{}

LayoutIndex MonotypeDictLayout::Index(KeyTypes kt, DataType type) {
  switch (kt) {
    case KeyTypes::Ints:          return getIntLayoutIndex(type);
    case KeyTypes::Strings:       return getStrLayoutIndex(type);
    case KeyTypes::StaticStrings: return getStaticStrLayoutIndex(type);
    default: always_assert(false);
  }
}

bool isMonotypeDictLayout(LayoutIndex index) {
  return kBaseLayoutIndex.raw <= index.raw &&
         index.raw < 2 * kBaseLayoutIndex.raw;
}

BespokeArray* MakeMonotypeDictFromVanilla(
    ArrayData* ad, DataType dt, KeyTypes kt) {
  if (ad->size() > kMaxNumElms) return nullptr;
  switch (kt) {
    case KeyTypes::Ints:
      return MonotypeDict<int64_t>::MakeFromVanilla(ad, dt);
    case KeyTypes::Strings:
      return MonotypeDict<StringData*>::MakeFromVanilla(ad, dt);
    case KeyTypes::StaticStrings:
      return MonotypeDict<StaticStrPtr>::MakeFromVanilla(ad, dt);
    default: always_assert(false);
  }
}

//////////////////////////////////////////////////////////////////////////////

}}
