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

import java.io.File;
import java.io.IOException;
import java.lang.reflect.Method;

/**
 * Controls deoptimization using dalvik.system.VMDebug class.
 */
public class DeoptimizationController {
  private static File createTempFile() throws Exception {
    try {
      return  File.createTempFile("test", ".trace");
    } catch (IOException e) {
      System.setProperty("java.io.tmpdir", "/data/local/tmp");
      try {
        return File.createTempFile("test", ".trace");
      } catch (IOException e2) {
        System.setProperty("java.io.tmpdir", "/sdcard");
        return File.createTempFile("test", ".trace");
      }
    }
  }

  public static void startDeoptimization() {
    try {
      File tempFile = createTempFile();
      tempFile.deleteOnExit();
      String tempFileName = tempFile.getPath();

      VMDebug.startMethodTracing(tempFileName, 0, 0, false, 1000);
      if (VMDebug.getMethodTracingMode() == 0) {
        throw new IllegalStateException("Not tracing.");
      }
    } catch (Exception exc) {
      exc.printStackTrace(System.err);
    }
  }

  public static void stopDeoptimization() {
    try {
      VMDebug.stopMethodTracing();
      if (VMDebug.getMethodTracingMode() != 0) {
        throw new IllegalStateException("Still tracing.");
      }
    } catch (Exception exc) {
      exc.printStackTrace(System.err);
    }
  }

  private static class VMDebug {
    private static final Method startMethodTracingMethod;
    private static final Method stopMethodTracingMethod;
    private static final Method getMethodTracingModeMethod;

    static {
      try {
        Class<?> c = Class.forName("dalvik.system.VMDebug");
        startMethodTracingMethod = c.getDeclaredMethod("startMethodTracing", String.class,
            Integer.TYPE, Integer.TYPE, Boolean.TYPE, Integer.TYPE);
        stopMethodTracingMethod = c.getDeclaredMethod("stopMethodTracing");
        getMethodTracingModeMethod = c.getDeclaredMethod("getMethodTracingMode");
      } catch (Exception e) {
        throw new RuntimeException(e);
      }
    }

    public static void startMethodTracing(String filename, int bufferSize, int flags,
        boolean samplingEnabled, int intervalUs) throws Exception {
      startMethodTracingMethod.invoke(null, filename, bufferSize, flags, samplingEnabled,
          intervalUs);
    }
    public static void stopMethodTracing() throws Exception {
      stopMethodTracingMethod.invoke(null);
    }
    public static int getMethodTracingMode() throws Exception {
      return (int) getMethodTracingModeMethod.invoke(null);
    }
  }
}
