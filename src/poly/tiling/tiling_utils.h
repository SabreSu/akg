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
#ifndef POLY_TILING_UTILS_H_
#define POLY_TILING_UTILS_H_

#include <iostream>
#include <fstream>

#include <tvm/target_info.h>
#include <tvm/ir.h>

#include "common/target_info.h"

namespace akg {
namespace ir {
namespace poly {

/* Device Info  */
enum TilingMemScope {
  // global
  MEM_SCOPE_GM = 0,
  // davinci
  MEM_SCOPE_UB,
  MEM_SCOPE_L1,
  MEM_SCOPE_L0A,
  MEM_SCOPE_L0B,
  MEM_SCOPE_L0C,
  // gpu
  MEM_SCOPE_SHARED,
  MEM_SCOPE_LOCAL,
  // end
  MEM_SCOPE_BULK,
};

class DavinciInfo {
 public:
  ~DavinciInfo() {}
  static DavinciInfo &GetInstance() {
    static DavinciInfo hardware_info;
    return hardware_info;
  }

  int64_t GetMemoryLimitInScope(int scope_idx) {
    CHECK_LT(scope_idx, MEM_SCOPE_BULK);
    return davinci_mem_limit_[scope_idx];
  }

 private:
  DavinciInfo() { InitDavinciMemoryLimit(); }
  int64_t davinci_mem_limit_[MEM_SCOPE_BULK]{0};

  void InitDavinciMemoryLimit() {
    auto CollectLimit = [this](const std::string &scope, TilingMemScope mem) {
      air::MemoryInfo info = air::GetMemoryInfo(scope);
      CHECK(info.defined());
      davinci_mem_limit_[mem] = info->max_num_bits / 8;
    };
    CollectLimit("local.UB", MEM_SCOPE_UB);
    CollectLimit("local.L1", MEM_SCOPE_L1);
    CollectLimit("local.L0A", MEM_SCOPE_L0A);
    CollectLimit("local.L0B", MEM_SCOPE_L0B);
    CollectLimit("local.L0C", MEM_SCOPE_L0C);
    davinci_mem_limit_[MEM_SCOPE_GM] = 0;
  }
};

class GpuInfo {
 public:
  ~GpuInfo() {}
  static GpuInfo &GetInstance() {
    static GpuInfo hardware_info;
    return hardware_info;
  }

  int64_t GetMemoryLimitInScope(int scope_idx) {
    CHECK_LT(scope_idx, MEM_SCOPE_BULK);
    return gpu_mem_limit_[scope_idx];
  }

 private:
  GpuInfo() { InitGpuMemoryLimit(); }
  int64_t gpu_mem_limit_[MEM_SCOPE_BULK]{0};

  void InitGpuMemoryLimit() {
    auto CollectLimit = [this](const std::string &scope, TilingMemScope mem) {
      air::GpuMemoryInfo info = air::GetGpuMemoryInfo(scope);
      CHECK(info.defined());
      gpu_mem_limit_[mem] = info->max_bytes_per_block;
    };
    CollectLimit("shared", MEM_SCOPE_SHARED);
    CollectLimit("reg", MEM_SCOPE_LOCAL);
    gpu_mem_limit_[MEM_SCOPE_GM] = 0;
  }
};

/* Log utils */
enum LogStage { ANA_SCHETREE, ANA_BUF_LIVE_EXTENT, ANA_TILING_SPACE, DO_TILING, DO_TUNING, MICRO_TUNING, GPU_MAPPING };

class TileLogger {
 public:
  ~TileLogger() {}
  using LogFile = std::vector<std::string>;
  static TileLogger &GetInstance(std::string log_file_name) {
    static TileLogger tile_logger_(log_file_name);
    return tile_logger_;
  }
  void AppendLine(LogStage stage, const std::string &line);
  void AppendLog(LogStage stage, std::stringstream &ss);
  bool DumpLogFile();
  void ClearCache();
  void LogFatalAndSaveLog(const std::string &fatal_log);
  std::string GetDumpDir();

 private:
  explicit TileLogger(std::string log_file_name) : log_file_name_(log_file_name) {}

  std::string log_file_name_;
  LogFile analyze_schedule_tree_stage_;
  LogFile analyze_buffer_live_extent_stage_;
  LogFile analyze_tiling_space_stage_;
  LogFile do_tiling_stage_;
  LogFile do_tuning_stage_;
  LogFile micro_tuning_stage_;
  LogFile gpu_mapping_stage_;
};

/* Halide & Schedule tree analysis utils */
using Band = std::vector<const air::ir::For *>;
using VarNames = std::vector<std::string>;

std::unordered_map<std::string, std::string> ExtractLoopIndicesFromMatrices(std::vector<VarNames> var_names_list);

VarNames VisitVarNames(const air::Expr &arg, VarNames var_names, bool add_num = true);

/* Data format definition */
const VarNames DavinciNCHW = {"N", "C", "H", "W", "C0"};
const VarNames DavinciNHWCC0 = {"N", "H", "W", "C", "C0"};
const VarNames DavinciNC1HWC0 = {"N", "C1", "H", "W", "C0"};

const VarNames ForwardFilter = {"C1_in", "C1_out", "C0_out", "C0_in"};          //  nZ, Cin = [kc1,kh,kw]
const VarNames BackpropFilter = {"C1_out", "C1_in", "C0_in", "C0_out"};         //  backprop_input, Cout = [kc1,kh,kw]
const VarNames ForwardFeaturemap = {"N", "C1_in", "H_in", "W_in", "C0_in"};     // zZ, H_in = [H, Kh], W_in = [W, kw]
const VarNames BackpropFeaturemap = {"N", "C1_out", "H_in", "W_in", "C0_out"};  // zZ, H_in = [H, Kh], W_in = [W, kw]
const VarNames FilterOutput = {"C1_out", "kh", "kw", "C1_in", "C0_in", "C0_out"};
const VarNames FilterInput = {"N", "C1_out", "H", "W", "C0_out"};

const VarNames FormatM = {"mi", "mo"};
const VarNames FormatN = {"ni", "no"};
const VarNames FormatK = {"ki", "ko"};
const VarNames FormatB = {"bi", "bo"};

}  // namespace poly
}  // namespace ir
}  // namespace akg
#endif  // POLY_TILING_UTILS_H_
