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
 */

#include "aur.h"
#include "base/dumpable.h"
#include "base/timing_logger.h"
#include "code_generator.h"
#include "constant_calculation_sinking.h"
#include "ext_utility.h"
#include "driver/compiler_driver.h"
#include "form_bottom_loops.h"
#include "find_ivs.h"
#include "generate_selects.h"
#include "graph_visualizer.h"
#include "gvn_after_fbl.h"
#include "loadhoist_storesink.h"
#include "loop_formation.h"
#include "loop_full_unrolling.h"
#ifndef SOFIA
#include "non_temporal_move.h"
#endif
#include "optimization.h"
#include "optimizing_compiler_stats.h"
#include "pass_framework.h"
#include "peeling.h"
#include "pure_invokes_analysis.h"
#include "remove_suspend.h"
#include "remove_unused_loops.h"
#include "scoped_thread_state_change.h"
#include "thread.h"
#include "trivial_loop_evaluator.h"
#include "value_propagation_through_heap.h"

namespace art {

// Enumeration defining possible commands to be applied to each pass.
enum PassInstrumentation {
  kPassInsertBefore,
  kPassInsertAfter,
  kPassReplace,
};

/**
 * @brief Used to provide directive for custom pass placement.
 */
struct HCustomPassPlacement {
  const char* pass_to_place;      /**!< The pass that has a custom location. */
  const char* pass_relative_to;   /**!< The pass for which the new pass is relative to. */
  PassInstrumentation directive;  /**!< How to do the pass insert (before, after, etc). */
};

/**
 * @brief Static array holding information about custom placements.
 */
static HCustomPassPlacement kPassCustomPlacement[] = {
  { "loop_peeling", "select_generator", kPassInsertBefore },
  { "loop_formation_before_peeling", "loop_peeling", kPassInsertBefore },
  // FIXME: this pass is disabled and should be eliminated
  // completely because Google has implemented a similar
  // optimization, called "select_generator".
  // { "generate_selects", "boolean_simplifier", kPassInsertAfter },
  { "form_bottom_loops", "load_store_elimination", kPassInsertAfter },
  { "loop_formation_before_bottom_loops", "form_bottom_loops", kPassInsertBefore },
  { "GVN_after_form_bottom_loops", "form_bottom_loops", kPassInsertAfter },
  { "value_propagation_through_heap", "GVN_after_form_bottom_loops", kPassInsertAfter },
  { "loop_formation", "value_propagation_through_heap", kPassInsertAfter },
  { "find_ivs", "loop_formation", kPassInsertAfter },
  { "trivial_loop_evaluator", "find_ivs", kPassInsertAfter },
  { "non_temporal_move", "trivial_loop_evaluator", kPassInsertAfter },
  { "constant_calculation_sinking", "non_temporal_move", kPassInsertAfter },
  { "remove_loop_suspend_checks", "constant_calculation_sinking", kPassInsertAfter },
  { "pure_invokes_analysis", "remove_loop_suspend_checks", kPassInsertAfter },
  { "loadhoist_storesink", "pure_invokes_analysis", kPassInsertAfter },
  { "remove_unused_loops", "loadhoist_storesink", kPassInsertAfter },
  { "loop_full_unrolling", "remove_unused_loops", kPassInsertAfter },
  { "load_store_elimination", "value_propagation_through_heap", kPassInsertBefore },
  { "aur", "remove_unused_loops", kPassInsertBefore },
};

/**
 * @brief Static array holding names of passes that need removed.
 * @details This is done in cases where common code pass ordering and
 * existing passes are not appropriate or compatible with extension.
 */
static const char* kPassRemoval[] = {
  nullptr,
};

static void AddX86Optimization(HOptimization* optimization,
                               ArenaVector<HOptimization*>& list,
                               ArenaSafeMap<const char*, HCustomPassPlacement*> &placements) {
  ArenaSafeMap<const char*, HCustomPassPlacement*>::iterator iter = placements.find(optimization->GetPassName());

  if (iter == placements.end()) {
    return;
  }

  HCustomPassPlacement* placement = iter->second;

  // Find the right pass to change now.
  size_t len = list.size();
  size_t idx;
  for (idx = 0; idx < len; idx++) {
    if (strcmp(list[idx]->GetPassName(), placement->pass_relative_to) == 0) {
      switch (placement->directive) {
        case kPassReplace:
          list[idx] = optimization;
          break;
        case kPassInsertBefore:
        case kPassInsertAfter:
          // Add an empty element.
          list.push_back(nullptr);

          // Find the start, is it idx or idx + 1?
          size_t start = idx;

          if (placement->directive == kPassInsertAfter) {
            start++;
          }

          // Push elements backwards.
          DCHECK_NE(len, list.size());
          for (size_t idx2 = len; idx2 >= start; idx2--) {
            list[idx2] = list[idx2 - 1];
          }

          // Place the new element.
          list[start] = optimization;
          break;
      }
      // Done here.
      break;
    }
  }
  // It must be the case that the custom placement was found.
  DCHECK_NE(len, idx) << "couldn't insert " << optimization->GetPassName() << " relative to " << placement->pass_relative_to;
}

static void FillCustomPlacement(ArenaSafeMap<const char*, HCustomPassPlacement*>& placements) {
  size_t len = arraysize(kPassCustomPlacement);

  for (size_t i = 0; i < len; i++) {
    placements.Overwrite(kPassCustomPlacement[i].pass_to_place, kPassCustomPlacement + i);
  }
}

static void FillOptimizationList(HGraph* graph,
                                 ArenaVector<HOptimization*>& list,
                                 HOptimization_X86* optimizations_x86[],
                                 size_t opts_x86_length) {
  // Get the custom placements for our passes.
  ArenaSafeMap<const char*, HCustomPassPlacement*> custom_placement(
      std::less<const char*>(),
      graph->GetArena()->Adapter(kArenaAllocMisc));
  FillCustomPlacement(custom_placement);

  for (size_t i = 0; i < opts_x86_length; i++) {
    HOptimization_X86* opt = optimizations_x86[i];
    if (opt != nullptr) {
      AddX86Optimization(opt, list, custom_placement);
    }
  }
}

/**
 * @brief Remove the passes in the optimization list.
 * @param opts the optimization vector.
 * @param driver the compilation driver.
 */
static void RemoveOptimizations(ArenaVector<HOptimization*>& opts,
                                CompilerDriver* driver) {
  std::unordered_set<std::string> disabled_passes;

  SplitStringIntoSet(driver->GetCompilerOptions().
                       GetPassManagerOptions()->GetDisablePassList(),
                     ',',
                     disabled_passes);

  // Add elements from kPassRemoval.
  for (size_t i = 0, len = arraysize(kPassRemoval); i < len; i++) {
    if (kPassRemoval[i] != nullptr) {
      disabled_passes.insert(std::string(kPassRemoval[i]));
    }
  }

  // If there are no disabled passes, bail.
  if (disabled_passes.empty()) {
    return;
  }

  size_t opts_len = opts.size();

  // We replace the opts with nullptr if we find a match.
  //   This is cheaper than rearranging the vectors.
  for (size_t opts_idx = 0; opts_idx < opts_len; opts_idx++) {
    HOptimization* opt = opts[opts_idx];
    if (disabled_passes.find(opt->GetPassName()) != disabled_passes.end()) {
      opts[opts_idx] = nullptr;
    }
  }
}

void PrintPasses(ArenaVector<HOptimization*>& opts) {
  size_t opts_len = opts.size();

  // We replace the opts with nullptr if we find a match.
  //   This is cheaper than rearranging the vectors.
  LOG(INFO) << "Pass List:";
  if (opts_len == 0) {
    LOG(INFO) << "\t<Empty>";
  }

  for (size_t opts_idx = 0; opts_idx < opts_len; opts_idx++) {
    HOptimization* opt = opts[opts_idx];
    if (opt != nullptr) {
      LOG(INFO) << "\t- " << opt->GetPassName();
    }
  }
}

bool PrintPassesOnlyOnce(ArenaVector<HOptimization*>& opts,
                         CompilerDriver* driver) {
  bool need_print = driver->GetCompilerOptions().
                            GetPassManagerOptions()->GetPrintPassNames();

  if (!need_print) {
    return false;
  }

  // Flags that we have already printed the pass name list.
  static volatile bool pass_names_already_printed_ = false;

  // Have we already printed the names?
  if (!pass_names_already_printed_) {
    // Double-check it under the lock.
    ScopedObjectAccess soa(Thread::Current());
    if (!pass_names_already_printed_) {
      pass_names_already_printed_ = true;
    } else {
      need_print = false;
    }
  } else {
    need_print = false;
  }

  if (!need_print) {
    return false;
  }

  PrintPasses(opts);
  return true;
}

/**
 * @brief Sets verbosity for passes.
 * @param optimizations the optimization array.
 * @param opts_len the length of optimizations array.
 * @param driver the compilation driver.
 */
void FillVerbose(HOptimization_X86* optimizations[],
                 size_t opts_len,
                 CompilerDriver* driver) {
  std::unordered_set<std::string> print_passes;
  const bool print_all_passes = driver->GetCompilerOptions().
                                GetPassManagerOptions()->GetPrintAllPasses();
  if (!print_all_passes) {
    // If we don't print all passes, we need to check the list.
    SplitStringIntoSet(driver->GetCompilerOptions().
                         GetPassManagerOptions()->GetPrintPassList(),
                       ',',
                       print_passes);

    // Are there any passes to print?
    if (print_passes.empty()) {
      return;
    }
  }

  for (size_t opts_idx = 0; opts_idx < opts_len; opts_idx++) {
    HOptimization* opt = optimizations[opts_idx];
    if (opt != nullptr) {
      if (print_all_passes ||
          print_passes.find(opt->GetPassName()) != print_passes.end()) {
        optimizations[opts_idx]->SetVerbose(true);
      }
    }
  }
}

void RunOptimizationsX86(HGraph* graph,
                         CompilerDriver* driver,
                         OptimizingCompilerStats* stats,
                         ArenaVector<HOptimization*>& opt_list,
                         PassObserver* pass_observer) {
  // Create the array for the opts.
  ArenaAllocator* arena = graph->GetArena();
  HLoopFormation* loop_formation = new (arena) HLoopFormation(graph);
  HFindInductionVariables* find_ivs = new (arena) HFindInductionVariables(graph, stats);
  HRemoveLoopSuspendChecks* remove_suspends = new (arena) HRemoveLoopSuspendChecks(graph, driver, stats);
  HRemoveUnusedLoops* remove_unused_loops = new (arena) HRemoveUnusedLoops(graph, stats);
  TrivialLoopEvaluator* tle = new (arena) TrivialLoopEvaluator(graph, driver, stats);
  HConstantCalculationSinking* ccs = new (arena) HConstantCalculationSinking(graph, stats);
#ifndef SOFIA
  HNonTemporalMove* non_temporal_move = new (arena) HNonTemporalMove(graph, driver, stats);
#endif
  LoadHoistStoreSink* lhss = new (arena) LoadHoistStoreSink(graph, stats);
  HValuePropagationThroughHeap* value_propagation_through_heap =
      new (arena) HValuePropagationThroughHeap(graph, driver, stats);
  HLoopFormation* formation_before_peeling =
      new (arena) HLoopFormation(graph, "loop_formation_before_peeling");
  HLoopPeeling* peeling = new (arena) HLoopPeeling(graph, stats);
  HPureInvokesAnalysis* pure_invokes_analysis = new (arena) HPureInvokesAnalysis(graph, stats);
  HLoopFormation* formation_before_bottom_loops =
      new (arena) HLoopFormation(graph, "loop_formation_before_bottom_loops");
  HFormBottomLoops* form_bottom_loops = new (arena) HFormBottomLoops(graph, stats);
  GVNAfterFormBottomLoops* gvn_after_fbl = new (arena) GVNAfterFormBottomLoops(graph);
  // HGenerateSelects* generate_selects = new (arena) HGenerateSelects(graph, stats);
  HLoopFullUnrolling* loop_full_unrolling = new (arena) HLoopFullUnrolling(graph, driver, stats);
  HAggressiveUseRemoverPass* aur = new (arena) HAggressiveUseRemoverPass(graph, driver, stats);

  HOptimization_X86* opt_array[] = {
    peeling,
    formation_before_peeling,
    // &generate_selects
    form_bottom_loops,
    formation_before_bottom_loops,
    gvn_after_fbl,
    value_propagation_through_heap,
    loop_formation,
    find_ivs,
    tle,
#ifndef SOFIA
    non_temporal_move,
#endif
    ccs,
    remove_suspends,
    pure_invokes_analysis,
    lhss,
    remove_unused_loops,
    loop_full_unrolling,
    aur,
  };

  // Fill verbose flags where we need it.
  FillVerbose(opt_array, arraysize(opt_array),
              driver);

  // Create the vector for the optimizations.
  FillOptimizationList(graph, opt_list, opt_array, arraysize(opt_array));

  // Finish by removing the ones we do not want.
  RemoveOptimizations(opt_list, driver);

  // Print the pass list, if needed.
  PrintPassesOnlyOnce(opt_list, driver);

  // Now execute the optimizations.
  size_t phase_id = 0;
  for (auto optimization : opt_list) {
    if (optimization != nullptr) {
      const char* name = optimization->GetPassName();
      // if debug option --stop-optimizing-after is passed
      // then check whether we need to stop optimization.
      if (driver->GetCompilerOptions().IsConditionalCompilation()) {
        if (driver->GetCompilerOptions().GetStopOptimizingAfter() < phase_id ||
            driver->GetCompilerOptions().GetStopOptimizingAfter() ==
            std::numeric_limits<uint32_t>::max()) {
          break;
        }
        VLOG(compiler) << "Applying " << name << ", phase_id = " << phase_id;
      }
      RunOptWithPassScope scope(optimization, pass_observer);
      scope.Run();
      phase_id++;
    }
  }
}
}  // namespace art