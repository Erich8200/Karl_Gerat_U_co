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

#include "licm.h"
#include "side_effects_analysis.h"

namespace art {

static bool IsPhiOf(HInstruction* instruction, HBasicBlock* block) {
  return instruction->IsPhi() && instruction->GetBlock() == block;
}

/**
 * Returns whether `instruction` has all its inputs and environment defined
 * before the loop it is in.
 */
static bool InputsAreDefinedBeforeLoop(HInstruction* instruction) {
  DCHECK(instruction->IsInLoop());
  HLoopInformation* info = instruction->GetBlock()->GetLoopInformation();
  for (HInputIterator it(instruction); !it.Done(); it.Advance()) {
    HLoopInformation* input_loop = it.Current()->GetBlock()->GetLoopInformation();
    // We only need to check whether the input is defined in the loop. If it is not
    // it is defined before the loop.
    if (input_loop != nullptr && input_loop->IsIn(*info)) {
      return false;
    }
  }

  for (HEnvironment* environment = instruction->GetEnvironment();
       environment != nullptr;
       environment = environment->GetParent()) {
    for (size_t i = 0, e = environment->Size(); i < e; ++i) {
      HInstruction* input = environment->GetInstructionAt(i);
      if (input != nullptr) {
        HLoopInformation* input_loop = input->GetBlock()->GetLoopInformation();
        if (input_loop != nullptr && input_loop->IsIn(*info)) {
          // We can move an instruction that takes a loop header phi in the environment:
          // we will just replace that phi with its first input later in `UpdateLoopPhisIn`.
          bool is_loop_header_phi = IsPhiOf(input, info->GetHeader());
          if (!is_loop_header_phi) {
            return false;
          }
        }
      }
    }
  }
  return true;
}

/**
 * If `environment` has a loop header phi, we replace it with its first input.
 */
static void UpdateLoopPhisIn(HEnvironment* environment, HLoopInformation* info) {
  for (; environment != nullptr; environment = environment->GetParent()) {
    for (size_t i = 0, e = environment->Size(); i < e; ++i) {
      HInstruction* input = environment->GetInstructionAt(i);
      if (input != nullptr && IsPhiOf(input, info->GetHeader())) {
        environment->RemoveAsUserOfInput(i);
        HInstruction* incoming = input->InputAt(0);
        environment->SetRawEnvAt(i, incoming);
        incoming->AddEnvUseAt(environment, i);
      }
    }
  }
}

void LICM::Run() {
  DCHECK(side_effects_.HasRun());
  // Only used during debug.
  ArenaBitVector visited(graph_->GetArena(), graph_->GetBlocks().Size(), false);

  // Post order visit to visit inner loops before outer loops.
  for (HPostOrderIterator it(*graph_); !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();
    if (!block->IsLoopHeader()) {
      // Only visit the loop when we reach the header.
      continue;
    }

    HLoopInformation* loop_info = block->GetLoopInformation();
    SideEffects loop_effects = side_effects_.GetLoopEffects(block);
    HBasicBlock* pre_header = loop_info->GetPreHeader();

    for (HBlocksInLoopIterator it_loop(*loop_info); !it_loop.Done(); it_loop.Advance()) {
      HBasicBlock* inner = it_loop.Current();
      DCHECK(inner->IsInLoop());
      if (inner->GetLoopInformation() != loop_info) {
        // Thanks to post order visit, inner loops were already visited.
        DCHECK(visited.IsBitSet(inner->GetBlockId()));
        continue;
      }
      visited.SetBit(inner->GetBlockId());

      // We can move an instruction that can throw only if it is the first
      // throwing instruction in the loop. Note that the first potentially
      // throwing instruction encountered that is not hoisted stops this
      // optimization. Non-throwing instruction can still be hoisted.
      bool found_first_non_hoisted_throwing_instruction_in_loop = !inner->IsLoopHeader();
      for (HInstructionIterator inst_it(inner->GetInstructions());
           !inst_it.Done();
           inst_it.Advance()) {
        HInstruction* instruction = inst_it.Current();
        if (instruction->CanBeMoved()
            && (!instruction->CanThrow() || !found_first_non_hoisted_throwing_instruction_in_loop)
            && !instruction->GetSideEffects().DependsOn(loop_effects)
            && InputsAreDefinedBeforeLoop(instruction)) {
          // We need to update the environment if the instruction has a loop header
          // phi in it.
          if (instruction->NeedsEnvironment()) {
            UpdateLoopPhisIn(instruction->GetEnvironment(), loop_info);
          }
          instruction->MoveBefore(pre_header->GetLastInstruction());
        } else if (instruction->CanThrow()) {
          // If `instruction` can throw, we cannot move further instructions
          // that can throw as well.
          found_first_non_hoisted_throwing_instruction_in_loop = true;
        }
      }
    }
  }
}

}  // namespace art
