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

public class Main {

  // CHECK-START: void Main.test1(boolean, int, int, int, int, int) register (after)
  // CHECK:       name "B0"
  // CHECK-NOT:     ParallelMove
  // CHECK:       name "B1"
  // CHECK-NOT:   end_block
  // CHECK:         If
  // CHECK-NOT:     ParallelMove
  // CHECK:       name "B3"
  // CHECK-NOT:   end_block
  // CHECK:         ArraySet
  // We could check here that there is a parallel move, but it's only valid
  // for some architectures (for example x86), as other architectures may
  // not do move at all.
  // CHECK:       end_block
  // CHECK-NOT:     ParallelMove

  public static void test1(boolean z, int a, int b, int c, int d, int m) {
    int e = live1;
    int f = live2;
    int g = live3;
    if (z) {
    } else {
      // Create enough live instructions to force spilling on x86.
      int h = live4;
      int i = live5;
      array[2] = e + i + h;
      array[3] = f + i + h;
      array[4] = g + i + h;
      array[0] = h;
      array[1] = i + h;

    }
    live1 = e + f + g;
  }

  // CHECK-START: void Main.test2(boolean, int, int, int, int, int) register (after)
  // CHECK:       name "B0"
  // CHECK-NOT:     ParallelMove
  // CHECK:       name "B1"
  // CHECK-NOT:   end_block
  // CHECK:         If
  // CHECK-NOT:     ParallelMove
  // CHECK:       name "B3"
  // CHECK-NOT:   end_block
  // CHECK:         ArraySet
  // We could check here that there is a parallel move, but it's only valid
  // for some architectures (for example x86), as other architectures may
  // not do move at all.
  // CHECK:       end_block
  // CHECK-NOT:     ParallelMove

  public static void test2(boolean z, int a, int b, int c, int d, int m) {
    int e = live1;
    int f = live2;
    int g = live3;
    if (z) {
      if (y) {
        int h = live4;
        int i = live5;
        array[2] = e + i + h;
        array[3] = f + i + h;
        array[4] = g + i + h;
        array[0] = h;
        array[1] = i + h;
      }
    }
    live1 = e + f + g;
  }

  // CHECK-START: void Main.test3(boolean, int, int, int, int, int) register (after)
  // CHECK:       name "B0"
  // CHECK-NOT:     ParallelMove
  // CHECK:       name "B1"
  // CHECK-NOT:   end_block
  // CHECK:         If
  // CHECK-NOT:     ParallelMove
  // CHECK:       name "B6"
  // CHECK-NOT:   end_block
  // CHECK:         ArraySet
  // We could check here that there is a parallel move, but it's only valid
  // for some architectures (for example x86), as other architectures may
  // not do move at all.
  // CHECK:       end_block
  // CHECK-NOT:     ParallelMove

  public static void test3(boolean z, int a, int b, int c, int d, int m) {
    // Same version as test2, but with branches reversed, to ensure
    // whatever linear order is computed, we will get the same results.
    int e = live1;
    int f = live2;
    int g = live3;
    if (z) {
      live1 = e;
    } else {
      if (y) {
        live1 = e;
      } else {
        int h = live4;
        int i = live5;
        array[2] = e + i + h;
        array[3] = f + i + h;
        array[4] = g + i + h;
        array[0] = h;
        array[1] = i + h;
      }
    }
    live1 = e + f + g;
  }

  public static void main(String[] args) {
  }

  static boolean y;
  static int live1;
  static int live2;
  static int live3;
  static int live4;
  static int live5;
  static int[] array;
}
