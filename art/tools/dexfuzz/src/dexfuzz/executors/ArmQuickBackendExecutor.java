/*
 * Copyright (C) 2014 The Android Open Source Project
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

package dexfuzz.executors;

import dexfuzz.listeners.BaseListener;

public class ArmQuickBackendExecutor extends Executor {

  public ArmQuickBackendExecutor(BaseListener listener, Device device) {
    super("ARM Quick Backend", 5, listener, Architecture.ARM, device, true);
  }

  @Override
  public void execute(String programName) {
    StringBuilder commandBuilder = new StringBuilder();
    commandBuilder.append("dalvikvm32 -Xcompiler-option --compiler-backend=Quick ");
    if (device.noBootImageAvailable()) {
      commandBuilder.append("-Ximage:/data/art-test/core.art -Xnorelocate ");
    }
    commandBuilder.append("-cp ").append(testLocation).append("/").append(programName).append(" ");
    commandBuilder.append(executeClass);
    executionResult = executeCommandWithTimeout(commandBuilder.toString(), true);
  }
}
