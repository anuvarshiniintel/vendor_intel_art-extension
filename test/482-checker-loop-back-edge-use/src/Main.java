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

  /// CHECK-START: void Main.loop1(boolean) liveness (after)
  /// CHECK:         ParameterValue  liveness:2  ranges:{[2,22)} uses:[17,22]
  /// CHECK:         Goto            liveness:20
  public static void loop1(boolean incoming) {
    while (incoming) {}
  }

  /// CHECK-START: void Main.loop2(boolean) liveness (after)
  /// CHECK:         ParameterValue  liveness:4  ranges:{[4,44)} uses:[35,40,44]
  /// CHECK:         Goto            liveness:38
  /// CHECK:         Goto            liveness:42
  public static void loop2(boolean incoming) {
    while (true) {
      System.out.println("foo");
      while (incoming) {}
    }
  }

  /// CHECK-START: void Main.loop3(boolean) liveness (after)
  /// CHECK:         ParameterValue  liveness:4  ranges:{[4,60)} uses:[56,60]
  /// CHECK:         Goto            liveness:58

  // CHECK-START: void Main.loop3(boolean) liveness (after)
  // CHECK-NOT:     Goto liveness:50
  public static void loop3(boolean incoming) {
    // 'incoming' only needs a use at the outer loop's back edge.
    while (System.currentTimeMillis() != 42) {
      while (Runtime.getRuntime() != null) {}
      System.out.println(incoming);
    }
  }

  // CHECK-START: void Main.loop4(boolean) liveness (after)
  // CHECK:         ParameterValue  liveness:4  ranges:{[4,22)} uses:[22]

  // CHECK-START: void Main.loop4(boolean) liveness (after)
  // CHECK-NOT:     Goto            liveness:18
  public static void loop4(boolean incoming) {
    // 'incoming' has no loop use, so should not have back edge uses.
    System.out.println(incoming);
    while (System.currentTimeMillis() != 42) {
      while (Runtime.getRuntime() != null) {}
    }
  }

  /// CHECK-START: void Main.loop5(boolean) liveness (after)
  /// CHECK:         ParameterValue  liveness:4  ranges:{[4,54)} uses:[37,46,50,54]
  /// CHECK:         Goto            liveness:48
  /// CHECK:         Goto            liveness:52
  public static void loop5(boolean incoming) {
    // 'incoming' must have a use at both back edges.
    while (Runtime.getRuntime() != null) {
      while (incoming) {
        System.out.println(incoming);
      }
    }
  }

  /// CHECK-START: void Main.loop6(boolean) liveness (after)
  /// CHECK:         ParameterValue  liveness:4  ranges:{[4,50)} uses:[26,50]
  /// CHECK:         Goto            liveness:48

  /// CHECK-START: void Main.loop6(boolean) liveness (after)
  /// CHECK-NOT:     Goto            liveness:24
  public static void loop6(boolean incoming) {
    // 'incoming' must have a use only at the first loop's back edge.
    while (true) {
      System.out.println(incoming);
      while (Runtime.getRuntime() != null) {}
    }
  }

  /// CHECK-START: void Main.loop7(boolean) liveness (after)
  /// CHECK:         ParameterValue  liveness:4  ranges:{[4,54)} uses:[36,45,50,54]
  /// CHECK:         Goto            liveness:48
  /// CHECK:         Goto            liveness:52
  public static void loop7(boolean incoming) {
    // 'incoming' must have a use at both back edges.
    while (Runtime.getRuntime() != null) {
      System.out.println(incoming);
      while (incoming) {}
    }
  }

  /// CHECK-START: void Main.loop8() liveness (after)
  /// CHECK:         StaticFieldGet  liveness:14 ranges:{[14,48)} uses:[39,44,48]
  /// CHECK:         Goto            liveness:42
  /// CHECK:         Goto            liveness:46
  public static void loop8() {
    // 'incoming' must have a use at both back edges.
    boolean incoming = field;
    while (Runtime.getRuntime() != null) {
      while (incoming) {}
    }
  }

  /// CHECK-START: void Main.loop9() liveness (after)
  /// CHECK:         StaticFieldGet  liveness:26 ranges:{[26,40)} uses:[35,40]
  /// CHECK:         Goto            liveness:42
  public static void loop9() {
    while (Runtime.getRuntime() != null) {
      // 'incoming' must only have a use in the inner loop.
      boolean incoming = field;
      while (incoming) {}
    }
  }

  public static void main(String[] args) {
  }

  static boolean field;
}