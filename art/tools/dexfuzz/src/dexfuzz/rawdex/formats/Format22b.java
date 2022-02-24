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

package dexfuzz.rawdex.formats;

import dexfuzz.rawdex.DexRandomAccessFile;
import dexfuzz.rawdex.Instruction;

import java.io.IOException;

public class Format22b extends Format2 implements ContainsVRegs, ContainsConst {
  @Override
  public void writeToFile(DexRandomAccessFile file, Instruction insn) throws IOException {
    file.writeByte((byte) insn.info.value);
    file.writeByte((byte) insn.vregA);
    file.writeByte((byte) insn.vregB);
    file.writeByte((byte) insn.vregC);
    return;
  }

  @Override
  public long getA(byte[] raw) throws IOException {
    return RawInsnHelper.getUnsignedByteFromByte(raw, 1);
  }

  @Override
  public long getB(byte[] raw) throws IOException {
    return RawInsnHelper.getUnsignedByteFromByte(raw, 2);
  }

  @Override
  public long getC(byte[] raw) throws IOException {
    return RawInsnHelper.getSignedByteFromByte(raw, 3);
  }

  @Override
  public long getConst(Instruction insn) {
    return insn.vregC;
  }

  @Override
  public void setConst(Instruction insn, long constant) {
    insn.vregC = constant;
  }

  @Override
  public long getConstRange() {
    return (1 << 8);
  }

  @Override
  public int getVRegCount() {
    return 2;
  }
}
