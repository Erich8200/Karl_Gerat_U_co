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
    public static void floatTest() {
      float f = 0;
      float fc = 1f;
      for (int i = 0; i < 2; i++) {
        f -= fc;
        f = -f;
      }

      System.out.println(f);
      System.out.println(f + 0f);
      System.out.println(f - (-0f));
    }

    public static void doubleTest() {
      double d = 0;
      double dc = 1f;
      for (int i = 0; i < 2; i++) {
        d -= dc;
        d = -d;
      }

      System.out.println(d);
      System.out.println(d + 0f);
      System.out.println(d - (-0f));
    }

    public static void main(String[] args) {
        doubleTest();
        floatTest();
    }

}
