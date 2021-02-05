/**
 * Copyright 2019-2021 Huawei Technologies Co., Ltd
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
#include "poly/dma_inject.h"

#include <tvm/ir.h>
#include <algorithm>
#include <utility>

#include "poly/scop.h"
#include "poly/transform.h"
#include "poly/scop_builder.h"

namespace akg {
namespace ir {
namespace poly {

isl::map StrideNormalization(const isl::map &access, const isl::multi_val &strides, const isl::multi_aff &offsets) {
  CHECK_EQ(strides.size(), offsets.size());
  auto space = access.get_space();
  space = space.range();
  space = space.map_from_set();
  auto ma = isl::multi_aff::identity(space);
  ma = ma.scale_down(strides);
  auto ret = access.sum(isl::map(offsets.neg()));
  ret = ret.apply_range(isl::map(ma));
  return ret;
}

struct EqualityConstraintInfo {
  int range_dim;
  int domain_dim;
  int range_stride;
  int domain_stride;
  int offset;

  bool operator==(const EqualityConstraintInfo &x) const {
    return range_dim == x.range_dim && domain_dim == x.domain_dim && offset == x.offset &&
           range_stride == x.range_stride && domain_stride == x.domain_stride;
  }
  bool operator!=(const EqualityConstraintInfo &x) const { return !(operator==(x)); }
};

using EqualityInfoMap = std::unordered_map<int, EqualityConstraintInfo>;

void ExtractOffsetFromConstraintEx(__isl_keep isl_constraint *c, EqualityInfoMap *equality_info_map) {
  CHECK(equality_info_map != nullptr);
  if (!isl_constraint_is_equality(c)) return;

  EqualityConstraintInfo info;
  auto val_ptr = isl_constraint_get_constant_val(c);
  info.offset = static_cast<int>(isl_val_get_num_si(val_ptr));
  static_cast<void>(isl_val_free(val_ptr));

  int n_dim_in = isl_constraint_dim(c, isl_dim_in);
  int n_dim_out = isl_constraint_dim(c, isl_dim_out);
  bool domain_found = false;
  bool range_found = false;

  for (int i = 0; i < n_dim_in; ++i) {
    auto coef_val = isl_constraint_get_coefficient_val(c, isl_dim_in, i);
    int64_t coef = isl_val_get_num_si(coef_val);
    static_cast<void>(isl_val_free(coef_val));
    if (coef == 0) continue;
    if (domain_found) return;
    domain_found = true;
    info.domain_stride = coef;
    info.domain_dim = i;
  }

  for (int i = 0; i < n_dim_out; ++i) {
    auto coef_val = isl_constraint_get_coefficient_val(c, isl_dim_out, i);
    int64_t coef = isl_val_get_num_si(coef_val);
    static_cast<void>(isl_val_free(coef_val));
    if (coef == 0) continue;
    if (range_found) return;
    range_found = true;
    info.range_stride = coef;
    info.range_dim = i;
  }

  if (info.range_stride < 0) {
    info.range_stride = -info.range_stride;
    info.domain_stride = -info.domain_stride;
    info.offset = -info.offset;
  }

  if (domain_found && range_found) {
    equality_info_map->operator[](info.range_dim) = info;
  }
}

isl_stat ExtractOffsetFromConstraint(__isl_take isl_constraint *c, void *user) {
  CHECK(c != nullptr);
  CHECK(user != nullptr);
  auto equality_info_map = reinterpret_cast<EqualityInfoMap *>(user);
  ExtractOffsetFromConstraintEx(c, equality_info_map);
  static_cast<void>(isl_constraint_free(c));
  return isl_stat_ok;
}

bool IsReadWriteAccessesMergeable(const isl::map &access, std::vector<int> &unmergeable_dims) {
  EqualityInfoMap ref_info_map;
  bool isMergeable = true;
  access.foreach_basic_map([&](const isl::basic_map &bmap) -> void {
    isl::basic_map simplified_bmap = bmap.detect_equalities();
    EqualityInfoMap equality_info_map;
    isl_stat status = isl_basic_map_foreach_constraint(simplified_bmap.get(), ExtractOffsetFromConstraint,
                                                       reinterpret_cast<void *>(&equality_info_map));
    CHECK(status == isl_stat_ok);
    if (equality_info_map.empty()) return;
    for (const auto &pair : equality_info_map) {
      const auto &dim_info = pair.second;
      if (ref_info_map.count(dim_info.range_dim) == 0) {
        ref_info_map[dim_info.range_dim] = dim_info;
      } else if (dim_info != ref_info_map[dim_info.range_dim]) {
        isMergeable = false;
        unmergeable_dims.push_back(dim_info.range_dim);
      }
    }
  });
  return isMergeable;
}

ScopedFootprint ComputeFootprintOfRange(const isl::map &access) {
  ScopedFootprint footprint;
  footprint.stride_values = isl::multi_val::zero(access.get_space().range());
  footprint.stride_offsets = isl::multi_aff::zero(access.get_space());

  int n_subscripts = footprint.stride_values.size();
  for (int i = 0; i < n_subscripts; ++i) {
    auto si = access.get_range_stride_info(i);
    footprint.stride_values = footprint.stride_values.set_val(i, si.get_stride());
    footprint.stride_offsets = footprint.stride_offsets.set_aff(i, si.get_offset());
  }

  isl::map recorded_access = StrideNormalization(access, footprint.stride_values, footprint.stride_offsets);
  footprint.box = recorded_access.get_range_simple_fixed_box_hull();
  footprint.is_valid = true;
  footprint.should_split = false;
  return footprint;
}

isl::aff GetZeroAff(const isl::aff &aff) {
  isl_aff *zero_aff = aff.copy();
  CHECK(zero_aff != nullptr);
  zero_aff = isl_aff_set_constant_si(zero_aff, 0);
  size_t n_dim = isl_aff_dim(zero_aff, isl_dim_in);
  CHECK_GE(n_dim, 0);
  for (size_t i = 0; i < n_dim; ++i) {
    zero_aff = isl_aff_set_coefficient_si(zero_aff, isl_dim_in, i, 0);
  }
  return isl::manage(zero_aff);
}

void ResetFootprintStrides(ScopedFootprint &footprint) {
  for (auto invalid_dim : footprint.invalid_dims) {
    footprint.stride_values = footprint.stride_values.set_val(invalid_dim, 1);
    isl::aff original_offset = footprint.stride_offsets.get_aff(invalid_dim);
    footprint.stride_offsets = footprint.stride_offsets.set_aff(invalid_dim, GetZeroAff(original_offset));
  }
}

void ResizeFootprintBox(const isl::map &access, ScopedFootprint &footprint, const int first_invalid_domain_dim) {
  if (first_invalid_domain_dim == -1) return;

  isl::map recorded_access = StrideNormalization(access, footprint.stride_values, footprint.stride_offsets);
  isl_map *stripped_access = recorded_access.copy();
  CHECK(stripped_access != nullptr);
  int num_invalid_dims = recorded_access.dim(isl_dim_in) - first_invalid_domain_dim;
  stripped_access = isl_map_remove_dims(stripped_access, isl_dim_in, first_invalid_domain_dim, num_invalid_dims);
  isl::map unshifted_access = isl::manage(stripped_access).add_dims(isl_dim_in, num_invalid_dims);
  footprint.box = unshifted_access.get_range_simple_fixed_box_hull();
}

ScopedFootprint ReComputeFootprintOfRange(const isl::map &access, const std::vector<int> &unmergeable_dims) {
  ScopedFootprint footprint = ComputeFootprintOfRange(access);
  auto default_footprint = ComputeBufferFootprint(access, footprint);
  int first_invalid_domain_dim = -1;
  auto identity_footprint_dims = ExpandInvalidDims(unmergeable_dims, default_footprint, first_invalid_domain_dim);
  if (first_invalid_domain_dim == -1) return footprint;

  footprint.is_valid = false;
  footprint.invalid_dims = identity_footprint_dims;
  ResetFootprintStrides(footprint);
  ResizeFootprintBox(access, footprint, first_invalid_domain_dim);
  return footprint;
}

std::unique_ptr<TensorFootprintCluster> TensorFootprintCluster::ComputeFootprintCluster(const isl::map &original_access,
                                                                                        const isl::map &scoped_access,
                                                                                        ReferenceType type,
                                                                                        bool need_dma,
                                                                                        bool need_extension) {
  auto cluster = std::unique_ptr<TensorFootprintCluster>(new (std::nothrow) TensorFootprintCluster);
  CHECK(cluster) << "memory alloc fail.";
  auto fp = std::unique_ptr<TensorFootprint>(
    new (std::nothrow) TensorFootprint(original_access, scoped_access, type, need_dma, need_extension));
  CHECK(fp) << "memory alloc fail.";
  cluster->tensor_foot_prints.push_back(std::move(fp));
  cluster->foot_print_ = ComputeFootprintOfRange(scoped_access.domain_factor_domain());

  if (!cluster->foot_print_.box.is_valid()) {
    LOG(WARNING) << "foot_print_ box is invalid, scoped_access: " << scoped_access.domain_factor_domain();
    return cluster;
  }

  cluster->footprint_map_ = isl::map(cluster->ComputeBufferedFootprints());

  return cluster;
}

isl::aff TensorFootprintCluster::LowerBound(const isl::aff &offset, const isl::val &stride,
                                            const isl::aff &stride_offset) const {
  return offset * stride + stride_offset;
}

isl::aff TensorFootprintCluster::UpperBound(const isl::val &size, const isl::aff &offset, const isl::val &stride,
                                            const isl::aff &stride_offset) const {
  return (offset + size) * stride + stride_offset;
}

isl::map TensorFootprintCluster::ExtractSingleAccessRelation() const {
  auto accessed_domain = RichAccessRelations().domain();
  auto space = foot_print_.box.get_space();
  auto referenced = isl::map::universe(space).intersect_domain(accessed_domain);

  auto identity = isl::multi_aff::identity(space.range().map_from_set());
  for (int i = 0; i < static_cast<int>(foot_print_.GetBoxDim()); ++i) {
    CHECK(!foot_print_.GetBoxSizeValue(i).is_infty())
      << "cannot determine foot_print_ box, please specify the boundary of shape of cluster " << *this;
    auto lower =
      LowerBound(foot_print_.GetBoxLowerBound(i), foot_print_.GetStrideValue(i), foot_print_.GetStrideOffset(i));
    auto upper = UpperBound(foot_print_.GetBoxSizeValue(i), foot_print_.GetBoxLowerBound(i),
                            foot_print_.GetStrideValue(i), foot_print_.GetStrideOffset(i));
    auto iden_aff = identity.get_aff(i);
    auto partial = (lower <= iden_aff) & (upper > iden_aff);
    referenced = referenced & partial;
  }
  return referenced;
}

bool TensorFootprintCluster::UnWriteable() const {
  for (auto const &footprint : tensor_foot_prints) {
    if (footprint->type == ReferenceType::Write) return false;
  }
  return true;
}

bool TensorFootprintCluster::UnReadable() const {
  for (auto const &footprint : tensor_foot_prints) {
    if (footprint->type == ReferenceType::Read) return false;
  }
  return true;
}

bool CheckeSpaceEuality(isl::space space, isl::multi_val mval) {
  auto copy = mval.get_space();
  if (!space.has_equal_tuples(copy)) return false;
  return true;
}

isl::set TensorFootprintCluster::BufferedFootprint() const {
  auto space = RichAccessRelations().range().space();
  auto sizes = foot_print_.box.get_size();
  if (!CheckeSpaceEuality(space, sizes)) {
    LOG(FATAL) << "unexpected dimensionality mismatch";
  }

  isl::set footprint = isl::set::universe(space);
  auto identity = isl::multi_aff::identity(space.map_from_set());
  for (int i = 0, e = sizes.size(); i < e; ++i) {
    footprint = footprint & (identity.aff(i) >= 0) & (identity.aff(i) < sizes.val(i));
  }
  return footprint;
}

std::vector<size_t> TensorFootprintCluster::GetFixedBoxSizes() const {
  std::vector<size_t> fix_box_size;
  auto box_size = foot_print_.box.get_size();
  fix_box_size.reserve(box_size.size());
  auto val_list = box_size.get_val_list();
  for (const auto &val : val_list) {
    fix_box_size.push_back(val.get_num_si());
  }
  return fix_box_size;
}

isl::map RichAccessRelation(const TensorFootprintCluster &cluster, ReferenceType type) {
  auto accesses = isl::map::empty(cluster.tensor_foot_prints.front()->scoped_access.space());
  if (!cluster.tensor_foot_prints.empty()) {
    for (const auto &foorprint : cluster.tensor_foot_prints) {
      if (foorprint->type == type) {
        accesses = accesses.unite(foorprint->scoped_access);
      } else {
        continue;
      }
    }
  } else {
    LOG(FATAL) << "no tensor_foot_prints in the group";
  }
  return accesses;
}

bool NeedDmaImpl(const TensorFootprintCluster &cluster, ReferenceType type) {
  if (cluster.tensor_foot_prints.empty()) {
    LOG(FATAL) << "no references in the cluster";
  }

  for (const auto &footprint : cluster.tensor_foot_prints) {
    if (footprint->type != type) {
      continue;
    }
    if (footprint->need_dma) {
      return true;
    }
  }
  return false;
}

bool NeedExtensionImpl(const TensorFootprintCluster &cluster, ReferenceType type) {
  if (cluster.tensor_foot_prints.empty()) {
    LOG(FATAL) << "no tensor_foot_prints in the cluster";
  }

  for (const auto &footprint : cluster.tensor_foot_prints) {
    if (footprint->type != type) {
      continue;
    }
    if (footprint->need_extension) {
      return true;
    }
  }
  return false;
}

isl::map TensorFootprintCluster::RichWriteRelations() const { return RichAccessRelation(*this, ReferenceType::Write); }

isl::map TensorFootprintCluster::RichReadRelations() const { return RichAccessRelation(*this, ReferenceType::Read); }

bool TensorFootprintCluster::WriteNeedDma() const { return NeedDmaImpl(*this, ReferenceType::Write); }

bool TensorFootprintCluster::ReadNeedDma() const { return NeedDmaImpl(*this, ReferenceType::Read); }

bool TensorFootprintCluster::WriteNeedExtension() const { return NeedExtensionImpl(*this, ReferenceType::Write); }

bool TensorFootprintCluster::ReadNeedExtension() const { return NeedExtensionImpl(*this, ReferenceType::Read); }

size_t GetFootprintSize(std::unique_ptr<TensorFootprintCluster> &cluster) {
  auto box_sizes = cluster->foot_print_.box.get_size();
  size_t size_val = 1;
  int dims = static_cast<int>(box_sizes.size());
  for (int i = 0; i < dims; ++i) {
    size_val *= static_cast<size_t>(box_sizes.get_val(i).get_num_si());
  }
  return size_val;
}

bool MergedClusterHasLargerSize(std::unique_ptr<TensorFootprintCluster> &cluster1,
                                std::unique_ptr<TensorFootprintCluster> &cluster2,
                                std::unique_ptr<TensorFootprintCluster> &merged) {
  return GetFootprintSize(cluster1) + GetFootprintSize(cluster2) < GetFootprintSize(merged);
}

std::unique_ptr<TensorFootprintCluster> TensorFootprintCluster::ClusteringFootprints(
  std::unique_ptr<TensorFootprintCluster> &&cluster1, std::unique_ptr<TensorFootprintCluster> &&cluster2) {
  CHECK(cluster1);
  CHECK(cluster2);
  auto ret = std::unique_ptr<TensorFootprintCluster>(new TensorFootprintCluster);
  CHECK(ret);
  auto total = cluster1->tensor_foot_prints.size() + cluster2->tensor_foot_prints.size();
  ret->tensor_foot_prints.reserve(total);

  for (auto it = cluster1->tensor_foot_prints.begin(); it != cluster1->tensor_foot_prints.end(); ++it) {
    ret->tensor_foot_prints.push_back(std::move(*it));
  }

  for (auto it = cluster2->tensor_foot_prints.begin(); it != cluster2->tensor_foot_prints.end(); ++it) {
    ret->tensor_foot_prints.push_back(std::move(*it));
  }

  auto accesses = ret->RichAccessRelations();
  bool has_only_read_or_write = ret->UnWriteable() || ret->UnReadable();
  std::vector<int> unmergeable_dims;
  bool is_mergeable = (cluster1->foot_print_.box.get_offset().plain_is_equal(cluster2->foot_print_.box.get_offset()) ||
                       IsReadWriteAccessesMergeable(accesses, unmergeable_dims));
  if (has_only_read_or_write || is_mergeable) {
    ret->foot_print_ = ComputeFootprintOfRange(accesses);
    if (has_only_read_or_write && !is_mergeable) {
      ret->foot_print_.should_split = true;
    } else if (has_only_read_or_write && MergedClusterHasLargerSize(cluster1, cluster2, ret)) {
      LOG(WARNING) << "two footprints of tensor " << accesses.range().get_tuple_id()
                   << " are merged, resulting in a larger size";
    }
  } else {
    LOG(INFO) << "cannot tile tensor " << accesses.range().get_tuple_id()
              << " because accesses in different tiles cannot merge: " << accesses;
    ret->foot_print_ = ReComputeFootprintOfRange(accesses, unmergeable_dims);
  }
  return ret;
}

/*
 * Get shape info n_dim & shape
 * 1. find n_dim & shape from binds based on tensor_id
 * 2. if found, update n_dim & shape from buf_def based on tensor_id
 * */
void TensorShapeInfo(const Scop &scop, const isl::id &tensor_id, size_t &n_dim, Array<Expr> &shape) {
  n_dim = 0;
  for (const auto &i : scop.binds_) {
    if (i.first->op->name == tensor_id.get_name()) {
      n_dim = i.first.ndim();
      shape = i.first->shape;
    }
  }

  if (!n_dim) {
    auto buf_def = scop.GetBufferDefInfo(tensor_id);
    n_dim = buf_def.sizes.size();
    for (auto i : buf_def.sizes) {
      shape.push_back(Expr(i));
    }
  }
}

isl::set CollectTensorSet(const Scop &scop, const isl::id &tensor_id) {
  auto space = scop.schedule_.get_domain().get_space();
  size_t n_dim;
  Array<Expr> shape;
  TensorShapeInfo(scop, tensor_id, n_dim, shape);

  auto coordinate = CollectTensorCoordinate(space, tensor_id, n_dim);
  auto tensor_set = isl::set::universe(coordinate.get_space());
  if (n_dim == 0) return tensor_set;

  auto identity = isl::multi_aff::identity(coordinate.get_space().map_from_set());
  for (size_t i = 0; i < n_dim; ++i) {
    auto min = Int2Aff(space, 0).unbind_params_insert_domain(coordinate);
    auto extent = Expr2Aff(space, shape[i]).unbind_params_insert_domain(coordinate);
    auto aff = isl::multi_aff::identity(coordinate.get_space().map_from_set()).get_aff(static_cast<int>(i));
    tensor_set = tensor_set & (min.le_set(aff)) & (aff.le_set(min + extent - 1));
  }
  return tensor_set;
}

/*
 * remove the schedule dimensions  which corresponding to size-one tensor dimension.
 */
isl::multi_aff RemoveDimensionOfSizeOne(const isl::multi_aff &schedule, const std::vector<size_t> &tensor_dim) {
  auto squashed_aff = schedule.get_aff_list();
  auto ori_size = squashed_aff.size();
  for (int i = ori_size - 1; i >= 0; --i) {
    auto pos = static_cast<unsigned int>(i);
    if (pos < tensor_dim.size() && tensor_dim[pos] == 1) {
      squashed_aff = squashed_aff.drop(pos, tensor_dim[pos]);
    }
  }

  auto squashed_domain = schedule.get_space().domain();
  squashed_domain = squashed_domain.add_unnamed_tuple_ui((unsigned int)squashed_aff.size());
  return isl::multi_aff(squashed_domain, squashed_aff);
}

isl::map GetScopedAccess(const isl::union_map &schedule, const isl::map &access) {
  auto union_access = isl::union_map(access.curry());
  union_access = union_access.apply_domain(schedule);
  auto scoped_access = isl::map::from(union_access);
  return scoped_access.uncurry();
}

isl::map GemmInnerTransposeAffine::ConstructAffine(const isl::map original_map) {
  // space:: S -> O
  isl::space original_space = original_map.get_space();

  // MA:: [S -> O] -> O
  auto original_space_inserter = isl::multi_aff::range_map(original_space);

  isl::map footprint = isl::map(original_space_inserter);

  // map:: O -> O
  footprint = footprint.curry().range().unwrap();

  int n_in = footprint.dim(isl_dim_in);
  int n_out = footprint.dim(isl_dim_out);

  auto footprint_space = footprint.get_space();

  auto p_s = footprint_space.wrap();
  auto ls = isl::local_space(p_s);

  std::vector<isl::aff> v_aff_x;
  CHECK_GE(n_in, 0);
  for (int i = 0; i < n_in; ++i) {
    isl::aff aff_i;
    aff_i = aff_i.var_on_domain(ls, isl_dim_out, i);
    v_aff_x.push_back(aff_i);
  }

  std::vector<isl::aff> v_aff_y;
  CHECK_GE(n_out, 0);
  for (int i = 0; i < n_out; ++i) {
    isl::aff aff_i;
    aff_i = aff_i.var_on_domain(ls, isl_dim_out, n_in + i);
    v_aff_y.push_back(aff_i);
  }

  // construct affine map
  // B no ko ki ni ---> B no ko ni ki
  CHECK(v_aff_x.size() == v_aff_y.size());
  size_t len = v_aff_x.size();
  CHECK_GE(len, 4);

  isl::set set_1 = v_aff_x[len - 4].eq_set(v_aff_y[len - 4]);
  isl::set set_2 = v_aff_x[len - 3].eq_set(v_aff_y[len - 3]);
  isl::set set_3 = v_aff_x[len - 2].eq_set(v_aff_y[len - 1]);
  isl::set set_4 = v_aff_x[len - 1].eq_set(v_aff_y[len - 2]);

  isl::set set = set_1.intersect(set_2).intersect(set_3).intersect(set_4);
  for (size_t i = 0; i < len - 4; ++i) {
    isl::set set_tmp = v_aff_x[i].eq_set(v_aff_y[i]);
    set = set.intersect(set_tmp);
  }

  footprint = set.unwrap();
  return footprint;
}

isl::map GemmTransposeAffine::ConstructAffine(const isl::map original_map) {
  // space:: S -> O
  isl::space original_space = original_map.get_space();

  // MA:: [S -> O] -> O
  auto original_space_inserter = isl::multi_aff::range_map(original_space);

  isl::map footprint = isl::map(original_space_inserter);

  // map:: O -> O
  footprint = footprint.curry().range().unwrap();

  int n_in = footprint.dim(isl_dim_in);
  int n_out = footprint.dim(isl_dim_out);

  auto footprint_space = footprint.get_space();

  auto p_s = footprint_space.wrap();
  auto ls = isl::local_space(p_s);

  std::vector<isl::aff> v_aff_x;
  CHECK_GE(n_in, 0);
  for (int i = 0; i < n_in; ++i) {
    isl::aff aff_i;
    aff_i = aff_i.var_on_domain(ls, isl_dim_out, i);
    v_aff_x.push_back(aff_i);
  }

  std::vector<isl::aff> v_aff_y;
  CHECK_GE(n_out, 0);
  for (int i = 0; i < n_out; ++i) {
    isl::aff aff_i;
    aff_i = aff_i.var_on_domain(ls, isl_dim_out, n_in + i);
    v_aff_y.push_back(aff_i);
  }

  // construct affine map
  // B no ko ki ni ---> B ko no ni ki
  CHECK(v_aff_x.size() == v_aff_y.size());
  size_t len = v_aff_x.size();
  CHECK_GE(len, 4);

  isl::set set_1 = v_aff_x[len - 4].eq_set(v_aff_y[len - 3]);
  isl::set set_2 = v_aff_x[len - 3].eq_set(v_aff_y[len - 4]);
  isl::set set_3 = v_aff_x[len - 2].eq_set(v_aff_y[len - 1]);
  isl::set set_4 = v_aff_x[len - 1].eq_set(v_aff_y[len - 2]);

  isl::set set = set_1.intersect(set_2).intersect(set_3).intersect(set_4);
  for (size_t i = 0; i < len - 4; ++i) {
    isl::set set_tmp = v_aff_x[i].eq_set(v_aff_y[i]);
    set = set.intersect(set_tmp);
  }

  footprint = set.unwrap();
  return footprint;
}

isl::map GemmTransposeBlockAffine::ConstructAffine(const isl::map original_map) {
  // space:: S -> O
  isl::space original_space = original_map.get_space();

  // MA:: [S -> O] -> O
  auto original_space_inserter = isl::multi_aff::range_map(original_space);

  isl::map footprint = isl::map(original_space_inserter);

  // map:: O -> O
  footprint = footprint.curry().range().unwrap();

  int n_in = footprint.dim(isl_dim_in);
  int n_out = footprint.dim(isl_dim_out);

  auto footprint_space = footprint.get_space();

  auto p_s = footprint_space.wrap();
  auto ls = isl::local_space(p_s);

  std::vector<isl::aff> v_aff_x;
  CHECK_GE(n_in, 0);
  for (int i = 0; i < n_in; ++i) {
    isl::aff aff_i;
    aff_i = aff_i.var_on_domain(ls, isl_dim_out, i);
    v_aff_x.push_back(aff_i);
  }

  std::vector<isl::aff> v_aff_y;
  CHECK_GE(n_out, 0);
  for (int i = 0; i < n_out; ++i) {
    isl::aff aff_i;
    aff_i = aff_i.var_on_domain(ls, isl_dim_out, n_in + i);
    v_aff_y.push_back(aff_i);
  }

  /*B no ko ki ni ---> B ko no ni ki
   *
   * construct affine map
   * */
  CHECK(v_aff_x.size() == v_aff_y.size());
  size_t len = v_aff_x.size();
  CHECK_GE(len, 4);

  isl::set set_1 = v_aff_x[len - 4].eq_set(v_aff_y[len - 3]);
  isl::set set_2 = v_aff_x[len - 3].eq_set(v_aff_y[len - 4]);
  isl::set set_3 = v_aff_x[len - 2].eq_set(v_aff_y[len - 2]);
  isl::set set_4 = v_aff_x[len - 1].eq_set(v_aff_y[len - 1]);
  isl::set set = set_1.intersect(set_2).intersect(set_3).intersect(set_4);
  for (size_t i = 0; i < len - 4; ++i) {
    isl::set set_tmp = v_aff_x[i].eq_set(v_aff_y[i]);
    set = set.intersect(set_tmp);
  }

  footprint = set.unwrap();
  return footprint;
}

isl::map Im2colAffine::ConstructAffine(const isl::map original_map) {
  // space:: S -> O
  isl::space original_space = original_map.get_space();

  // MA:: [S -> O] -> O
  auto original_space_inserter = isl::multi_aff::range_map(original_space);

  isl::map footprint = isl::map(original_space_inserter);

  // map:: O -> O
  footprint = footprint.curry().range().unwrap();

  isl_map *p_footprint = footprint.copy();
  p_footprint = isl_map_add_dims(p_footprint, isl_dim_out, 1);

  int n_in = isl_map_dim(p_footprint, isl_dim_in);
  int n_out = isl_map_dim(p_footprint, isl_dim_out);

  CHECK_GE(n_out, 0);
  for (int i = 0; i < n_out; ++i) {
    std::string arg = "arg" + std::to_string(i) + "'";
    p_footprint = isl_map_set_dim_name(p_footprint, isl_dim_out, i, arg.c_str());
  }

  footprint = isl::manage(p_footprint);

  auto footprint_space = footprint.get_space();

  auto p_s = footprint_space.wrap();
  auto ls = isl::local_space(p_s);

  std::vector<isl::aff> v_aff_x;
  CHECK_GE(n_in, 0);
  for (int i = 0; i < n_in; ++i) {
    isl::aff aff_i;
    aff_i = aff_i.var_on_domain(ls, isl_dim_out, i);
    v_aff_x.push_back(aff_i);
  }

  std::vector<isl::aff> v_aff_y;
  CHECK_GE(n_out, 0);
  for (int i = 0; i < n_out; ++i) {
    isl::aff aff_i;
    aff_i = aff_i.var_on_domain(ls, isl_dim_out, n_in + i);
    v_aff_y.push_back(aff_i);
  }
  CHECK_GE(v_aff_x.size(), 5);
  CHECK_GE(v_aff_y.size(), 6);

  ConstructAffineMap(footprint, v_aff_x, v_aff_y, original_map, ls);
  return footprint;
}

void Im2colAffine::ConstructAffineMap(isl::map &footprint, std::vector<isl::aff> &v_aff_x,
                                      std::vector<isl::aff> &v_aff_y, const isl::map &original_map,
                                      isl::local_space &ls) {
  int64_t stride_h = 1;
  int64_t stride_w = 1;
  int64_t kernel_h = 0;
  int64_t kernel_w = 0;
  int64_t tileH = 0;
  int64_t tileW = 0;
  int64_t padLeft = 0;
  int64_t padTop = 0;

  auto it = attrInfo_.find(ATTR_CONV_STRIDE_H);
  if ((it != attrInfo_.end()) && (*it).second.as<IntImm>()) stride_h = (*it).second.as<IntImm>()->value;

  it = attrInfo_.find(ATTR_CONV_STRIDE_W);
  if ((it != attrInfo_.end()) && (*it).second.as<IntImm>()) stride_w = (*it).second.as<IntImm>()->value;

  it = attrInfo_.find(ATTR_CONV_KERNEL_H);
  if ((it != attrInfo_.end()) && (*it).second.as<IntImm>()) kernel_h = (*it).second.as<IntImm>()->value;

  it = attrInfo_.find(ATTR_CONV_KERNEL_W);
  if ((it != attrInfo_.end()) && (*it).second.as<IntImm>()) kernel_w = (*it).second.as<IntImm>()->value;

  it = attrInfo_.find(ATTR_CONV_TILE_H);
  if ((it != attrInfo_.end()) && (*it).second.as<IntImm>()) tileH = (*it).second.as<IntImm>()->value;

  it = attrInfo_.find(ATTR_CONV_TILE_W);
  if ((it != attrInfo_.end()) && (*it).second.as<IntImm>()) tileW = (*it).second.as<IntImm>()->value;

  it = attrInfo_.find(ATTR_CONV_PAD_LEFT);
  if ((it != attrInfo_.end()) && (*it).second.as<IntImm>()) padLeft = (*it).second.as<IntImm>()->value;

  it = attrInfo_.find(ATTR_CONV_PAD_TOP);
  if ((it != attrInfo_.end()) && (*it).second.as<IntImm>()) padTop = (*it).second.as<IntImm>()->value;

  int64_t wo = (tileW - kernel_w) / stride_w + 1;
  int64_t ho = (tileH - kernel_h) / stride_h + 1;

  isl::val v_s_h = isl::val(footprint.ctx(), stride_h);
  isl::val v_s_w = isl::val(footprint.ctx(), stride_w);
  isl::val v_w_o = isl::val(footprint.ctx(), wo);

  isl::set set_1 = v_aff_x[0].eq_set(v_aff_y[0]);
  isl::set set_2 = v_aff_x[1].eq_set(v_aff_y[2]);
  isl::aff aff_3 = (v_aff_y[1] / static_cast<int>(wo)).floor() * v_s_h + v_aff_y[3] - static_cast<int>(padTop);
  isl::aff aff_4 = v_aff_y[1].mod(v_w_o) * v_s_w + v_aff_y[4] - static_cast<int>(padLeft);
  isl::set set_3 = v_aff_x[2].eq_set(aff_3);
  isl::set set_4 = v_aff_x[3].eq_set(aff_4);
  isl::set set_5 = v_aff_x[4].eq_set(v_aff_y[5]);

  isl::val v_0 = isl::val(footprint.ctx(), 0);
  isl::val v_k_h = isl::val(footprint.ctx(), (kernel_h - 1));
  isl::val v_k_w = isl::val(footprint.ctx(), (kernel_w - 1));
  isl::val v_hw = isl::val(footprint.ctx(), (ho * wo - 1));

  isl::aff aff_v_0 = isl::aff(ls, v_0);
  isl::aff aff_k_h = isl::aff(ls, v_k_h);
  isl::aff aff_k_w = isl::aff(ls, v_k_w);
  isl::aff aff_v_hw = isl::aff(ls, v_hw);

  isl::set set_6 = v_aff_y[3].ge_set(aff_v_0);
  isl::set set_7 = v_aff_y[3].le_set(aff_k_h);
  isl::set set_8 = v_aff_y[4].ge_set(aff_v_0);
  isl::set set_9 = v_aff_y[4].le_set(aff_k_w);

  isl::set set_10 = v_aff_y[1].ge_set(aff_v_0);
  isl::set set_11 = v_aff_y[1].le_set(aff_v_hw);

  isl::set set = set_1.intersect(set_2)
                   .intersect(set_3)
                   .intersect(set_4)
                   .intersect(set_5)
                   .intersect(set_6)
                   .intersect(set_7)
                   .intersect(set_8)
                   .intersect(set_9)
                   .intersect(set_10)
                   .intersect(set_11);
  footprint = set.unwrap();
  CHECK(attrInfo_[ATTR_CONV_FEATURE_NAME].as<StringImm>());
  isl::id im2colId = isl::id(original_map.ctx(), attrInfo_[ATTR_CONV_FEATURE_NAME].as<StringImm>()->value);
  footprint = footprint.set_tuple_id(isl_dim_out, im2colId);
}

isl::map WeightAffine::ConstructAffine(const isl::map original_map) {
  // space:: S -> O
  isl::space original_space = original_map.get_space();

  // MA:: [S -> O] -> O
  auto original_space_inserter = isl::multi_aff::range_map(original_space);

  isl::map footprint = isl::map(original_space_inserter);

  // map:: O -> O
  footprint = footprint.curry().range().unwrap();

  isl_map *p_footprint = footprint.copy();

  int n_in = isl_map_dim(p_footprint, isl_dim_in);
  int n_out = isl_map_dim(p_footprint, isl_dim_out);

  footprint = isl::manage(p_footprint);

  auto footprint_space = footprint.get_space();

  auto p_s = footprint_space.wrap();
  auto ls = isl::local_space(p_s);

  std::vector<isl::aff> v_aff_x;
  CHECK_GE(n_in, 0);
  for (int i = 0; i < n_in; ++i) {
    isl::aff aff_i;
    aff_i = aff_i.var_on_domain(ls, isl_dim_out, i);
    v_aff_x.push_back(aff_i);
  }

  std::vector<isl::aff> v_aff_y;
  CHECK_GE(n_out, 0);
  for (int i = 0; i < n_out; ++i) {
    isl::aff aff_i;
    aff_i = aff_i.var_on_domain(ls, isl_dim_out, n_in + i);
    v_aff_y.push_back(aff_i);
  }

  CHECK(v_aff_x.size() == v_aff_y.size());
  CHECK_GE(v_aff_x.size(), 4);
  int64_t kh = 0;
  int64_t kw = 0;
  auto it = attrInfo_.find(ATTR_CONV_KERNEL_H);
  if ((it != attrInfo_.end()) && (*it).second.as<IntImm>()) kh = (*it).second.as<IntImm>()->value;

  it = attrInfo_.find(ATTR_CONV_KERNEL_W);
  if ((it != attrInfo_.end()) && (*it).second.as<IntImm>()) kw = (*it).second.as<IntImm>()->value;

  isl::set set_0 = v_aff_x[0].eq_set(static_cast<int>(kh) - 1 - v_aff_y[0]);
  isl::set set_1 = v_aff_x[1].eq_set(static_cast<int>(kw) - 1 - v_aff_y[1]);
  isl::set set_2 = v_aff_x[2].eq_set(v_aff_y[3]);
  isl::set set_3 = v_aff_x[3].eq_set(v_aff_y[2]);

  isl::set set = set_0.intersect(set_1).intersect(set_2).intersect(set_3);
  footprint = set.unwrap();
  return footprint;
}

isl::map FractalAffine::ConstructAffine(const isl::map original_map) {
  // space:: S -> O
  isl::space original_space = original_map.get_space();

  // MA:: [S -> O] -> O
  auto original_space_inserter = isl::multi_aff::range_map(original_space);

  isl::map footprint = isl::map(original_space_inserter);

  // map:: O -> O
  footprint = footprint.curry().range().unwrap();

  isl_map *p_footprint = footprint.copy();
  p_footprint = isl_map_add_dims(p_footprint, isl_dim_in, 1);

  int n_in = isl_map_dim(p_footprint, isl_dim_in);
  int n_out = isl_map_dim(p_footprint, isl_dim_out);

  CHECK_GE(n_in, 0);
  for (int i = 0; i < n_in; ++i) {
    std::string arg = "arg" + std::to_string(i) + "'";
    p_footprint = isl_map_set_dim_name(p_footprint, isl_dim_in, i, arg.c_str());
  }
  CHECK(attrInfo_[ATTR_CONV_FEATURE_NAME].as<StringImm>());
  p_footprint =
    isl_map_set_tuple_name(p_footprint, isl_dim_in, attrInfo_[ATTR_CONV_FEATURE_NAME].as<StringImm>()->value.c_str());
  CHECK_GE(n_out, 0);
  for (int i = 0; i < n_out; ++i) {
    std::string arg = "arg" + std::to_string(i) + "''";
    p_footprint = isl_map_set_dim_name(p_footprint, isl_dim_out, i, arg.c_str());
  }
  footprint = isl::manage(p_footprint);

  auto footprint_space = footprint.get_space();

  auto p_s = footprint_space.wrap();
  auto ls = isl::local_space(p_s);

  std::vector<isl::aff> v_aff_x;
  for (int i = 0; i < n_in; ++i) {
    isl::aff aff_i;
    aff_i = aff_i.var_on_domain(ls, isl_dim_out, i);
    v_aff_x.push_back(aff_i);
  }

  std::vector<isl::aff> v_aff_y;
  for (int i = 0; i < n_out; ++i) {
    isl::aff aff_i;
    aff_i = aff_i.var_on_domain(ls, isl_dim_out, n_in + i);
    v_aff_y.push_back(aff_i);
  }

  CHECK_GE(v_aff_x.size(), 6);
  CHECK_GE(v_aff_y.size(), 5);

  ConstructAffineMap(footprint, v_aff_x, v_aff_y, original_map, ls);
  return footprint;
}

void FractalAffine::ConstructAffineMap(isl::map &footprint, std::vector<isl::aff> &v_aff_x,
                                       std::vector<isl::aff> &v_aff_y, const isl::map &original_map,
                                       isl::local_space &ls) {
  /* construct affine map */
  int64_t block_size = 16;
  isl::val v_b_s = isl::val(footprint.ctx(), block_size);

  isl::set set_0 = v_aff_y[0].eq_set(v_aff_x[0]);
  isl::aff aff_1 = (v_aff_x[1] / static_cast<int>(block_size)).floor();
  isl::set set_1 = v_aff_y[1].eq_set(aff_1);

  int64_t k_h = 0;
  int64_t k_w = 0;
  auto it = attrInfo_.find(ATTR_CONV_KERNEL_H);
  if ((it != attrInfo_.end()) && (*it).second.as<IntImm>()) k_h = (*it).second.as<IntImm>()->value;

  it = attrInfo_.find(ATTR_CONV_KERNEL_W);
  if ((it != attrInfo_.end()) && (*it).second.as<IntImm>()) k_w = (*it).second.as<IntImm>()->value;

  isl::val v_k_w = isl::val(footprint.ctx(), (k_w));
  isl::val v_k_hw = isl::val(footprint.ctx(), (k_h * k_w));

  isl::aff aff_k_hw = isl::aff(ls, v_k_hw);
  isl::aff aff_2_1 = v_aff_x[2].mul(aff_k_hw);

  isl::aff aff_k_w = isl::aff(ls, v_k_w);
  isl::aff aff_2_2 = v_aff_x[3].mul(aff_k_w);

  isl::aff aff_2 = aff_2_1.add(aff_2_2);
  aff_2 = aff_2.add(v_aff_x[4]);

  isl::set set_2 = v_aff_y[2].eq_set(aff_2);

  isl::aff aff_3 = v_aff_x[1].mod(v_b_s);
  isl::set set_3 = v_aff_y[3].eq_set(aff_3);
  isl::set set_4 = v_aff_y[4].eq_set(v_aff_x[5]);

  isl::set set = set_0.intersect(set_1).intersect(set_2).intersect(set_3).intersect(set_4);

  footprint = set.unwrap();
  CHECK(attrInfo_[ATTR_CONV_FEATURE_NAME].as<StringImm>());
  isl::id fractalId = isl::id(original_map.ctx(), attrInfo_[ATTR_CONV_FEATURE_NAME].as<StringImm>()->value);
  footprint = footprint.set_tuple_id(isl_dim_out, fractalId);
}

void AffineRefGroupConstructor::create() {
  switch (type_) {
    case AffineType::AFFINE_GEMM:
      affine_ = new GemmTransposeAffine();
      break;
    case AffineType::AFFINE_GEMMBLOCK:
      affine_ = new GemmTransposeBlockAffine();
      break;
    case AffineType::AFFINE_GEMMBLOCKIN:
      affine_ = new GemmInnerTransposeAffine();
      break;
    case AffineType::AFFINE_IM2COL:
      affine_ = new Im2colAffine();
      break;
    case AffineType::AFFINE_WEIGHTTRANS:
      affine_ = new WeightAffine();
      break;
    case AffineType::AFFINE_FRACTAL:
      affine_ = new FractalAffine();
      break;
    default:
      affine_ = nullptr;
  }
}

std::unique_ptr<TensorFootprintCluster> AffineRefGroupConstructor::ConstructRefGroup(Scop &scop,
                                                                                     const isl::union_map &accesses,
                                                                                     const isl::union_set &domain,
                                                                                     const isl::union_map &schedule,
                                                                                     ReferenceType type) {
  for (auto a : accesses.get_map_list()) {
    auto tensor_id = a.get_tuple_id(isl_dim_out);
    // filter out tensor
    if (affine_->NotNeedConstruct(tensor_id.get_name(), scop)) {
      continue;
    }

    if (isl::union_map(a.curry()).intersect_domain(domain).is_empty()) {
      continue;
    }
    return ConstructAffineMapFootprintCluster(schedule, a, type, true);
  }
  return nullptr;
}

std::unique_ptr<TensorFootprintCluster> AffineRefGroupConstructor::ConstructAffineMapFootprintCluster(
  const isl::union_map &schedule, const isl::map &access, ReferenceType type, bool need_dma) {
  if (type_ == AffineType::AFFINE_FRACTAL) {
    return FractalAffineMapFootprintCluster(schedule, access, type, need_dma);
  }

  return AffineMapFootprintCluster(schedule, access, type, need_dma);
}

std::unique_ptr<TensorFootprintCluster> AffineRefGroupConstructor::FractalAffineMapFootprintCluster(
  const isl::union_map &schedule, const isl::map &access, ReferenceType type, bool need_dma) {
  auto scoped_access = GetScopedAccess(schedule, access);
  std::unique_ptr<TensorFootprintCluster> rg_C1 =
    TensorFootprintCluster::ComputeFootprintCluster(access, scoped_access, type, need_dma);

  Im2colAffine im2col;
  auto fracAffine = static_cast<FractalAffine *>(affine_);
  if (fracAffine != nullptr) {
    im2col.attrInfo_ = fracAffine->attrInfo_;
  }
  isl::map im2colMap = im2col.ConstructAffine(scoped_access.domain_factor_domain());
  isl::map fractalMap = affine_->ConstructAffine(scoped_access.domain_factor_domain());

  scoped_access = scoped_access.apply_range(im2colMap);
  scoped_access = scoped_access.apply_range(fractalMap);

  std::unique_ptr<TensorFootprintCluster> tensorGroup =
    TensorFootprintCluster::ComputeFootprintCluster(access, scoped_access, type, need_dma);

  isl::map l1footprint = isl::map(rg_C1->ComputeBufferedFootprints());
  l1footprint = l1footprint.apply_range(im2colMap);
  tensorGroup->footprint_map_ = l1footprint.apply_range(fractalMap);

  return tensorGroup;
}

std::unique_ptr<TensorFootprintCluster> AffineRefGroupConstructor::AffineMapFootprintCluster(
  const isl::union_map &schedule, const isl::map &access, ReferenceType type, bool need_dma) {
  auto scoped_access = GetScopedAccess(schedule, access);
  std::unique_ptr<TensorFootprintCluster> rg_C1 =
    TensorFootprintCluster::ComputeFootprintCluster(access, scoped_access, type, need_dma);
  isl::map affineMap = affine_->ConstructAffine(scoped_access.domain_factor_domain());
  scoped_access = scoped_access.apply_range(affineMap);

  std::unique_ptr<TensorFootprintCluster> tensorGroup =
    TensorFootprintCluster::ComputeFootprintCluster(access, scoped_access, type, need_dma);
  auto l1footprint = isl::map(rg_C1->ComputeBufferedFootprints());
  tensorGroup->footprint_map_ = l1footprint.apply_range(affineMap);

  return tensorGroup;
}

std::unique_ptr<TensorFootprintCluster> ConstructAffineFpCluster(Scop &scop, const isl::union_map &accesses,
                                                                 const isl::union_set &domain,
                                                                 const isl::union_map &schedule, ReferenceType type,
                                                                 AffineType affine_type, AffineTensor right_matrix) {
  AffineRefGroupConstructor constructor(affine_type);
  constructor.create();

  switch (affine_type) {
    case AffineType::AFFINE_GEMM: {
      auto affine = static_cast<GemmTransposeAffine *>(constructor.affine_);
      if (affine != nullptr) {
        affine->SetRightMatrix(right_matrix);
      }
    } break;
    case AffineType::AFFINE_GEMMBLOCK: {
      auto affine = static_cast<GemmTransposeBlockAffine *>(constructor.affine_);
      if (affine != nullptr) {
        affine->SetRightMatrix(right_matrix);
      }
    } break;
    case AffineType::AFFINE_GEMMBLOCKIN: {
      auto affine = static_cast<GemmInnerTransposeAffine *>(constructor.affine_);
      if (affine != nullptr) {
        affine->SetRightMatrix(right_matrix);
      }
      break;
    }
    case AffineType::AFFINE_IM2COL: {
      auto affine = static_cast<Im2colAffine *>(constructor.affine_);
      if (affine != nullptr) {
        affine->attrInfo_ = scop.attr_info_;
      }
    } break;
    case AffineType::AFFINE_WEIGHTTRANS: {
      auto affine = static_cast<WeightAffine *>(constructor.affine_);
      if (affine != nullptr) {
        affine->attrInfo_ = scop.attr_info_;
      }
    } break;
    case AffineType::AFFINE_FRACTAL: {
      auto affine = static_cast<FractalAffine *>(constructor.affine_);
      if (affine != nullptr) {
        affine->attrInfo_ = scop.attr_info_;
      }
    } break;
    default:
      break;
  }

  return constructor.ConstructRefGroup(scop, accesses, domain, schedule, type);
}

void AddAllBufferFootprintOfTensor(const Scop &scop, const isl::id &tensor_id,
                                   std::unordered_set<isl::id, isl::IslIdIslHash> &buffered_tensors) {
  buffered_tensors.insert(tensor_id);
  for (const auto &info : scop.buffer_def_infos_) {
    if (info.dst_tensor_id == tensor_id) {
      buffered_tensors.insert(info.ancester_tensor_id);
    }
  }
}

std::unordered_set<isl::id, isl::IslIdIslHash> GatherStatementsInSubtree(const isl::schedule_node &tree) {
  std::unordered_set<isl::id, isl::IslIdIslHash> statements;
  auto gather_statements = [&](const isl::schedule_node &node) -> bool {
    if (tree.isa<isl::schedule_node_filter>()) {
      isl::union_set filter = tree.as<isl::schedule_node_filter>().get_filter();
      filter.foreach_set([&](const isl::set &set) -> void { statements.insert(set.get_tuple_id()); });
      return false;  // no need to descend
    }
    if (node.isa<isl::schedule_node_band>()) {
      auto band = node.as<isl::schedule_node_band>();
      band.get_partial_schedule_union_map().foreach_map(
        [&](const isl::map &map) -> void { statements.insert(map.get_tuple_id(isl_dim_in)); });
    }
    return true;  // descend into children nodes
  };
  tree.foreach_descendant_top_down(gather_statements);
  return statements;
}

bool IsExtensionUsedInSubTree(const Scop &scop, const isl::schedule_node &tree, const isl::union_map &extension,
                              const isl::union_map &accesses) {
  auto statements = GatherStatementsInSubtree(tree);

  std::unordered_set<isl::id, isl::IslIdIslHash> promoted_tensors;
  extension.foreach_map([&](const isl::map &footprint) -> void {
    if (!footprint.range().is_wrapping()) return;
    const isl::id &tensor_id = footprint.range().unwrap().domain().unwrap().get_tuple_id(isl_dim_out);
    AddAllBufferFootprintOfTensor(scop, tensor_id, promoted_tensors);
  });

  bool found_extension_in_subtree = false;
  accesses.foreach_map([&](const isl::map &access) -> void {
    const isl::id &access_tensor_id = access.get_tuple_id(isl_dim_out);
    if (promoted_tensors.count(access_tensor_id) > 0) {
      const isl::id &statement_id = access.domain().unwrap().get_tuple_id(isl_dim_in);
      if (statements.count(statement_id) > 0) found_extension_in_subtree = true;
    }
  });

  return found_extension_in_subtree;
}

isl::schedule_node InsertExtensionHere(isl::schedule_node &tree, const isl::schedule_node &graft, isl_bool before) {
  if (before) {
    tree = tree.graft_before(graft);
  } else {
    tree = tree.graft_after(graft);
  }
  const int level_distance_from_original_pos = 3;
  return tree.ancestor(level_distance_from_original_pos);
}

/*
 * Insert the extension to the filters that access the promoted tensors, and remove redundant extensions.
 * If the extension is the first filter that access the promoted tensor, the extension is needed.
 * Otherwise, we compare the partial schedule of this filter and the last promoted tensor.
 * If they have the same range, then they will be in a same tile, and the footprint can be reused.
 * Otherwise, a new extension needs to be inserted.
 */
isl::schedule_node InsertExtensionToFirstAccessedFilters(const Scop &scop, isl::schedule_node &tree,
                                                         const isl::union_map &extension,
                                                         const isl::schedule_node &graft, isl_bool before,
                                                         bool &found_extension_in_schedule) {
  found_extension_in_schedule = false;
  if (scop.IsConv() || !tree.isa<isl::schedule_node_sequence>()) {
    return tree;
  }

  isl::union_map accesses = scop.data_.reads.unite(scop.data_.writes);
  isl::union_set last_schedule_range;

  unsigned int n_children = tree.n_children();
  for (unsigned int i = 0; i < n_children; ++i) {
    unsigned int child_idx = before ? i : n_children - 1 - i;
    if (IsExtensionUsedInSubTree(scop, tree.get_child(child_idx), extension, accesses)) {
      tree = tree.child(child_idx).child(0);

      bool insert_here = false;
      bool is_first = (!found_extension_in_schedule);
      isl::union_map partial_schedule = ShortSchedule(tree);
      isl::union_set schedule_range = partial_schedule.range();

      if (is_first) {
        insert_here = true;
      } else if (!schedule_range.is_subset(last_schedule_range)) {
        insert_here = true;
      }

      if (insert_here) {
        found_extension_in_schedule = true;
        last_schedule_range = schedule_range;
        tree = InsertExtensionHere(tree, graft, before);
      }

      tree = tree.parent().parent();
    }
  }
  return tree;
}

/*
 * Insert extension before or after the entire sequence node.
 * This should be used when we cannot determine the filter that access the promoted tensors, so
 * we have to be conservative.
 *
 * The schedule tree will look like:
 *
 * sequence:
 * - filter: GM -> BUF copy1
 * - filter: GM -> BUF copy2
 * - sequence:
 *   - compute1
 *   - compute2
 * - filter: BUF -> GM copy
 */
isl::schedule_node DefaultInsertExtension(isl::schedule_node tree, const isl::schedule_node &graft, isl_bool before,
                                          int original_sequence_index) {
  if (before) {
    tree = tree.graft_before(graft);
  } else {
    tree = tree.graft_after(graft);
  }
  const int level_distance_from_original_pos = 2;
  if (isl_bool_true == before) ++original_sequence_index;
  tree = tree.ancestor(level_distance_from_original_pos).child(original_sequence_index).child(0);
  return tree;
}

/*
 * Construct an extension node from "extension" and "schedule", and insert it into the specified position
 * in schedule tree. "before" param indicates before or after the specified position.
 * The specified position should be a sequence node. The extension will be inserted into the closest filter
 * before the first access (or after the last access). It extracts reads and writes information from scop.
 *
 * Example:
 *   sequence:
 *   - filter1: S_0[i0] (reads input_1)
 *   - filter2: S_1[i0] (reads input_2)
 *
 * After inserting extension that promotes input_2:
 *   sequence:
 *   - filter1: S_0[i0]
 *   - filter2: S_1[i0]
 *     child:
 *       extension: { [i0] -> GMread[[[i0] -> input_2[arg0 = i1]] -> input_2_local_BUF[arg0' = arg0]]: i0 <= 1000 }
 *       child:
 *         sequence:
 *         - filter: { [i0] -> GMread[[[i0] -> input_2[arg0 = i1]] -> input_2_local_BUF[arg0' = arg0]] }
 *           child:
 *             schedule: ...
 *         - filter: S_1[i0]
 *           ... (original schedule)
 */
isl::schedule_node InsertExtensionBeforeOrAfter(const Scop &scop, isl::schedule_node tree,
                                                const isl::union_map &extension,
                                                const isl::multi_union_pw_aff &schedule, isl_bool before) {
  if (tree.isa<isl::schedule_node_filter>() && tree.parent().isa<isl::schedule_node_sequence>()) {
    tree = tree.parent();
  }

  if (tree.isa<isl::schedule_node_extension>()) {
    tree = tree.child(0);
    for (unsigned int index = 0; index < tree.n_children(); ++index) {
      isl::schedule_node_filter child = tree.child(index).as<isl::schedule_node_filter>();
      bool isuser =
        child.get_filter().every_set([](const isl::set &s) -> bool { return s.get_tuple_name() != "C1read"; });
      if (isuser) {
        tree = child.child(0);
        break;
      }
    }
  }

  if (!tree.isa<isl::schedule_node_sequence>()) {
    tree = tree.insert_sequence(isl::union_set_list(tree.get_universe_domain()));
  }

  CHECK(tree.isa<isl::schedule_node_sequence>()) << "extension must be inserted into a sequence node";

  isl::schedule_node graft = isl::schedule_node::from_extension(extension);
  graft = graft.child(0).insert_partial_schedule(schedule).parent();

  int index = tree.parent().get_ancestor_child_position(tree.ancestor(2));

  if (tree.parent().isa<isl::schedule_node_filter>()) {
    if (isl_bool_true == before) {
      tree = tree.ancestor(2).child(0).child(0);
    } else {
      int size = tree.ancestor(2).n_children();
      tree = tree.ancestor(2).child(size - 1).child(0);
    }
  }

  bool found_extension_in_schedule = false;
  tree = InsertExtensionToFirstAccessedFilters(scop, tree, extension, graft, before, found_extension_in_schedule);

  if (found_extension_in_schedule) {
    return tree;
  } else {
    return DefaultInsertExtension(tree, graft, before, index);
  }
}

static std::string MemTypeToString(const MemType &memType) {
  switch (memType) {
    case MemType::BUF_:
      return "BUF";
    case MemType::C1_:
      return "C1";
    case MemType::BUF_C0_:
      return "BUFC0";
    case MemType::BUF_C1_:
      return "BUFC1";
    case MemType::C0A_:
      return "C0A";
    case MemType::C0B_:
      return "C0B";
    case MemType::C0C_:
      return "C0C";
    case MemType::DDR:
      return "GM";
    default:
      return "";
  }
}

static std::string GetIslReadName(Scop &scop, const isl::id &cluster_id) {
  auto tensor_info = scop.GetBufferDefInfo(cluster_id);
  MemType memType = tensor_info.SrcMemType();
  return MemTypeToString(memType) + "read";
}

static std::string GetIslWriteName(Scop &scop, const isl::id &cluster_id) {
  if (scop.HasBufferDefInfo(cluster_id)) {
    auto tensor_info = scop.GetBufferDefInfo(cluster_id);
    MemType memType = tensor_info.DstMemType();
    return MemTypeToString(memType) + "write";
  }
  return MemTypeToString(MemType::DDR) + "write";
}

isl::schedule_node PlaceIm2colBelowImpl(Scop &scop, isl::schedule_node tree, const TensorFootprintCluster &cluster,
                                        const isl::map &footprint, const isl::set &original_elements,
                                        const isl::set &read_set) {
  bool reads = (!cluster.RichReadRelations().is_empty() && cluster.ReadNeedDma());
  if (reads) {
    auto cluster_id = footprint.get_tuple_id(isl_dim_out);
    auto buffered_footprint = cluster.BufferedFootprint().set_tuple_id(cluster_id);
    auto buffered_universe = isl::set::universe(footprint.get_space().domain().unwrap().domain());
    auto array_id = footprint.get_space().domain().unwrap().get_tuple_id(isl_dim_out);
    auto buffered_read = isl::map(buffered_universe, read_set.set_tuple_id(array_id).intersect(original_elements))
                           .wrap()
                           .product(buffered_footprint);
    auto fp_space_identity = isl::multi_aff::identity(footprint.get_space().range().map_from_set());
    auto buffer_def = scop.GetBufferDefInfo(cluster_id);
    fp_space_identity = RemoveDimensionOfSizeOne(fp_space_identity, buffer_def.TensorSize(tree.parent()));
    auto extension_map = footprint.wrap().identity().domain_factor_domain().domain_factor_domain();
    isl::id read_id = isl::id(tree.ctx(), GetIslReadName(scop, cluster_id));
    auto read_extension = extension_map.intersect_range(buffered_read).set_tuple_id(isl_dim_out, read_id);
    auto read_mupa = isl::multi_union_pw_aff(fp_space_identity.pullback(
      isl::multi_aff::wrapped_range_map(footprint.get_space().wrap().set_set_tuple_id(read_id))));
    tree = InsertExtensionBeforeOrAfter(scop, tree.get_child(0), read_extension, read_mupa, isl_bool_true);
  }
  scop.schedule_ = tree.get_schedule();
  return tree;
}

void UpdateTensorShape(Scop &scop, const isl::map &read_extension) {
  ScopedFootprint foot_print = ComputeFootprintOfRange(read_extension.domain_factor_domain());
  if (!foot_print.box.is_valid()) {
    return;
  }
  isl::id cluster_id = isl::id(read_extension.ctx(), read_extension.get_tuple_id(isl_dim_out).get_name() + LOCAL_BUF);
  std::vector<size_t> shape;
  shape.reserve(foot_print.GetBoxDim());
  for (const auto &size : foot_print.box.get_size().get_val_list()) {
    shape.push_back(size.get_num_si());
  }
  static_cast<void>(scop.UpdateBufferDefInfoSizes(cluster_id, shape));
}

isl::schedule_node InsertStmtExtension(Scop &scop, isl::schedule_node tree, isl::map read, isl::map read_extension,
                                       const isl::union_map &raw_reads, const isl::union_map &raw_writes,
                                       const isl::union_map &raw_copyin, const isl::union_map &schedule,
                                       BufferDefInfo &def) {
  isl::union_map reads = isl::union_map(read);
  isl::union_map writes = raw_writes.intersect_range(reads.range());
  isl::union_map dependence = DependenceAnalysis(writes, reads, writes, schedule);
  isl::union_set stmt = dependence.domain().universe();
  writes = raw_writes.intersect_domain(stmt);
  UpdateTensorShape(scop, read_extension);

  /* get stmt extension */
  isl::union_map stmt_ext = isl::union_map(read_extension);
  stmt_ext = stmt_ext.apply_range(writes.reverse().polyhedral_hull());
  stmt_ext = stmt_ext.polyhedral_hull();

  std::map<unsigned int, const isl::map> stmt_ext_map;
  stmt_ext.foreach_map([&](const isl::map &m) -> void {
    std::string name = m.range().get_tuple_name();
    unsigned int index = WrappedStrtol(name.substr(name.find('_') + 1, name.length() - 1));
    stmt_ext_map.insert(std::make_pair(index, m));
  });

  for (auto it = stmt_ext_map.rbegin(); it != stmt_ext_map.rend(); ++it) {
    isl::map stmt_extension = isl::map::from(it->second);
    stmt_extension = stmt_extension.domain_factor_domain();

    /* get schedule */
    isl::space stmtSpace = stmt_extension.get_space().range();
    isl::multi_aff identity_copy_schedule = isl::multi_aff::identity(stmtSpace.map_from_set());
    identity_copy_schedule = RemoveDimensionOfSizeOne(identity_copy_schedule, def.TensorSize(tree.parent()));
    isl::multi_union_pw_aff stmtSchedule = isl::multi_union_pw_aff(identity_copy_schedule);
    /* insert extension node */
    tree = InsertExtensionBeforeOrAfter(scop, tree.get_child(0), stmt_extension, stmtSchedule, isl_bool_true);
  }

  /* next */
  reads = raw_reads.intersect_domain(stmt);
  reads = reads.subtract(raw_copyin);
  if (!reads.is_empty()) {
    isl::union_map relation = writes.reverse().apply_range(reads);
    isl::union_map readExt = isl::union_map(read_extension);
    readExt = readExt.apply_range(relation);
    isl::map_list readList = reads.get_map_list();
    int n = readList.size();
    for (int i = 0; i < n; ++i) {
      read = readList.get_at(i);
      readExt = readExt.intersect_range(isl::union_set(read.range()));
      read_extension = isl::map::from(readExt);
      tree = InsertStmtExtension(scop, tree, read, read_extension, raw_reads, raw_writes, raw_copyin, schedule, def);
    }
  }
  return tree;
}

void CheckOutOfBoundAccess(const isl::map &access_elements, const isl::set &original_elements,
                           const std::string &access_type) {
  isl::set complementOriginalElements = isl::set::universe(original_elements.get_space()).subtract(original_elements);
  isl::map outOfBoundAccess = access_elements.intersect_range(complementOriginalElements);
  if (!outOfBoundAccess.is_empty()) {
    if (outOfBoundAccess.is_equal(access_elements)) {
      LOG(WARNING) << "detected always out of bound " << access_type << " access: " << outOfBoundAccess << std::endl
                   << "Please check DSL and remove the corresponding statement. tensor shape: " << original_elements;
    } else {
      LOG(WARNING) << "detected possible out of bound " << access_type << " access: " << outOfBoundAccess << std::endl
                   << "tensor shape: " << original_elements;
    }
  }
}

void PlaceDataCopyBelowImplReadWrite(Scop &scop, isl::schedule_node &tree, const TensorFootprintCluster &cluster,
                                     const isl::map &footprint, const isl::id &tensor_id,
                                     const isl::set &original_elements, const isl::map &exact_writes,
                                     isl::map &read_extension, isl::set &buffered_footprint, const isl::id &cluster_id,
                                     isl::map &extension_map, isl::id &read_id) {
  bool reads = (!cluster.RichReadRelations().is_empty() && cluster.ReadNeedDma());
  bool writes = (!cluster.RichWriteRelations().is_empty() && cluster.WriteNeedDma());
  if (writes) {
    auto tensor_info = scop.GetBufferDefInfo(cluster_id);
    if (MemType::BUF_C0_ == tensor_info.DstMemType() || MemType::BUF_ == tensor_info.DstMemType() ||
        tensor_info.IsPreMmuC1Write()) {
      if (!scop.IsInBinds(tensor_id)) writes = false;
    }
    if (tensor_info.IsPreMmuC1Write()) {
      if (!scop.IsInBinds(tensor_id)) reads = false;
    }
  }

  auto fp_space_identity = isl::multi_aff::identity(footprint.get_space().range().map_from_set());
  auto buffer_def = scop.GetBufferDefInfo(cluster_id);
  fp_space_identity = RemoveDimensionOfSizeOne(fp_space_identity, buffer_def.TensorSize(tree.parent()));
  if (reads) {
    auto read_mupa = isl::multi_union_pw_aff(fp_space_identity.pullback(
      isl::multi_aff::wrapped_range_map(footprint.get_space().wrap().set_set_tuple_id(read_id))));
    tree = InsertExtensionBeforeOrAfter(scop, tree.get_child(0), read_extension, read_mupa, isl_bool_true);
  }
  if (writes) {
    isl::schedule_node tree_write = tree.get_child(0);
    if (scop.params_.empty() && scop.IsLoadIm2colC1BUF()) {
      tree_write = tree;
    }
    isl::set writes_set = exact_writes.intersect_range(original_elements).wrap().product(buffered_footprint);
    isl::id write_id = isl::id(tree.ctx(), GetIslWriteName(scop, tensor_id));
    isl::map write_extension = extension_map.intersect_range(writes_set).set_tuple_id(isl_dim_out, write_id);
    auto write_mupa = isl::multi_union_pw_aff(fp_space_identity.pullback(
      isl::multi_aff::wrapped_range_map(footprint.get_space().wrap().set_set_tuple_id(write_id))));
    tree = InsertExtensionBeforeOrAfter(scop, tree_write, write_extension, write_mupa, isl_bool_false);
  }
}

void PlaceDataCopyBelowImplFakeReads(Scop &scop, isl::schedule_node &tree, const TensorFootprintCluster &cluster,
                                     isl::map &read_extension, const isl::id &cluster_id) {
  auto buffer_def = scop.GetBufferDefInfo(cluster_id);
  bool fake_reads = (!cluster.RichReadRelations().is_empty() && cluster.ReadNeedDma() && cluster.ReadNeedExtension());
  if (fake_reads) {
    isl::schedule_node node = tree;
    while (!node.isa<isl::schedule_node_mark>() && !node.isa<isl::schedule_node_domain>()) {
      node = node.parent();
    }
    CHECK(node.isa<isl::schedule_node_mark>()) << "must find a mark node." << std::endl;
    auto tag = node.as<isl::schedule_node_mark>().get_id().get_name();
    if (tag == REALIZE_C1) {
      isl::map stmt_extension = read_extension.range().unwrap();
      isl::id stmt_tensor_id = cluster_id;
      size_t pos = cluster_id.get_name().find("_local_");
      if (pos != std::string::npos) {
        std::string substr = cluster_id.get_name().substr(0, pos);
        if (pos != 0) stmt_tensor_id = isl::id(stmt_tensor_id.ctx(), substr);
      }
      stmt_extension = stmt_extension.set_tuple_id(isl_dim_out, stmt_tensor_id);

      isl::union_set readTensor = isl::union_set(stmt_extension.range());
      isl::union_map reads_map = scop.data_.fake_copyin.domain_factor_domain().intersect_range(readTensor.universe());
      if (!reads_map.is_empty()) {
        isl::union_map raw_reads = scop.data_.reads.domain_factor_domain();
        isl::union_map raw_writes = scop.data_.writes.domain_factor_domain();
        isl::union_map raw_copyin = scop.data_.copyin.domain_factor_domain();

        isl::map_list readList = reads_map.get_map_list();
        int n = readList.size();
        for (int i = 0; i < n; ++i) {
          tree = InsertStmtExtension(scop, tree, readList.get_at(i), stmt_extension, raw_reads, raw_writes, raw_copyin,
                                     scop.sch_, buffer_def);
        }
      }
    }
  }
}

isl::schedule_node PlaceDataCopyBelowImpl(Scop &scop, isl::schedule_node tree, const TensorFootprintCluster &cluster,
                                          const isl::map &footprint, const isl::id &tensor_id,
                                          const isl::set &original_elements, const isl::map &exact_reads,
                                          const isl::map &exact_writes) {
  auto cluster_id = footprint.get_tuple_id(isl_dim_out);

  if (!scop.IsConv()) CheckOutOfBoundAccess(exact_reads, original_elements, "read");

  bool special_dma = false;
  if (scop.conv_special_dma_ || (scop.attr_info_.count(ATTR_CONV_SPECIAL_DMA) > 0)) {
    if (scop.attr_info_.count(ATTR_CONV_BACKPROP_FILTER) > 0 && scop.attr_info_.count(ATTR_CONV_KERNEL_H) > 0 &&
        scop.attr_info_.count(ATTR_CONV_KERNEL_W) > 0 && scop.attr_info_.count(ATTR_CONV_FEATURE_C) > 0) {
      std::string featureName = scop.ExtractStringFromAttrs(ATTR_CONV_FEATURE_NAME) + LOCAL_C1;
      int kh = scop.ExtractIntFromAttrs(ATTR_CONV_KERNEL_H);
      int kw = scop.ExtractIntFromAttrs(ATTR_CONV_KERNEL_W);
      int ci = scop.ExtractIntFromAttrs(ATTR_CONV_FEATURE_C);
      if (featureName == cluster_id.get_name() && kh == 7 && kw == 7 && ci == 16) {
        special_dma = true;
      }
    }
  }

  isl::set read_set;
  if (special_dma) {
    read_set = cluster.ExtractSingleAccessRelation().intersect_range(original_elements).wrap();
  } else {
    read_set = exact_reads.intersect_range(original_elements).wrap();
  }

  isl::set buffered_footprint = cluster.BufferedFootprint().set_tuple_id(cluster_id);
  read_set = read_set.product(buffered_footprint);

  isl::map extension_map = footprint.wrap().identity().domain_factor_domain().domain_factor_domain();
  isl::id read_id = isl::id(tree.ctx(), GetIslReadName(scop, cluster_id));
  isl::map read_extension = extension_map.intersect_range(read_set).set_tuple_id(isl_dim_out, read_id);
  if (special_dma) {
    isl::map read_set_map = read_extension.range().unwrap();
    isl_map *p_reads = read_set_map.copy();
    p_reads = isl_map_remove_divs(p_reads);
    p_reads = isl_map_drop_special_constraints(p_reads, 0, 2);
    read_set_map = isl::manage(p_reads);
    read_extension =
      read_set_map.wrap().identity().domain_factor_domain().domain_factor_domain().set_tuple_id(isl_dim_out, read_id);
  }
  if (!scop.IsConv()) CheckOutOfBoundAccess(exact_writes, original_elements, "write");

  PlaceDataCopyBelowImplReadWrite(scop, tree, cluster, footprint, tensor_id, original_elements, exact_writes,
                                  read_extension, buffered_footprint, cluster_id, extension_map, read_id);

  PlaceDataCopyBelowImplFakeReads(scop, tree, cluster, read_extension, cluster_id);

  scop.schedule_ = tree.get_schedule();
  return tree;
}

isl::schedule_node PlaceInnerDataCopyBelow(Scop &scop, const isl::schedule_node &tree,
                                           const TensorFootprintCluster &cluster,
                                           const TensorFootprintCluster &outer_scope_cluster, const isl::id &tensor_id,
                                           const isl::id &cluster_id, const isl::id &outer_scope_cluster_id) {
  // map :: [S -> O] -> P_inner
  isl::map inner_scope_footprint = isl::map(cluster.ComputeBufferedFootprints()).set_tuple_id(isl_dim_out, cluster_id);

  // map :: [S -> O] -> P_outer
  isl::map outer_scope_footprint =
    isl::map(outer_scope_cluster.ComputeBufferedFootprints()).set_tuple_id(isl_dim_out, outer_scope_cluster_id);

  isl::set outerScopeGroupFootprint = outer_scope_cluster.BufferedFootprint().set_tuple_id(outer_scope_cluster_id);

  // space :: S -> [O -> P]
  isl::space outerSpace = outer_scope_footprint.get_space().curry();
  isl::space innerSpace = inner_scope_footprint.get_space().curry();
  auto outer_scope_in_dims = isl_space_dim(outerSpace.get(), isl_dim_in);
  auto inner_scope_in_dims = isl_space_dim(innerSpace.get(), isl_dim_in);
  CHECK_GE(inner_scope_in_dims, outer_scope_in_dims);

  if (inner_scope_in_dims > outer_scope_in_dims) {
    outer_scope_footprint = outer_scope_footprint.curry();
    outer_scope_footprint = isl::manage(
      isl_map_add_dims(outer_scope_footprint.copy(), isl_dim_in, inner_scope_in_dims - outer_scope_in_dims));
    outer_scope_footprint = outer_scope_footprint.uncurry();
  }

  // map :: [S -> O] -> S
  auto domain_access_to_domain_map =
    isl::map(isl::multi_aff::domain_map(inner_scope_footprint.get_space().domain().unwrap()));

  // map :: [S -> O] -> [S -> P_outer]
  outer_scope_footprint = domain_access_to_domain_map.range_product(outer_scope_footprint);

  inner_scope_footprint = inner_scope_footprint.apply_domain(outer_scope_footprint);

  return PlaceDataCopyBelowImpl(scop, tree, cluster, inner_scope_footprint, tensor_id, outerScopeGroupFootprint,
                                cluster.RichReadRelations().wrap().apply(outer_scope_footprint).unwrap(),
                                cluster.RichWriteRelations().wrap().apply(outer_scope_footprint).unwrap());
}

isl::schedule_node PlaceIm2colBelow(Scop &scop, const isl::schedule_node &tree, const TensorFootprintCluster &cluster,
                                    const TensorFootprintCluster &outer_scope_cluster, const isl::id &cluster_id,
                                    const isl::id &outer_scope_cluster_id) {
  // map :: [S -> O] -> P_inner
  isl::map inner_scope_footprint = cluster.footprint_map_.set_tuple_id(isl_dim_out, cluster_id);

  // map :: [S -> O] -> P_outer
  isl::map outer_scope_footprint = outer_scope_cluster.footprint_map_.set_tuple_id(isl_dim_out, outer_scope_cluster_id);

  // space :: S -> [O -> P_outer]
  isl::space outerSpace = outer_scope_footprint.get_space().curry();
  isl::space innerSpace = inner_scope_footprint.get_space().curry();
  auto outer_scope_in_dims = isl_space_dim(outerSpace.get(), isl_dim_in);
  auto inner_scope_in_dims = isl_space_dim(innerSpace.get(), isl_dim_in);
  CHECK_GE(inner_scope_in_dims, outer_scope_in_dims);

  if (inner_scope_in_dims > outer_scope_in_dims) {
    outer_scope_footprint = outer_scope_footprint.curry();
    outer_scope_footprint = isl::manage(
      isl_map_add_dims(outer_scope_footprint.copy(), isl_dim_in, inner_scope_in_dims - outer_scope_in_dims));
    outer_scope_footprint = outer_scope_footprint.uncurry();
  }

  // map :: [S -> O] -> S
  auto domain_access_to_domain_map =
    isl::map(isl::multi_aff::domain_map(inner_scope_footprint.get_space().domain().unwrap()));

  // map :: [S -> O] -> [S -> P_outer]
  outer_scope_footprint = domain_access_to_domain_map.range_product(outer_scope_footprint);

  // map :: [S -> P_outer] -> P_inner
  inner_scope_footprint = inner_scope_footprint.apply_domain(outer_scope_footprint);
  return PlaceIm2colBelowImpl(scop, tree, cluster, inner_scope_footprint,
                              outer_scope_cluster.BufferedFootprint().set_tuple_id(outer_scope_cluster_id),
                              outer_scope_cluster.BufferedFootprint().set_tuple_id(outer_scope_cluster_id));
}

isl::schedule_node PlaceOuterDataCopyBelow(Scop &scop, const isl::schedule_node &tree,
                                           const TensorFootprintCluster &cluster, const isl::id &tensor_id,
                                           const isl::id &cluster_id) {
  CHECK(!cluster_id.is_null()) << "expected cluster id";
  auto tensor_elements = CollectTensorSet(scop, tensor_id);
  isl::map footprint;
  if (cluster.foot_print_.box.is_valid()) {
    footprint = isl::map(cluster.ComputeBufferedFootprints()).set_tuple_id(isl_dim_out, cluster_id);
  } else {
    footprint = isl::map(cluster.IdentityBufferFootprint()).set_tuple_id(isl_dim_out, cluster_id);
  }
  return PlaceDataCopyBelowImpl(scop, tree, cluster, footprint, tensor_id, tensor_elements, cluster.RichReadRelations(),
                                cluster.RichWriteRelations());
}

void UniteInterleavedReadsAndWrites(std::vector<std::unique_ptr<TensorFootprintCluster>> &clusters) {
  for (size_t i = 0; i < clusters.size(); ++i) {
    for (size_t j = i + 1; j < clusters.size(); ++j) {
      auto box_i = clusters[i].get()->foot_print_.box;
      auto box_j = clusters[j].get()->foot_print_.box;
      bool need_cluster = true;
      if (box_i.is_valid() && box_j.is_valid()) {
        bool is_same_box = box_i.get_space().get_tuple_id(isl_dim_out) == box_j.get_space().get_tuple_id(isl_dim_out);
        bool interleaved =
          !clusters[i]->ExtractSingleAccessRelation().intersect(clusters[j]->ExtractSingleAccessRelation()).is_empty();
        need_cluster = is_same_box || interleaved;
      }
      if (need_cluster) {
        clusters[i] = TensorFootprintCluster::ClusteringFootprints(std::move(clusters[i]), std::move(clusters[j]));
        clusters.erase(clusters.begin() + static_cast<int64_t>(j));
        --j;
      }
    }
  }
}

void CreateTensorFootprintClusters(TensorClusterInfo &tensor_info, const isl::id &target_tensor_id,
                                   const isl::union_map &accesses, const isl::union_map &copyin,
                                   const isl::union_map &fake_copyin, const isl::union_set &domain,
                                   const isl::union_map &schedule, ReferenceType type) {
  std::unordered_set<isl::id, isl::IslIdIslHash> unapproximatable;

  for (const auto &access : accesses.get_map_list()) {
    auto tensor_id = access.get_tuple_id(isl_dim_out);

    if (target_tensor_id.get_name() != tensor_id.get_name() || unapproximatable.count(tensor_id) != 0 ||
        isl::union_map(access.curry()).intersect_domain(domain).is_empty()) {
      continue;
    }

    auto IsRealRead = [&copyin, &access]() -> bool {
      for (auto b : copyin.get_map_list()) {
        auto ds_a = access.domain().get_space();
        auto ds_b = b.domain().get_space();
        if (ds_b.is_equal(ds_a)) {
          return true;
        }
      }
      return false;
    };

    auto IsFakeCopyin = [&fake_copyin, &access]() -> bool {
      for (auto b : fake_copyin.get_map_list()) {
        if (b.is_equal(access)) {
          return true;
        }
      }
      return false;
    };

    auto scoped_access = GetScopedAccess(schedule, access);
    bool need_dma = type == ReferenceType::Read ? IsRealRead() : true;
    bool need_extension = type == ReferenceType::Read ? IsFakeCopyin() : false;
    auto footprint_cluster =
      TensorFootprintCluster::ComputeFootprintCluster(access, scoped_access, type, need_dma, need_extension);

    if (footprint_cluster->foot_print_.box.is_valid()) {
      tensor_info.push_back(std::move(footprint_cluster));
    } else {
      unapproximatable.insert(tensor_id);
      LOG(INFO) << "access of tensor " << tensor_id << " is unapproximatable: " << access;
    }
  }
}

isl::multi_aff ComputeBufferFootprint(const isl::map &access, const ScopedFootprint &foot_print, bool with_strides,
                                      bool with_lower_bounds) {
  auto access_space = access.get_space();

  auto original_space_inserter = isl::multi_aff::domain_map(access_space);

  if (foot_print.GetBoxDim() == 0) {
    LOG(FATAL) << "get buffer footprint for scalars";
  }
  auto lower_bounds = foot_print.box.get_offset().pullback(original_space_inserter);
  auto offsets = foot_print.stride_offsets.pullback(original_space_inserter);

  isl::multi_aff original = isl::multi_aff::range_map(access_space);
  isl::multi_aff footprint = original - offsets;
  if (with_strides) {
    footprint = footprint.scale_down(foot_print.stride_values);
  }
  if (with_lower_bounds) {
    footprint = footprint - lower_bounds;
  }
  return footprint;
}

isl::multi_aff ComputeBufferFootprint(const isl::map &access, const ScopedFootprint &foot_print) {
  return ComputeBufferFootprint(access, foot_print, true, true);
}
using InvalidDimBitmap = std::vector<bool>;

/*
 * Example:
 * default_footprint = { [[i0, i1] -> reduce_1_4[arg0, arg1]] -> reduce_1_4[(3194 - i1 + arg0), (-i0 + arg1)] }
 * invalid_dims = [0]
 * return InvalidDimBitmap = [1] because (3194 - i1 + arg0) contains i1 but does not contain i0.
 */
static InvalidDimBitmap FindVarsInAffDims(const isl::multi_aff &default_footprint,
                                          const std::vector<int> &invalid_dims) {
  InvalidDimBitmap domain_invalid_dims;
  unsigned domain_n_dims = default_footprint.space().domain().unwrap().dim(isl_dim_in);
  domain_invalid_dims.resize(domain_n_dims, false);
  for (auto aff_dim : invalid_dims) {
    const isl::aff &aff = default_footprint.get_at(aff_dim);
    for (unsigned i = 0; i < domain_n_dims; ++i) {
      isl_val *coef_val = isl_aff_get_coefficient_val(aff.get(), isl_dim_in, i);
      int coef = isl_val_get_num_si(coef_val);
      static_cast<void>(isl_val_free(coef_val));
      if (coef != 0) domain_invalid_dims[i] = true;
    }
  }
  return domain_invalid_dims;
}

static InvalidDimBitmap FindLowerDimVars(const InvalidDimBitmap &dims, int &first_invalid_domain_dim) {
  InvalidDimBitmap lower_dims = dims;
  bool found = false;
  first_invalid_domain_dim = -1;
  for (unsigned i = 0; i < dims.size(); ++i) {
    if (dims[i]) {
      found = true;
      first_invalid_domain_dim = i;
    }
    if (found) lower_dims[i] = true;
  }
  return lower_dims;
}

/*
 * Example:
 * default_footprint = { [[i0, i1] -> reduce_1_4[arg0, arg1]] -> reduce_1_4[(3194 - i1 + arg0), (-i0 + arg1)] }
 * domain_dims = [0,1], i.e. {i1}
 * return result_aff_dims = [0] because (3194 - i1 + arg0) contains i1 but (-i0 + arg1) does not contain i1.
 */
static std::vector<int> FindAffDimsWithVars(const isl::multi_aff &default_footprint,
                                            const InvalidDimBitmap &domain_dims) {
  std::vector<int> result_aff_dims;
  unsigned domain_n_dims = domain_dims.size();
  unsigned n_affs = default_footprint.size();
  for (unsigned aff_dim = 0; aff_dim < n_affs; ++aff_dim) {
    const isl::aff &aff = default_footprint.get_at(aff_dim);
    bool found = false;
    for (unsigned i = 0; i < domain_n_dims; ++i) {
      if (!domain_dims[i]) continue;

      isl_val *coef_val = isl_aff_get_coefficient_val(aff.get(), isl_dim_in, i);
      int coef = isl_val_get_num_si(coef_val);
      static_cast<void>(isl_val_free(coef_val));
      if (coef != 0) {
        found = true;
        break;
      }
    }
    if (found) result_aff_dims.push_back(aff_dim);
  }
  return result_aff_dims;
}

/*
 * Expand invalid dims to the dims lower than the input dims.
 * The dim ordering is determined from default_footprint.
 * For example, { [[i0, i1, i2, i3] -> reduce_1_4[arg0, arg1, arg2, arg3]]
 *     -> reduce_1_4[(3194 - i1 + arg0), (-i0 + arg1), (-i2 + arg2), (arg3)] }
 * Invalid dims contain dim 0 at first, i.e. (3194 - i1 + arg0) is invalid.
 * 1. Find the input dims accessed by invalid dims: (3194 - i1 + arg0) accesses i1.
 * 2. Expand input dims to all lower dims: from {i1} to {i1, i2, i3}.
 * 3. Find the affs that access lower dims: because i2 is accessed by (-i2 + arg2), so dim 2 is also invalid.
 *    Dim 1 and dim 3 are not invalid because (-i0 + arg1) and (arg3) do not contain lower dims.
 */
std::vector<int> ExpandInvalidDims(const std::vector<int> &invalid_dims, const isl::multi_aff &default_footprint,
                                   int &first_invalid_domain_dim) {
  auto domain_invalid_dims = FindVarsInAffDims(default_footprint, invalid_dims);
  auto lower_dims = FindLowerDimVars(domain_invalid_dims, first_invalid_domain_dim);
  return FindAffDimsWithVars(default_footprint, lower_dims);
}

/*
 * Use identity footprint for all invalid dims.
 */
static isl::multi_aff SelectDimsBufferFootprint(const std::vector<int> &invalid_dims,
                                                const isl::multi_aff &default_footprint,
                                                const isl::multi_aff &invalid_footprint) {
  isl::multi_aff select_footprint = default_footprint;
  for (int dim : invalid_dims) {
    select_footprint = select_footprint.set_at(dim, invalid_footprint.get_at(dim));
  }
  return select_footprint;
}

isl::multi_aff TensorFootprintCluster::ComputeBufferedFootprints(bool with_strides, bool with_lower_bounds) const {
  return ComputeBufferFootprint(RichAccessRelations(), foot_print_, with_strides, with_lower_bounds);
}

isl::multi_aff TensorFootprintCluster::ComputeBufferedFootprints() const {
  return foot_print_.is_valid
           ? ComputeBufferedFootprints(true, true)
           : SelectDimsBufferFootprint(foot_print_.invalid_dims, ComputeBufferedFootprints(true, true),
                                       IdentityBufferFootprint());
}

isl::multi_aff TensorFootprintCluster::IdentityBufferFootprint() const {
  return ComputeBufferedFootprints(false, false);
}

/*
 * Example:
 * default footprint:
 *  { [[i0, i1, i2, i3, i4] -> input_1[arg0, arg1, arg2, arg3, arg4]]
 *  -> input_1[(32 - i2 + arg0), (-i0 + arg1), (64 - i3 + arg2), (arg3), (arg4)] }
 *
 * unmerged footprint: (computed only using per-access information)
 *  { [[i0, i1, i2, i3, i4] -> input_1[arg0, arg1, arg2, arg3, arg4]]
 *  -> input_1[(-i2 + arg0), (-i0 + arg1), (-i3 + arg2), (64 - 56i4 + arg3), (arg4)] }
 *
 * return unshifted footprint:
 *  { [[i0, i1, i2, i3, i4] -> input_1[arg0, arg1, arg2, arg3, arg4]]
 *  -> input_1[(-i2 + arg0), (-i0 + arg1), (-i3 + arg2), (arg3), (arg4)] }
 *
 * For each dim, if the default and unmerged footprint only differs by a constant, then
 * use unmerged footprint; otherwise, use default footprint.
 */
isl::multi_aff TensorFootprintCluster::UnshiftedBufferFootprint(const isl::multi_aff &default_footprint,
                                                                const isl::id &fp_id) const {
  for (const auto &footprint : tensor_foot_prints) {
    if (footprint->id == fp_id) {
      ScopedFootprint unmerged_box = ComputeFootprintOfRange(footprint->scoped_access);
      bool with_strides = unmerged_box.is_valid;
      bool with_lower_bounds = with_strides;
      isl::multi_aff new_buf_fp =
        ComputeBufferFootprint(footprint->scoped_access, unmerged_box, with_strides, with_lower_bounds);
      isl::multi_aff diff = new_buf_fp.sub(default_footprint);
      for (unsigned dim = 0; dim < diff.size(); ++dim) {
        if (!IsAffNonZeroConst(diff.get_at(dim))) {
          new_buf_fp = new_buf_fp.set_at(dim, default_footprint.get_at(dim));
        }
      }
      return new_buf_fp;
    }
  }
  LOG(WARNING) << "footprint not found for " << fp_id << ", fall back to traditional buffer footprint";
  return default_footprint;
}

/*
 * Return buffer footprint cluster if tensor target_id is accessed in the outer_schedule.
 * Return nullptr if not accessed.
 */
std::unique_ptr<TensorFootprintCluster> TensorFootprintCluster::HoistBufferFootprintCluster(
  const isl::union_map &outer_schedule, const isl::id &target_id, const isl::union_map &reads,
  const isl::union_map &copyin, const isl::union_map &writes, const isl::union_map &fake_copyin) {
  TensorClusterInfo tensor_info;

  auto domain = outer_schedule.domain();

  CreateTensorFootprintClusters(tensor_info, target_id, writes, copyin, fake_copyin, domain, outer_schedule,
                                ReferenceType::Write);
  CreateTensorFootprintClusters(tensor_info, target_id, reads, copyin, fake_copyin, domain, outer_schedule,
                                ReferenceType::Read);

  UniteInterleavedReadsAndWrites(tensor_info);

  if (tensor_info.empty()) return nullptr;

  return std::move(tensor_info[0]);
}

}  // namespace poly
}  // namespace ir
}  // namespace akg
