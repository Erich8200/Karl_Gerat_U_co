/*
 * Copyright (C) 2015 The Android Open Source Project
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

#ifndef ART_RUNTIME_BASE_ITERATION_RANGE_H_
#define ART_RUNTIME_BASE_ITERATION_RANGE_H_

#include <iterator>

namespace art {

// Helper class that acts as a container for range-based loops, given an iteration
// range [first, last) defined by two iterators.
template <typename Iter>
class IterationRange {
 public:
  typedef Iter iterator;
  typedef typename std::iterator_traits<Iter>::difference_type difference_type;
  typedef typename std::iterator_traits<Iter>::value_type value_type;
  typedef typename std::iterator_traits<Iter>::pointer pointer;
  typedef typename std::iterator_traits<Iter>::reference reference;

  IterationRange(iterator first, iterator last) : first_(first), last_(last) { }

  iterator begin() const { return first_; }
  iterator end() const { return last_; }
  iterator cbegin() const { return first_; }
  iterator cend() const { return last_; }

 private:
  const iterator first_;
  const iterator last_;
};

template <typename Iter>
static inline IterationRange<Iter> MakeIterationRange(const Iter& begin_it, const Iter& end_it) {
  return IterationRange<Iter>(begin_it, end_it);
}

}  // namespace art

#endif  // ART_RUNTIME_BASE_ITERATION_RANGE_H_
