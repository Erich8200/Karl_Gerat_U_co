/*
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_COMPILER_UTILS_ARENA_BIT_VECTOR_H_
#define ART_COMPILER_UTILS_ARENA_BIT_VECTOR_H_

#include "base/arena_object.h"
#include "base/bit_vector.h"

namespace art {

class ArenaAllocator;
class ScopedArenaAllocator;

// Type of growable bitmap for memory tuning.
enum OatBitMapKind {
  kBitMapMisc = 0,
  kBitMapUse,
  kBitMapDef,
  kBitMapLiveIn,
  kBitMapBMatrix,
  kBitMapDominators,
  kBitMapIDominated,
  kBitMapDomFrontier,
  kBitMapRegisterV,
  kBitMapTempSSARegisterV,
  kBitMapNullCheck,
  kBitMapClInitCheck,
  kBitMapPredecessors,
  kNumBitMapKinds
};

std::ostream& operator<<(std::ostream& os, const OatBitMapKind& kind);

/*
 * A BitVector implementation that uses Arena allocation.
 */
class ArenaBitVector : public BitVector, public ArenaObject<kArenaAllocGrowableBitMap> {
 public:
  ArenaBitVector(ArenaAllocator* arena, uint32_t start_bits, bool expandable,
                 OatBitMapKind kind = kBitMapMisc);
  ArenaBitVector(ScopedArenaAllocator* arena, uint32_t start_bits, bool expandable,
                 OatBitMapKind kind = kBitMapMisc);
  ~ArenaBitVector() {}

 private:
  const OatBitMapKind kind_;      // for memory use tuning. TODO: currently unused.

  DISALLOW_COPY_AND_ASSIGN(ArenaBitVector);
};


}  // namespace art

#endif  // ART_COMPILER_UTILS_ARENA_BIT_VECTOR_H_
