/*
 * INTEL CONFIDENTIAL
 * Copyright (c) 2015, Intel Corporation All Rights Reserved.
 *
 * The source code contained or described herein and all documents related to the
 * source code ("Material") are owned by Intel Corporation or its suppliers or
 * licensors. Title to the Material remains with Intel Corporation or its suppliers
 * and licensors. The Material contains trade secrets and proprietary and
 * confidential information of Intel or its suppliers and licensors. The Material
 * is protected by worldwide copyright and trade secret laws and treaty provisions.
 * No part of the Material may be used, copied, reproduced, modified, published,
 * uploaded, posted, transmitted, distributed, or disclosed in any way without
 * Intel's prior express written permission.
 *
 * No license under any patent, copyright, trade secret or other intellectual
 * property right is granted to or conferred upon you by disclosure or delivery of
 * the Materials, either expressly, by implication, inducement, estoppel or
 * otherwise. Any license under such intellectual property rights must be express
 * and approved by Intel in writing.
 *
 */
#ifndef ART_COMPILER_OPTIMIZING_FORM_BOTTOM_LOOPS_H_
#define ART_COMPILER_OPTIMIZING_FORM_BOTTOM_LOOPS_H_

#include "driver/dex_compilation_unit.h"
#include "ext_utility.h"
#include "nodes.h"
#include "optimization_x86.h"

#include <unordered_map>
#include <unordered_set>

namespace art {

class HInstructionCloner;

class HFormBottomLoops : public HOptimization_X86 {
 public:
  explicit HFormBottomLoops(HGraph* graph, OptimizingCompilerStats* stats = nullptr)
      : HOptimization_X86(graph, kFormBottomLoopsPassName, stats) { }

  void Run() OVERRIDE;

 private:
  // Stores all required information about blocks of a loop being optimized.
  struct FBLContext {
    FBLContext(HLoopInformation_X86* loop,
               HBasicBlock* pre_header_block,
               HBasicBlock* header_block,
               HBasicBlock* first_block,
               HBasicBlock* back_block,
               HBasicBlock* exit_block) :
      loop_(loop),
      pre_header_block_(pre_header_block),
      header_block_(header_block),
      first_block_(first_block),
      back_block_(back_block),
      exit_block_(exit_block) { }

    HLoopInformation_X86* loop_;
    HBasicBlock* pre_header_block_;
    HBasicBlock* header_block_;
    HBasicBlock* first_block_;
    HBasicBlock* back_block_;
    HBasicBlock* exit_block_;
  };

  static constexpr const char* kFormBottomLoopsPassName = "form_bottom_loops";

  // Mapping from Instruction to its Clone.
  typedef std::unordered_map<HInstruction*, HInstruction*> InstrToInstrMap;

  /**
   * @brief Should this loop be rewritten as a bottom tested loop?
   * @param loop The current loop.
   * @param loop_header The top of the loop.
   * @param exit_block The exit from the loop.
   * @returns 'true' if the loop should be rewritten.
   */
  bool ShouldTransformLoop(HLoopInformation_X86* loop,
                           HBasicBlock* loop_header,
                           HBasicBlock* exit_block);

  /**
   * @brief Is the loop header block safe to rewrite?
   * @param loop_block The top of the loop.
   * @returns 'true' if all instructions in the loop are safe to rewrite.
   */
  bool CheckLoopHeader(HBasicBlock* loop_block);

  /**
   * @brief Rewrite the loop as a bottom tested loop.
   * @param loop The current loop.
   * @param loop_header The top of the loop.
   * @param exit_block The exit from the loop.
   */
  void RewriteLoop(HLoopInformation_X86* loop,
                   HBasicBlock* loop_header,
                   HBasicBlock* exit_block);

  /**
   * @brief Transform CFG to make a loop bottom formed.
   * @param graph The graph.
   * @param context The context describing a loop being transformed.
   * @param first_successor_is_exit Indicates whether the first successor
   *        of header is exit.
   */
  void DoCFGTransformation(HGraph_X86* graph,
                           FBLContext& context,
                           bool first_successor_is_exit);

  /**
   * @brief Creates a new Phi node for replacements in FBL.
   * @details If both inputs and block are specified, creates
   *          a new Phi(in_1, in_2) and adds it to the block.
   *          If in_2 or block are null, it creates a new Phi
   *          with first input in_1. This Phi is to be added
   *          manually in future.
   * @param in_1 The first input of Phi node (not null).
   * @param in_2 The second input of Phi node. May be null.
   * @param block The block to add Phi. May be null <=> in_2 is null.
   * @return Newly-created Phi node.
   */
  HPhi* NewPhi(HInstruction* in_1,
               HInstruction* in_2 = nullptr,
               HBasicBlock* block = nullptr);

  /**
   * @brief Clones instructions and Phis from header to bb.
   * @param context The context describing a loop being transformed.
   * @param cloner The cloner that is used during copying process.
   */
  void CloneInstructions(FBLContext& context,
                         HInstructionCloner& cloner);

  /**
   * @brief Fix uses of old header instruction after FBL transformation.
   * @param insn The instruction to be fixed.
   * @param clone The clone of insn in copied block.
   * @param context The context describing a loop being transformed.
   */
  void FixHeaderInsnUses(HInstruction* insn,
                         HInstruction* clone,
                         FBLContext& context);

  /**
   * @brief Fix uses of old header phi after FBL transformation.
   * @param phi The phi to be fixed.
   * @param context The context describing a loop being transformed.
   */
  void FixHeaderPhisUses(HPhi* phi,
                         FBLContext& context);

  /**
   * @brief Returns a fixup phi' for phi node in header.
   * @details Phi fixup phi' is defined by following rules:
   *          phi' = phi(1),
   *            if phi(1) in old back edge or old preheader;
   *          phi' = Phi(phi(1), clone(phi(1))),
   *            if phi(1) is an insn from old header;
   *          phi' = Phi(phi(1)(0), phi(1)'),
   *            if phi(1) is a Phi from header old.
   * @param phi The phi for which we need phi'.
   * @param context The context describing a loop being transformed.
   */
  HInstruction* GetPhiFixup(HPhi* phi,
                            FBLContext& context);

  /**
   * @brief Returns an interlace fixup Phi(phi(0), phi') for phi in header.
   * @param phi The phi for which we need interlace fixup.
   * @param context The context describing a loop being transformed.
   * @param mapping Mapping that maps phis on already-created Interlace Fixup.
   * @param block The basic block which needs inderlace fixup.
   * @return Phi(phi(0), phi') for phi in block.
   */
  HInstruction* GetPhiInterlaceFixup(HPhi* phi,
                                     FBLContext& context,
                                     InstrToInstrMap& mapping,
                                     HBasicBlock* block);
  /**
   * @brief Returns a header fixup Phi(insn, clone) for instruction in header.
   * @param insn The instruction that needs fixup.
   * @param clone The clone of insn.
   * @param mapping Mapping that maps phis on already-created Interlace Fixup.
   * @param block The basic block which needs header fixup (always new header).
   * @return Phi(insn, clone) for instruction in block.
   */
  HInstruction* GetHeaderFixup(HInstruction* insn,
                               HInstruction* clone,
                               InstrToInstrMap& mapping,
                               HBasicBlock* block);

  /**
   * @brief Replaces an input of user with new input.
   * @param user The instruction where we need replacement.
   * @param new_input The new input to replace with.
   * @param index The input index to replace.
   */
  void ReplaceInput(HInstruction* user,
                    HInstruction* new_input,
                    size_t index) {
    DCHECK(new_input->GetBlock() != nullptr);
    PRINT_PASS_OSTREAM_MESSAGE(this, "Replacing input #" << index
                               << " of " << user
                               << " with " << new_input);
    user->ReplaceInput(new_input, index);
  }

  /**
   * @brief Replaces an input of environment user with new input.
   * @param user The environment where we need replacement.
   * @param new_input The new input to replace with.
   * @param index The input index to replace.
   */
  void ReplaceEnvInput(HEnvironment* user,
                       HInstruction* new_input,
                       size_t index) {
    DCHECK(new_input->GetBlock() != nullptr);
    PRINT_PASS_OSTREAM_MESSAGE(this, "Replacing input #" << index
                               << " of env of " << user->GetHolder()
                               << " with " << new_input);
    user->RemoveAsUserOfInput(index);
    user->SetRawEnvAt(index, new_input);
    new_input->AddEnvUseAt(user, index);
  }

  /**
   * @brief Performs internal structures initialization for a new loop.
   * TODO: Consider moving the maps to context during refactoring.
   */
  void PrepareForNewLoop() {
    phi_fixup_.clear();
    interlace_phi_fixup_inside_.clear();
    interlace_phi_fixup_outside_.clear();
    clones_.clear();
    header_fixup_inside_.clear();
    header_fixup_outside_.clear();
  }

  // Maps phi on phi' that is used to fix up its uses.
  InstrToInstrMap phi_fixup_;

  // Maps phi on Phi(phi(0), phi') that is used to fix up its uses.
  InstrToInstrMap interlace_phi_fixup_inside_;
  InstrToInstrMap interlace_phi_fixup_outside_;

  // Maps insn on Phi(insn, clone).
  InstrToInstrMap header_fixup_inside_;
  InstrToInstrMap header_fixup_outside_;

  // Contains all clones.
  std::unordered_set<HInstruction*> clones_;

  DISALLOW_COPY_AND_ASSIGN(HFormBottomLoops);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_FORM_BOTTOM_LOOPS_H_
