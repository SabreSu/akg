/**
 * Copyright 2020 Huawei Technologies Co., Ltd
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

#include "shared_memory_manager.h"
#include "poly/schedule_tree_util.h"
#include "poly/scop.h"
#include "poly/dma_inject.h"
#include <vector>
#include <numeric>

namespace akg {
namespace ir {
namespace poly {

isl::schedule SharedMemoryManager::Run(isl::schedule sch) {
  schedule_ = sch;
  auto root = sch.get_root();
  UpdateDepth(root);
  if (scop_info_.user_config_.GetSharedDepth() >= 0) {
    depth_ = scop_info_.user_config_.GetSharedDepth();
    use_config_ = true;
  }
  CHECK_GE(depth_, 0) << "shared depth should be greater than or equal with zero!";

  // collect all bands at the given depth in the schedule tree
  size_t remain_memory = share_memory_size_;
  root = HoistSharedMemoryOnDepth(root, remain_memory, depth_);
  bool unroll_shared = scop_info_.user_config_.GetUnrollShared();
  root = MapCopiesToThreads(root, unroll_shared);
  schedule_ = root.get_schedule();

  return schedule_;
}

isl::schedule_node SharedMemoryManager::HoistSharedMemoryOnDepth(const isl::schedule_node &root, size_t &remain_memory,
                                                                 size_t depth) {
  auto fn = [depth, &remain_memory, this](isl::schedule_node node) -> isl::schedule_node {
    auto res_node = node;
    if (node.isa<isl::schedule_node_band>()) {
      if (ContainsDepth(node, depth)) {
        auto node_splitted = BandSplitAtDepth(node, depth);
        if (!use_config_ && IsAncestorMapToThread(node_splitted)) {
          LOG(INFO) << "a subtree under the thread_marker cannot "
                    << "be promoted.";
          return node;
        }
        res_node = ManageToShareBelow(this->schedule_, node_splitted, remain_memory);
      }
    }
    return res_node;
  };

  auto root_node = root;
  if (depth == 0) {
    root_node = GenerateEmptyBandInRoot(root_node);
    auto node_splitted = BandSplitAtDepth(root_node, depth);
    node_splitted = ManageToShareBelow(schedule_, node_splitted, remain_memory);
    return node_splitted;
  }

  return MapDescendantTopDown(root, fn);
}

isl::union_set SharedMemoryManager::GatherMappingsTo(MappingCfg *cfg) {
  isl::schedule_node root = schedule_.get_root();
  auto domain_node = root.as<isl::schedule_node_domain>();
  auto domain = domain_node.domain();
  auto mapping_filters = CollectNode<isl::schedule_node_filter>(schedule_);

  std::vector<isl::id> filters;
  for (size_t idx = 0; idx < cfg->bound; ++idx) {
    auto value = cfg->GetAt(idx);
    auto id = isl::id(root.ctx(), value.first);
    filters.push_back(id);
  }
  mapping_filters = FilterNode(mapping_filters, filters);

  auto mapping = isl::union_set::empty(domain.ctx());
  for (auto item : mapping_filters) {
    if (item.isa<isl::schedule_node_filter>()) {
      auto filter = item.as<isl::schedule_node_filter>();
      auto filter_domain = filter.filter().intersect(CollectDomain(item));
      mapping = mapping.unite(filter.filter());
    }
  }
  return mapping;
}

isl::schedule_node SharedMemoryManager::MapCopiesToThreads(isl::schedule_node &root, bool unroll) {
  auto CollectReadWriteFilter = [&unroll, this](isl::schedule_node node) -> isl::schedule_node {
    if (node.isa<isl::schedule_node_filter>()) {
      // transform isl::union_set to a vector of isl::set
      isl::union_set uset = node.as<isl::schedule_node_filter>().get_filter();
      std::vector<isl::set> vset;
      uset.foreach_set([&vset](isl::set s) { vset.push_back(s); });

      bool is_all_sets_read_or_write = std::all_of(vset.begin(), vset.end(), [](isl::set s) {
        auto read_id = isl::id(s.ctx(), std::string(READ_ID_NAME));
        auto write_id = isl::id(s.ctx(), std::string(WRITE_ID_NAME));
        return s.get_tuple_id() == read_id || s.get_tuple_id() == write_id;
      });

      if (is_all_sets_read_or_write) {
        // It is not allowed multi filter-band pairs below a read/write filter.
        int count_filter_band_pair = 0;
        node.foreach_descendant_top_down([&count_filter_band_pair](const isl::schedule_node &sub_node) -> bool {
          if (sub_node.isa<isl::schedule_node_filter>()) {
            if (sub_node.n_children() > 0 && sub_node.child(0).isa<isl::schedule_node_band>()) {
              count_filter_band_pair++;
            }
          }
          return true;
        });
        CHECK(count_filter_band_pair == 1) << "multi filter-> band pairs exist in a read/write filter subtree.";

        auto band_node = node.child({0});
        CHECK(band_node.isa<isl::schedule_node_band>()) << "Type of Node must be band.";
        std::string atomic_type = InAtomicTensors(node);

        // split member that does not involved in thread mapping
        bool has_split = false;
        auto thread_cfg = scop_info_.user_config_.GetThreadConfig();
        auto mem_size = band_node.as<isl::schedule_node_band>().n_member();
        if (mem_size > thread_cfg->bound) {
          band_node = band_node.as<isl::schedule_node_band>().split(mem_size - thread_cfg->bound);
          band_node = band_node.child(0);
          has_split = true;
        }

        // TODO: CHECK <<"no copy band"

        // TODO: CHECK <<"attempted to map memory copies to threads below another thread mapping"
        Mapping mapping;
        auto after_map_pair = MapInnerDimToThreads(band_node, true, thread_cfg, mapping, false);
        band_node = after_map_pair.first;
        if (atomic_type != "" && band_node.isa<isl::schedule_node_mark>() && band_node.has_children() &&
            band_node.child(0).isa<isl::schedule_node_filter>()) {
          band_node =
            band_node.child(0).child(0).insert_mark(isl::id(band_node.ctx(), AtomicMarker("_" + atomic_type)));
          band_node = band_node.parent().parent();
        }
        if (has_split) {
          band_node = band_node.parent();
        }

        if (unroll) {
          band_node = UnrollByMarkOptions(band_node, scop_info_.user_config_.GetMaxUnrollLoop());
        }

        node = band_node.parent();
      }
    }
    return node;
  };

  return root.map_descendant_bottom_up(CollectReadWriteFilter);
}

isl::schedule_node SharedMemoryManager::ManageToShareBelow(isl::schedule &root_sch, isl::schedule_node &node,
                                                           size_t &remaining_memory) {
  isl::schedule_node root_node = root_sch.get_root();

  CHECK(use_config_ || !IsAncestorMapToThread(node)) << "shared memory promotion cannot below thread_marker.";

  auto partial_sched = LocalSchedule(node);
  auto cfg = scop_info_.user_config_.GetBlockConfig();
  auto mapping = GatherMappingsTo(cfg);

  auto out_sched = partial_sched.intersect_domain(mapping);
  CreateClusterList(node, out_sched);
  auto new_node = HoistClusters(root_node, node, remaining_memory);
  auto sync_manager = scop_info_.sync_manager_;
  return sync_manager.InsertPromotionSync(new_node);
}

std::set<std::string> SharedMemoryManager::AnalysisReduceTensors() {
  std::set<std::string> id_sets;
  if (!scop_info_.user_config_.GetEnableAkgReduceLib()) {
    return id_sets;
  }

  /*************************************************
   * In order to enable cuda atomic operator, add
   * these tensors for shared memory promotion list
   *************************************************/
  auto atomic_tensors = scop_info_.analysis_result_.GetAtomicTensors();
  if (!atomic_tensors.empty()) {
    id_sets.clear();
    for (const auto &item : atomic_tensors) {
      if (id_sets.count(item.tensor_name) == 0) {
        id_sets.emplace(item.tensor_name);
      }
    }
  }

  /***********************************************
   * For the condition that it is without cuda
   * atomic usage, but with reduce operation.
   * Also need to add these tensors for shared memory
   * promotion list.
   *********************************************/
  auto reduce_out_tensors = scop_info_.analysis_result_.GetReduceOutTensors();
  for (const auto &item : reduce_out_tensors) {
    if (id_sets.count(item) == 0) {
      id_sets.emplace(item);
    }
  }

  return id_sets;
}

void SharedMemoryManager::CreateClusterList(const isl::schedule_node &node, const isl::union_map &outer_sch) {
  isl::union_map reads = scop_info_.analysis_result_.GetReads();
  isl::union_map writes = scop_info_.analysis_result_.GetWrites();
  isl::union_map copyin = scop_info_.analysis_result_.GetCopyin();
  isl::union_map fake_copyin = scop_info_.analysis_result_.GetFakeCopyin();

  auto read_map = scop_info_.StmtReadMap();
  auto write_map = scop_info_.StmtWriteMap();
  auto stmt_map = scop_info_.analysis_result_.GetStmtOpInfoMap();
  std::vector<isl::id> tensor_list;
  std::set<std::string> id_sets;
  std::set<std::string> read_sets;
  std::set<std::string> write_sets;
  for (auto item : read_map) {
    for (auto item_id : item.second) {
      if (read_sets.count(item_id.get_name()) == 0) {
        read_sets.insert(item_id.get_name());
      }
    }
  }
  for (auto item : write_map) {
    for (auto item_id : item.second) {
      if (write_sets.count(item_id.get_name()) == 0) {
        write_sets.insert(item_id.get_name());
      }
    }
  }
  /*********************************************************
   * manage only read tensors to share memory
   * for read and write tensor, should be managed to local memory
   ********************************************************/
  std::set_difference(read_sets.begin(), read_sets.end(), write_sets.begin(), write_sets.end(),
                      std::inserter(id_sets, id_sets.begin()));

  if (scop_info_.user_config_.GetEnableAkgReduceLib()) {
    id_sets = AnalysisReduceTensors();
  }

  if (!configed_tensors_.empty()) {
    id_sets.clear();
    for (const auto &item : configed_tensors_) {
      if (id_sets.count(item) == 0) {
        id_sets.emplace(item);
      }
    }
  }
  for (auto item : id_sets) {
    tensor_list.push_back(isl::id(scop_info_.ctx_, item));
  }
  for (const auto &item : tensor_list) {
    isl::id dst_tensor_id = GpuDstId(GpuMemType::SHARED, item);
    std::vector<size_t> buffer_sizes;
    std::vector<std::pair<isl::id, MemType>> data_stream;
    data_stream.push_back(std::make_pair(item, MemType::DDR));
    data_stream.push_back(std::make_pair(item, MemType::SHARED_));
    BufferDefInfo promoted_info = BufferDefInfo{item,
                                                dst_tensor_id,
                                                item,
                                                MemType::DDR,
                                                "",
                                                false,
                                                false,
                                                data_stream,
                                                Tensor(),
                                                Handle(),
                                                buffer_sizes,
                                                nullptr,
                                                isl::union_map::empty(isl::space(scop_info_.ctx_, 0))};
    promoted_info.footprints_cluster =
      TensorFootprintCluster::HoistBufferFootprintCluster(outer_sch, item, reads, copyin, writes, fake_copyin);
    if (promoted_info.footprints_cluster != nullptr) {
      promoted_info.footprint_cluster_map.emplace_back(std::make_pair(node, promoted_info.footprints_cluster));
      scop_info_.analysis_result_.buffer_def_infos_.push_back(promoted_info);
      GatherBufferFootprintDefInfo(node, promoted_info);
    }
  }
}

void SharedMemoryManager::GatherBufferFootprintDefInfo(const isl::schedule_node &node, BufferDefInfo &tensor_info) {
  auto fp_cluster = tensor_info.footprints_cluster;
  std::vector<size_t> sizes;
  if (fp_cluster == nullptr) {
    tensor_info.AddSize(node, sizes);
    return;
  }
  sizes = fp_cluster->GetFixedBoxSizes();

  isl::id tensor_id = tensor_info.tensor_id;
  isl::id cluster_id = tensor_info.dst_tensor_id;

  // build a Halide Node for cluster_id
  Array<Expr> shapes;
  for (auto i : sizes) {
    shapes.push_back(Expr(static_cast<int>(i)));
  }

  Type type = scop_info_.GetDtypeOf(tensor_id);
  Tensor tensor = placeholder(shapes, type, cluster_id.get_name());
  const Buffer buffer = decl_buffer(shapes, scop_info_.GetDtypeOf(tensor_id), cluster_id.get_name());
  scop_info_.user_config_.SetBind(tensor, buffer);

  tensor_info.sizes = sizes;
  tensor_info.tensor = tensor;
  tensor_info.data_type = type;
  tensor_info.AddSize(node, sizes);
}

isl::schedule_node SharedMemoryManager::HoistClusters(const isl::schedule_node &root_node,
                                                      const isl::schedule_node &node, size_t &remaining_memory) {
  auto partial_sched_mupa = ShortScheduleMupa(root_node, node);
  auto res_node = node;
  for (size_t index = 0; index < scop_info_.analysis_result_.buffer_def_infos_.size(); index++) {
    BufferDefInfo &buffer_info = scop_info_.analysis_result_.buffer_def_infos_[index];
    auto fp_cluster = buffer_info.GetFootPrintClusterGPU(node);
    if ((fp_cluster == nullptr || !fp_cluster->foot_print_.box.is_valid())) {
      continue;
    }
    auto id = buffer_info.tensor_id;
    auto box_sizes = fp_cluster->GetFixedBoxSizes();
    if (box_sizes.size() == 0) {
      LOG(FATAL) << "Can not manage a scalar tensor";
    }

    if (box_sizes.back() % 2 == 0) {
      box_sizes.back() += 1;
    }

    auto approximation_size = std::accumulate(box_sizes.begin(), box_sizes.end(), 1, std::multiplies<size_t>());
    size_t byte = Bytes(id);
    size_t memory_requirement = approximation_size * byte;
    bool use_reuse_filter = true;
    if (InAtomicTensors(buffer_info.tensor_id.name()) || InReduceTensors(buffer_info.tensor_id.name())) {
      use_reuse_filter = false;
    }
    if (memory_requirement < remaining_memory) {
      if (use_reuse_filter && !ReuseTensorCluster(*fp_cluster, partial_sched_mupa) &&
          !CoalescingAccessWay(root_node, res_node, *fp_cluster)) {
        continue;
      }
      res_node = HoistToBlockThreadMemory(res_node, GpuMemType::SHARED, id, *(fp_cluster), true);
      remaining_memory -= memory_requirement;

      // collect active_buffer_footprints_ info for codegen
      auto out_schedule = LocalSchedule(res_node);
      auto active_domains = CollectDomain(res_node);
      auto dst_id = GpuDstId(GpuMemType::SHARED, id);
      scop_info_.analysis_result_.active_buffer_footprints_.emplace_back(std::make_pair(
        active_domains,
        BufferedFootPrintInfo{std::shared_ptr<TensorFootprintCluster>(std::move(fp_cluster)), out_schedule, dst_id}));
      buffer_info.find_buffer = true;
    }
  }
  return res_node;
}

isl::schedule_node SharedMemoryManager::HoistToBlockThreadMemory(isl::schedule_node &tree, GpuMemType type,
                                                                 const isl::id &tensor_id,
                                                                 TensorFootprintCluster &cluster,
                                                                 bool force_last_extension_odd) {
  auto out_schedule = LocalSchedule(tree);
  auto active_domains = CollectDomain(tree);

  isl::id dst_tensor_id = GpuDstId(type, tensor_id);
  auto sizes = cluster.GetFixedBoxSizes();
  if (sizes.size() > 0 && force_last_extension_odd && (sizes.back() % 2) == 0) {
    sizes.back() += 1;
  }

  auto res_node = PlaceOuterDataCopyBelow(scop_info_, tree, cluster, tensor_id, dst_tensor_id, out_schedule,
                                          schedule_.get_domain().get_space());
  return res_node;
}

bool SharedMemoryManager::ReuseTensorCluster(const TensorFootprintCluster &cluster,
                                             const isl::multi_union_pw_aff &outer_pw_aff) {
  isl::union_map out_schedule = isl::union_map::from(outer_pw_aff);
  out_schedule = out_schedule.range_product(cluster.OrigianlAccessRelations());
  return !out_schedule.is_injective();
}

bool SharedMemoryManager::CoalescingAccessWay(const isl::schedule_node &root, const isl::schedule_node &node,
                                              const TensorFootprintCluster &cluster) {
  isl::union_map original = cluster.OrigianlAccessRelations();
  size_t tensor_dim = cluster.foot_print_.GetBoxDim();
  std::vector<isl::schedule_node> thread_marker = CollectFnNode(IsThreadMappedMark, root);
  for (auto item : thread_marker) {
    if (!(item.isa<isl::schedule_node_mark>()) && !(item.has_children()) &&
        !(item.child(0).isa<isl::schedule_node_filter>())) {
      continue;
    }
    isl::schedule_node thread_filter = item.child(0);
    if (!thread_filter.has_children()) {
      continue;
    }
    isl::schedule_node thread_band = thread_filter.child(0);
    if (!thread_band.has_children()) {
      continue;
    }
    isl::schedule_node inner_band = thread_band.child(0);
    size_t num_mapped_thread = inner_band.schedule_depth() - thread_band.schedule_depth();
    if (num_mapped_thread == 0) {
      continue;
    }
    size_t inner_depth = inner_band.schedule_depth();
    auto active_domains = CollectDomain(thread_band);
    auto local_access = original.intersect_domain(active_domains);
    auto schedule = ShortSchedule(inner_band);
    auto schedule_access = local_access.apply_domain(schedule);
    for (auto access : schedule_access.get_map_list()) {
      auto schedule_space = access.get_space().domain();
      auto tensor_space = access.get_space().range();
      auto element_next = CreateMapIncreaseDim(tensor_space, tensor_dim - 1);
      auto schedule_next = CreateMapIncreaseDim(schedule_space, inner_depth - 1);
      auto access_by_adjacent_inner = schedule_next.apply_domain(access).apply_range(access);
      if (!access_by_adjacent_inner.is_subset(element_next)) {
        return true;
      }
    }
  }
  return false;
}

void SharedMemoryManager::UpdateDepth(const isl::schedule_node &root) {
  auto outer_band = GetOuterBand(root);
  auto cfg = scop_info_.user_config_.GetBlockConfig();
  if (outer_band.isa<isl::schedule_node_band>()) {
    auto block_depth = cfg->bound + 1;
    auto outer_band_depth = outer_band.as<isl::schedule_node_band>().n_member();
    block_depth = std::min<int>(block_depth, outer_band_depth);
    if (block_depth > outer_band_depth && !UnderThreadMarker(block_depth)) {
      depth_ = block_depth;
    } else {
      depth_ = outer_band_depth;
    }
  }
}

bool SharedMemoryManager::UnderThreadMarker(size_t depth) {
  isl::schedule_node root = this->schedule_.get_root();
  auto bands = BandsContainingScheduleDepth(root, depth);
  for (auto item : bands) {
    if (IsAncestorMapToThread(item)) {
      return true;
    }
  }
  return false;
}

std::string SharedMemoryManager::InAtomicTensors(isl::schedule_node &node) {
  if (!node.isa<isl::schedule_node_filter>()) {
    return "";
  }
  auto filter = node.as<isl::schedule_node_filter>().filter();
  auto filter_set = filter.unwrap();
  std::string atomic_type = "";
  filter_set.range().foreach_set([this, &atomic_type](const isl::set &s) -> void {
    std::string promoted_tensor = s.get_tuple_name();
    std::string posfix = "_shared";
    std::string::size_type pos = promoted_tensor.find(posfix);
    if (pos != std::string::npos) {
      std::string tensor = promoted_tensor.substr(0, pos);
      for (const auto &item : scop_info_.analysis_result_.GetAtomicTensors()) {
        if (item.tensor_name == tensor) {
          atomic_type = item.tensor_type;
        }
      }
    }
  });
  return atomic_type;
}

bool SharedMemoryManager::InAtomicTensors(std::string name) {
  for (const auto &item : scop_info_.analysis_result_.GetAtomicTensors()) {
    if (item.tensor_name == name) {
      return true;
    }
  }
  return false;
}

bool SharedMemoryManager::InReduceTensors(std::string name) {
  for (const auto &item : scop_info_.analysis_result_.GetReduceOutTensors()) {
    if (item == name) {
      return true;
    }
  }
  return false;
}

std::string SharedMemoryManager::AtomicMarker(std::string type) { return ATOMIC_MARKER + type; }

size_t SharedMemoryManager::Bytes(const isl::id tensor_id) {
  Type type = scop_info_.GetDtypeOf(tensor_id);
  return static_cast<size_t>(type.bytes());
}

}  // namespace poly
}  // namespace ir
}  // namespace akg
