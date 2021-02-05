/**
 * Copyright 2020-2021 Huawei Technologies Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "poly/tiling_strategy_manager.h"

#include <iostream>

namespace akg {
namespace ir {
namespace poly {
std::unordered_map<TileAxis *, std::vector<AttrInfo>> TilingStrategy::GetInterestedInfo(const std::string &attr_key,
                                                                                        bool match_whole_word) {
  std::unordered_map<TileAxis *, std::vector<AttrInfo>> result;
  std::vector<TileAxis *> axes =
    match_whole_word ? analyzer_->GetAxesOfAttr(attr_key) : analyzer_->GetAxesContainsAttr(attr_key);
  for (auto a : axes) {
    std::vector<AttrInfo> info;
    for (const auto &attr : a->attrs) {
      if ((match_whole_word && attr.attr_key != attr_key) ||
          (!match_whole_word && attr.attr_key.find(attr_key) == std::string::npos)) {
        continue;
      }
      info.emplace_back(attr);
    }
    result[a] = info;
  }
  return result;
}

void CustomTilingStrategy::AddConstraint() {
  auto interested_info = GetInterestedInfo(interested_attr_key, false);
  for (auto it : interested_info) {
    TileAxis *axis = it.first;
    for (auto attr : it.second) {
      std::vector<std::string> modes = akg::common::Split(attr.attr_key, ":");
      CHECK_EQ(modes.size(), 2U);
      std::string constraint_str = attr.attr_value;
      std::string related_buf;
      if (constraint_str.find("->") != std::string::npos) {
        std::vector<std::string> res = akg::common::Split(constraint_str, "->");
        related_buf = res[0];
        constraint_str = res[1];
      }
      std::vector<std::string> constraints = akg::common::Split(constraint_str, "_");
      CHECK_GE(constraints.size(), 1U);
      std::vector<std::string> level = akg::common::Split(constraints[0], ":");
      CHECK(level.size() == 2U && level[0] == "LEVEL");
      CHECK(level[1] == "C1" || level[1] == "C0");
      TileLevel lv = level[1] == "C1" ? LEVEC1 : LEVEC0;
      constraints.erase(constraints.begin());
      for (const auto &con : constraints) {
        std::vector<std::string> items = akg::common::Split(con, ":");
        CHECK_EQ(items.size(), 2U);
        CHECK_NE(items[0], "");
        CHECK_NE(items[1], "");
        if (items[0] == "MIN") {
          if (items[1] == "MIN") {
            if (lv == LEVEC1) {
              axis->l1_constraints.tile_extent_ = axis->l1_constraints.tile_min_;
            } else if (lv == LEVEC0) {
              axis->l0_constraints.tile_extent_ = axis->l0_constraints.tile_min_;
            }
          } else {
            if (lv == LEVEC1) {
              axis->l1_constraints.tile_min_ = CastToExpr(items[1]);
            } else if (lv == LEVEC0) {
              axis->l0_constraints.tile_min_ = CastToExpr(items[1]);
            }
          }
        } else if (items[0] == "FACTOR") {
          axis->TileRestrainToSingleValue(CastToExpr(items[1]), lv);
        } else if (items[0] == "CANDIDATE") {
          if (lv == LEVEC1)
            axis->InsertC1CandFactor(CastToExpr(items[1]));
          else
            axis->InsertC0CandFactor(CastToExpr(items[1]));
        } else if (items[0] == "MAX") {
          if (items[1] == "FULL") {
            axis->TileRestrainEntire(lv);
          } else {
            if (lv == LEVEC1) {
              axis->l1_constraints.tile_extent_ = CastToExpr(items[1]);
            } else if (lv == LEVEC0) {
              axis->l0_constraints.tile_extent_ = CastToExpr(items[1]);
            }
          }
        } else if (items[0] == "MOD") {
          axis->TileRestrainMod(CastToExpr(items[1]), lv);
        } else if (items[0] == "FORBIDISO") {
          axis->forbid_iso = true;
        } else if (items[0] == "PRIORITY") {
          axis->priority = static_cast<int>(std::strtol(items[1].c_str(), nullptr, 10));
        } else if (items[0] == "EXPANSION") {
          std::string info = related_buf + "->" + items[1];
          analyzer_->RootAxis()->MarkWithAttr(AttrInfo{"EXPANSION", info});
        } else if (items[0] == "AXISINFO") {
          axis->axis_type_ = items[1];
        }
      }
    }
  }
}

void ConflictTreeRangeStrategy::AddConstraint() {
  auto ApplyConflictStrategy = [this](TileAxis *axis) {
    int64_t const_extent = axis->GetConstExtent();
    if (const_extent == -1) {
      return;
    }
    // When axis has conflict ranges, it is likely a padded axis;
    // When padded axis has "MOD" attr, it is likely a transformed axis;
    // It is not safe to apply min tile(1) to padded-and-transformed axis
    // as poly may generate wrong index.
    if (!axis->HasAttr("MOD")) {
      axis->InsertC1CandFactor(CastIntToExpr(MIN_TILE));
    }
    if (axis->HasAttr("MODSHIFT")) {
      const_extent = (const_extent - axis->range_min);
      axis->RemoveAttr("MODSHIFT");
    }
    if (axis->HasAttr("SHIFT")) {
      axis->RemoveAttr("SHIFT");
    }
    axis->range_min = MIN_TILE;
    axis->InsertC1CandFactor(CastInt64ToExpr(const_extent));
    axis->l1_constraints.tile_min_ = CastIntToExpr(MIN_TILE);
    axis->l1_constraints.tile_extent_ = CastInt64ToExpr(const_extent);
    axis->l0_constraints.tile_min_ = CastIntToExpr(MIN_TILE);
    axis->l0_constraints.tile_extent_ = CastInt64ToExpr(const_extent);
  };
  auto CheckRange = [this, &ApplyConflictStrategy](TileAxis *axis) {
    std::unordered_set<int64_t> offset;
    std::unordered_set<int64_t> extent;
    int64_t min_off = -1;
    for (const auto &r : axis->tree_ranges) {
      const auto int_range = r.second.as<IntImm>();
      if (int_range == nullptr) {
        return;
      }
      if (r.first != 0) {
        offset.insert(r.first);
        if (min_off == -1) {
          min_off = r.first;
        } else if (r.first < min_off) {
          min_off = r.first;
        }
      }
      if (int_range->value != 0) {
        extent.insert(int_range->value - r.first);
      }
    }
    for (auto o : offset) {
      if (o % min_off != 0) {
        ApplyConflictStrategy(axis);
        return;
      }
    }
    if (extent.size() >= 2U) {
      ApplyConflictStrategy(axis);
    }
  };
  analyzer_->ForEachAxisTopDown(CheckRange);
}

void ModStrategy::AddConstraint() {
  auto interested_info = GetInterestedInfo(interested_attr_key);
  for (auto it : interested_info) {
    TileAxis *axis = it.first;
    for (const auto &attr : it.second) {
      CHECK_NE(attr.attr_value, "");
      auto mod_value = static_cast<int>(std::strtol(attr.attr_value.c_str(), nullptr, 10));
      axis->TileRestrainMod(mod_value, LEVEC1);
    }
  }
}

void CastStrategy::AddConstraint() {
  auto interested_info = GetInterestedInfo(interested_attr_key);
  for (auto it : interested_info) {
    TileAxis *axis = it.first;
    for (const auto &attr : it.second) {
      std::vector<std::string> src_dst = akg::common::Split(attr.attr_value, "->");
      CHECK_EQ(src_dst.size(), 2U);

      std::vector<std::string> src_list = akg::common::Split(src_dst[0], ",");
      CHECK_GE(src_list.size(), 1U);
      for (const auto &src : src_list) {
        std::vector<std::string> src_info = akg::common::Split(src, ":");
        CHECK_EQ(src_info.size(), 2U);
        CHECK_NE(src_info[1], "");
        axis->data_size[src_info[0]] = static_cast<int>(std::strtol(src_info[1].c_str(), nullptr, 10));
      }

      std::vector<std::string> dst_info = akg::common::Split(src_dst[1], ":");
      CHECK_EQ(dst_info.size(), 2U);
      CHECK_NE(dst_info[1], "");
      axis->data_size[dst_info[0]] = static_cast<int>(std::strtol(dst_info[1].c_str(), nullptr, 10));
    }
  }
}

void ReduceStrategy::AddConstraint() {
  for (auto axis : analyzer_->GetAxesOfAttr("REDUCE_DST_LAST")) {
    int64_t block_size = GetMaxAlignBytes(axis->data_size);
    int64_t const_extent = axis->GetConstExtent();
    if (const_extent == -1) {
      continue;
    }
    int64_t align_elem = air::ir::gcd(block_size, const_extent);
    if (align_elem == block_size) {
      axis->l1_constraints.tile_min_ = align_elem;
    } else {
      axis->priority += 1;
      axis->forbid_iso = true;
    }
  }
  for (auto axis : analyzer_->GetAxesOfAttr("REDUCE_SRC_LAST")) {
    axis->priority += 1;
  }
}

void VectorizedStrategy::AddConstraint() {
  if (analyzer_->op_type_ != INST_OP) {
    return;
  }
  for (auto axis : analyzer_->GetAxesOfAttr("INSTIZED")) {
    if (axis->HasAttr("DYNAMIC_BOUND")) {
      continue;
    }
    int64_t min_byte = -1;
    if (axis->data_size.empty()) {
      min_byte = 1;
    } else {
      for (const auto &it : axis->data_size) {
        if (min_byte == -1 || min_byte > it.second) {
          min_byte = it.second;
        }
      }
    }
    CHECK_NE(min_byte, 0);
    axis->l1_constraints.tile_mod_ = CanonicalSimplify(CastIntToExpr(INSTIZE_BYTE / min_byte));
  }
}

void TensorOfTensorStrategy::AddConstraint() {
  for (auto axis : analyzer_->GetAxesOfAttr("TOT")) {
    if (!axis->HasAttr("ALIGN:DMA")) continue;
    axis->TileRestrainToSingleValue(CastIntToExpr(MIN_TILE), LEVEC1);
  }
}

void PassDownAttrStrategy::AddConstraint() {
  for (auto axis : analyzer_->GetAxesOfAttr(AttrInfo{"ATTR", "pass_down"})) {
    axis->TileRestrainEntire(LEVEC1);
  }
}

void DynamicShapeLimitStrategy::AddConstraint() {
  auto interested_info = GetInterestedInfo(interested_attr_key);
  for (auto it : interested_info) {
    TileAxis *axis = it.first;
    for (const auto &attr : it.second) {
      CHECK_NE(attr.attr_value, "");
      axis->dyn_shape_limit = static_cast<int>(std::strtol(attr.attr_value.c_str(), nullptr, 10));
    }
  }
}

void DynamicBoundStrategy::AddConstraint() {
  auto interested_info = GetInterestedInfo(interested_attr_key);
  for (auto it : interested_info) {
    TileAxis *axis = it.first;
    for (const auto &attr : it.second) {
      CHECK_NE(attr.attr_value, "");
      auto bound = static_cast<int>(std::strtol(attr.attr_value.c_str(), nullptr, 10));
      axis->TileRestrainMod(bound, LEVEC1);
      axis->forbid_iso = true;
    }
  }
}

void ShiftAxisStrategy::AddConstraint() {
  auto interested_info = GetInterestedInfo(interested_attr_key);
  for (auto it : interested_info) {
    TileAxis *axis = it.first;
    int64_t const_extent = axis->GetConstExtent();
    if (const_extent == -1) {
      continue;
    }
    for (const auto &attr : it.second) {
      CHECK_NE(attr.attr_value, "");
      auto share_time = static_cast<int>(std::strtol(attr.attr_value.c_str(), nullptr, 10));
      axis->TileRestrainToSingleValue(const_extent * (share_time + 1), LEVEC1);
      break;
    }
  }
}

void ModShiftAxisStrategy::AddConstraint() {
  auto interested_info = GetInterestedInfo(interested_attr_key);
  for (auto it : interested_info) {
    TileAxis *axis = it.first;
    int64_t const_extent = axis->GetConstExtent();
    if (const_extent == -1) {
      continue;
    }
    for (const auto &attr : it.second) {
      axis->forbid_iso = true;
      auto imm_min = axis->GetConstConstraint(LEVEC1).tile_min_.as<IntImm>()->value;
      if (imm_min > const_extent) {
        CHECK_NE(attr.attr_value, "");
        auto share_time = static_cast<int>(std::strtol(attr.attr_value.c_str(), nullptr, 10));
        axis->TileRestrainToSingleValue(const_extent * (share_time + 1), LEVEC1);
      } else {
        auto ForbidOthersIso = [](TileAxis *a) { a->forbid_iso = true; };
        analyzer_->ForEachAxisTopDown(ForbidOthersIso);
      }
      break;
    }
  }
}

void ConvStrategy::AddConstraint() {
  conv_info_ = analyzer_->scop_->GetConvInfoForTiling();
  auto interested_info = GetInterestedInfo(interested_attr_key);
  for (auto it : interested_info) {
    TileAxis *axis = it.first;
    for (const auto &attr : it.second) {
      axis->axis_type_ = attr.attr_value;
      if (attr.attr_value == "N" || attr.attr_value == "C1_in_out") {
        axis->TileRestrainToSingleValue(CastIntToExpr(MIN_TILE), LEVEC1);
        axis->TileRestrainToSingleValue(CastIntToExpr(MIN_TILE), LEVEC0);
      } else if (attr.attr_value == "H") {
        RestrainH(axis);
      } else if (attr.attr_value == "W") {
        if (analyzer_->scop_->IsConvBackpropFilter()) {
          axis->TileRestrainEntire(LEVEC1);
        } else {
          RestrainW(axis);
        }
      } else if (attr.attr_value.find("C0") != std::string::npos || attr.attr_value == "kh" ||
                 attr.attr_value == "kw") {
        axis->TileRestrainEntire(LEVEC1);
      } else if (attr.attr_value == "C1_in" && analyzer_->is_dynamic_) {
        // dynamic case
        axis->TileRestrainEntire(LEVEC1);
      }
    }
  }
}

void ConvStrategy::RestrainH(TileAxis *axis) {
  CHECK(conv_info_.find(ATTR_CONV_FEATURE_H) != conv_info_.end());
  CHECK(conv_info_.find(ATTR_CONV_PAD_TOP) != conv_info_.end());
  CHECK(conv_info_.find(ATTR_CONV_STRIDE_H) != conv_info_.end());
  CHECK(conv_info_.find(ATTR_CONV_DILATION_H) != conv_info_.end());
  CHECK(conv_info_.find(ATTR_CONV_KERNEL_H) != conv_info_.end());
  Expr h = conv_info_[ATTR_CONV_FEATURE_H];
  Expr p_top = conv_info_[ATTR_CONV_PAD_TOP];
  Expr s_h = conv_info_[ATTR_CONV_STRIDE_H];
  Expr d_h = conv_info_[ATTR_CONV_DILATION_H];
  Expr k_h = conv_info_[ATTR_CONV_KERNEL_H];
  CHECK(h.defined() && p_top.defined() && s_h.defined() && d_h.defined() && k_h.defined()) << "Conv attr not defined.";
  Expr k_h_d = (k_h - 1) * d_h + 1;
  int tile_out_h = MIN_TILE + 1;
  while (arith_ana_.CanProve(
    ((air::ir::FloorDiv::make((axis->range_extent + tile_out_h - 1), CastIntToExpr(tile_out_h)) - 1) * tile_out_h -
     1) * s_h +
        k_h_d >
      h + p_top &&
    tile_out_h <= axis->range_extent)) {
    tile_out_h += 1;
  }
  axis->l1_constraints.tile_min_ = CastIntToExpr(tile_out_h);
}

void ConvStrategy::RestrainW(TileAxis *axis) {
  CHECK(conv_info_.find(ATTR_CONV_FEATURE_W) != conv_info_.end());
  CHECK(conv_info_.find(ATTR_CONV_PAD_LEFT) != conv_info_.end());
  CHECK(conv_info_.find(ATTR_CONV_STRIDE_W) != conv_info_.end());
  CHECK(conv_info_.find(ATTR_CONV_DILATION_W) != conv_info_.end());
  CHECK(conv_info_.find(ATTR_CONV_KERNEL_W) != conv_info_.end());
  Expr w = conv_info_[ATTR_CONV_FEATURE_W];
  Expr p_left = conv_info_[ATTR_CONV_PAD_LEFT];
  Expr s_w = conv_info_[ATTR_CONV_STRIDE_W];
  Expr d_w = conv_info_[ATTR_CONV_DILATION_W];
  Expr k_w = conv_info_[ATTR_CONV_KERNEL_W];
  CHECK(w.defined() && p_left.defined() && s_w.defined() && d_w.defined() && k_w.defined()) << "Conv attr not defined.";
  Expr k_w_d = (k_w - 1) * d_w + 1;
  int tile_out_w = 1;
  while (arith_ana_.CanProve(
    ((air::ir::FloorDiv::make((axis->range_extent + tile_out_w - 1), CastIntToExpr(tile_out_w)) - 1) * tile_out_w -
     1) * s_w +
        k_w_d >
      w + p_left &&
    tile_out_w <= axis->range_extent)) {
    tile_out_w += 1;
  }
  axis->l1_constraints.tile_min_ = CastIntToExpr(tile_out_w);
}

void GemmStrategy::AddConstraint() {
  auto interested_info = GetInterestedInfo(interested_attr_key);
  for (auto it : interested_info) {
    TileAxis *axis = it.first;
    for (const auto &attr : it.second) {
      axis->axis_type_ = attr.attr_value;
      if (attr.attr_value == "mi" || attr.attr_value == "ni" || attr.attr_value == "ki") {
        axis->TileRestrainMod(CastIntToExpr(MMU_UNIT), LEVEC1);
        axis->TileRestrainMod(CastIntToExpr(MMU_UNIT), LEVEC0);
        axis->TileRestrainToSingleValue(CastIntToExpr(MMU_UNIT), LEVEC1);
        axis->TileRestrainToSingleValue(CastIntToExpr(MMU_UNIT), LEVEC0);
      } else if (attr.attr_value == "bo" || attr.attr_value == "bi") {
        axis->TileRestrainToSingleValue(CastIntToExpr(MIN_TILE), LEVEC1);
        axis->TileRestrainToSingleValue(CastIntToExpr(MIN_TILE), LEVEC0);
      }
    }
  }
}

std::pair<int, int> MulticoreStrategy::GetProposalRangeForFullMulticore(TileAxis *multicore_axis) {
  int max_core = cand_.GetCoreNumConf();
  int used_core = 1;
  std::pair<int, int> proposal_range = std::make_pair(
    std::max(static_cast<int>(MIN_MULTICORE_BYTES / cand_.GetMinUbToGmDataAfterAxis(multicore_axis)), 1), -1);
  auto this_level_core = std::max(static_cast<int>(max_core / used_core), 1);
  std::stringstream ss;
  if (multicore_axis->range_extent.as<IntImm>() == nullptr) return proposal_range;
  auto shape = multicore_axis->range_extent.as<IntImm>()->value;
  bool is_last_level = false;
  for (auto other_axis : cand_.GetTileAxis()) {
    if (other_axis == multicore_axis) break;
    if (other_axis->index != multicore_axis->index || other_axis->HasAttr("REDUCE_AXIS")) continue;
    if (other_axis->range_extent.as<IntImm>() == nullptr) return proposal_range;
    int64_t l1_val = TileVarId::UNDEFINE;
    std::tie(l1_val, std::ignore) = cand_.GetConstTileVal(other_axis);
    if (l1_val == TileVarId::VAR) return proposal_range;
    if (l1_val == TileVarId::UNDEFINE) {
      CHECK(other_axis->l1_constraints.tile_min_.as<IntImm>())
        << "Static shape " << shape << " should have const tile min, while got "
        << other_axis->l1_constraints.tile_min_;
      l1_val = other_axis->l1_constraints.tile_min_.as<IntImm>()->value;
    }
    auto block_extent = std::max(static_cast<int>(other_axis->range_extent.as<IntImm>()->value / l1_val), 1);
    ss << "range " << multicore_axis->range_extent << " l1 tile " << l1_val << " -> block extent " << block_extent
       << " this level " << this_level_core;
    logger_.AppendLog(DO_TILING, ss);
    ss.str("");
    if (block_extent > this_level_core) {
      int factor = (block_extent + this_level_core - 1) / this_level_core;
      this_level_core = (block_extent + factor - 1) / factor;
      is_last_level = true;
    } else if (block_extent * 2 > this_level_core) {
      this_level_core = block_extent;
      is_last_level = true;
    } else {
      this_level_core = block_extent;
    }
    if (is_last_level) break;
    used_core *= this_level_core;
    this_level_core = std::max(static_cast<int>(max_core / used_core), 1);
    ss << "use core " << used_core << " this level " << this_level_core;
    logger_.AppendLog(DO_TILING, ss);
    ss.str("");
  }
  proposal_range.second = std::max(static_cast<int>(shape / this_level_core), 1);
  ss << " proposal range (" << proposal_range.first << ", " << proposal_range.second << ")";
  logger_.AppendLog(DO_TILING, ss);
  return proposal_range;
}
int64_t MulticoreStrategy::AdjustTilingAccordingToMulticoreConstraint(TileAxis *multicore_axis, int64_t tiling_factor) {
  CHECK_GT(tiling_factor, 0) << "tiling factor cant be zero or negative";
  auto proposal_range = GetProposalRangeForFullMulticore(multicore_axis);
  auto min_factor_for_enough_data = proposal_range.first;
  auto max_factor_for_full_cores = proposal_range.second;
  auto origin_factor = tiling_factor;
  std::stringstream ss;

  if ((!multicore_axis->mc_sup) || (multicore_axis->HasAttr("REDUCE_AXIS")) ||
      (tiling_factor < cand_.GetMinFactorToEnableMulticore(multicore_axis) ||
       (tiling_factor == max_factor_for_full_cores) || (max_factor_for_full_cores <= 0))) {
    logger_.AppendLine(DO_TILING, "This axis is not suitable for multicore, return.");
    return origin_factor;
  }

  auto CheckConstConstraint = [this, &ss](Expr constraint) {
    if (constraint.as<IntImm>() == nullptr) {
      ss << "Static shape should have const constraint, while got " << constraint;
      logger_.LogFatalAndSaveLog(ss.str());
    }
  };
  CheckConstConstraint(multicore_axis->range_extent);
  CheckConstConstraint(multicore_axis->l1_constraints.tile_min_);
  CheckConstConstraint(multicore_axis->l1_constraints.tile_mod_);

  if (tiling_factor < max_factor_for_full_cores) {
    auto end = static_cast<int>(sqrt(max_factor_for_full_cores));
    while (max_factor_for_full_cores % tiling_factor != 0 && tiling_factor > end) --tiling_factor;
  } else {
    tiling_factor = max_factor_for_full_cores;
  }

  auto shape = multicore_axis->range_extent.as<IntImm>()->value;
  bool efficient = (shape % tiling_factor == 0) >= (shape % origin_factor == 0);
  auto multicore_shrink_limit = 2;
  auto reduced_mem = std::max(origin_factor - tiling_factor, min_factor_for_enough_data - tiling_factor);
  auto pending_blocks = cand_.GetMaximalPendingBlocks(multicore_axis);
  if ((static_cast<int>(origin_factor / tiling_factor) >= multicore_shrink_limit) && reduced_mem > pending_blocks) {
    ss << "If axis adjust to " << tiling_factor << ", " << reduced_mem << " memory is reduced;"
       << " while maximal pending blocks is only " << pending_blocks << ", adjust may not be efficient.";
    logger_.AppendLog(DO_TILING, ss);
    efficient = false;
  }
  bool valid = tiling_factor >= multicore_axis->l1_constraints.tile_min_.as<IntImm>()->value;
  if (tiling_factor >= multicore_axis->l1_constraints.tile_mod_.as<IntImm>()->value) {
    valid = valid && tiling_factor % multicore_axis->l1_constraints.tile_mod_.as<IntImm>()->value == 0;
  } else {
    auto weak_constraint = multicore_axis->l1_constraints.tile_mod_.as<IntImm>()->value % tiling_factor == 0;
    valid = valid && multicore_axis->HasAttr("INSTIZED") && weak_constraint;
  }
  ss << "--> Adjust tiling factor " << origin_factor << " to " << tiling_factor << " if valid(" << valid
     << ") and efficient(" << efficient << ") according to proposal range (" << min_factor_for_enough_data << ", "
     << max_factor_for_full_cores << ")";
  logger_.AppendLog(DO_TILING, ss);
  return (valid && efficient) ? tiling_factor : origin_factor;
}

}  // namespace poly
}  // namespace ir
}  // namespace akg
