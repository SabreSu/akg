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
#include "util.h"

namespace akg {
bool IsBlockIdx(const std::string &name) { return name.find("blockIdx") != std::string::npos; }
bool IsBlockIdxX(const std::string &name) { return name == BLOCK_IDX_X; }
bool IsBlockIdxY(const std::string &name) { return name == BLOCK_IDX_Y; }
bool IsBlockIdxZ(const std::string &name) { return name == BLOCK_IDX_Z; }
bool IsThreadIdxX(const std::string &name) { return name == THREAD_IDX_X; }
bool IsThreadIdxY(const std::string &name) { return name == THREAD_IDX_Y; }
bool IsThreadIdxZ(const std::string &name) { return name == THREAD_IDX_Z; }

std::string GetProcess(const std::string &json_str) {
  size_t pos = json_str.find("\"process\"");
  if (pos != std::string::npos && json_str.find("cuda", pos) != std::string::npos) {
    return "cuda";
  }
  return "aicore";
}

std::string GetSchedule(Array<Tensor> &outputs) {
  for (const Tensor &t : outputs) {
    if (t->op->tag == "comm_reduce" || t->op->tag == "comm_reduce_idx") {
      return "reduce";
    }
  }
  return "injective";
}

picojson::value String2Json(const std::string &json_str) {
  picojson::value v;
  std::string err = picojson::parse(v, json_str);
  CHECK(err.empty()) << "json parse error, error message: " << err;
  return v;
}
bool IsReduce(const std::string &op_name) {
  // if topi support more, add to this list
  std::unordered_set<std::string> elems = {"ReduceSum", "ReduceMax", "ReduceMin"};
  return elems.find(op_name) != elems.end();
}
bool IsTransform(const std::string &op_name) {
  // if topi support more, add to this list
  std::unordered_set<std::string> elems = {"Reshape", "ExpandDims", "Squeeze", "Flatten", "ProccessNode"};
  return elems.find(op_name) != elems.end();
}
bool IsInplaceAssign(const std::string &op_name) { return op_name == "InplaceAssign"; }
bool IsAssign(const std::string &op_name) { return op_name == "Assign"; }
bool IsOtherOp(const std::string &op_name) {
  // if topi support more, add to this list
  std::unordered_set<std::string> elems = {"Matmul",    "BatchMatMul", "Conv",          "Transpose",
                                           "Tile",      "Assign",      "InplaceAssign", "EquivFormat",
                                           "TransData", "AddMinValue", "BroadcastTo"};
  return elems.find(op_name) != elems.end();
}
bool IsElemwise(const std::string &op_name) {
  return !IsReduce(op_name) && !IsTransform(op_name) && !IsOtherOp(op_name);
}
bool EqualShape(const Array<Expr> &shape1, const Array<Expr> &shape2) {
  if (shape1.size() != shape2.size()) return false;
  for (size_t i = 0; i < shape1.size(); ++i) {
    if (!Equal(shape1[i], shape2[i])) {
      return false;
    }
  }
  return true;
}

bool ShapeIsOne(const Array<Expr> &shape) { return shape.size() == 1 && Equal(shape[0], 1); }
std::string GetOpName(const Provide *p) {
  auto call = p->value.as<Call>();
  CHECK(call);
  auto op_name = call->name;
  return op_name;
}
std::string CreateDataFormatKey(const std::string &tensor_name) {
  std::string key = tensor_name + "_format";
  return key;
}

}  // namespace akg
