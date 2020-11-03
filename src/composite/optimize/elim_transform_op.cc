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
#include "composite/optimize/optimize.h"

namespace akg {
class ElimTransformAnalysis {
 public:
  ElimTransformAnalysis(Graph &g, BuildInfoOpt &opt, AnalysisResult &result) : g_(g), opt_(opt), result_(result){};
  void Run() {
    // from output to input, try to remove each transform op, when removed op, should change each tensor's shape by
    // elemwise op, and try to add reshape op when unelemwise op's input shape and output shape are changed.
    size_t settled_size;
    do {
      settled_size = g_.visited_funcs.size();
      for (const auto &output : g_.output_funcs) {
        AnalysisInner(output);
      }
    } while (settled_size != g_.visited_funcs.size());

    for (const auto &p : result_.to_be_removed) {
      // if output removed, should collect opt.sames
      if (g_.output_funcs.count(p->func)) {
        opt_.sames[p->func] = result_.to_be_replaced[p->func];
      }
    }
  }

 private:
  void AnalysisTransform(const FunctionRef &output) {
    auto provide = g_.func_stmts[output];
    auto call = provide->value.as<Call>();
    CHECK(call);
    CHECK(call->args.size() == 1);
    CHECK(call->args[0].as<Call>());
    auto input = call->args[0].as<Call>()->func;
    // if output is kernel output and input is kernel input, cannot remove this op
    if (!(g_.output_funcs.count(output) && g_.input_funcs.count(input))) {
      // if not visited or input shape and output shape as same, can remove this op, change input shape to output shape,
      // replace output tensor to input tensor
      auto input_shape = result_.ShapeChanged(input) ? result_.changed_shapes[input] : g_.func_shape[input];
      auto output_shape = result_.ShapeChanged(output) ? result_.changed_shapes[output] : g_.func_shape[output];
      if (!g_.visited_funcs.count(input) || EqualShape(input_shape, output_shape)) {
        result_.to_be_replaced[output] = input;
        // if any tensor replace to output already, it should change to new input
        for (auto &kv : result_.to_be_replaced) {
          if (kv.second == output) {
            kv.second = input;
          }
        }
        result_.changed_shapes[input] = output_shape;
        result_.to_be_removed.insert(provide);
        g_.visited_funcs.insert(output);
        g_.visited_funcs.insert(input);
      }  // else if visited and input output shape are different, do noting, if input shape changed, already in set
    }
  }

  void AnalysisElemwise(const FunctionRef &output) {
    auto inputs = g_.pre_graph[output];
    bool output_changed = result_.ShapeChanged(output);
    auto output_shape = output_changed ? result_.changed_shapes[output] : g_.func_shape[output];
    for (const auto &input : inputs) {
      if (!g_.visited_funcs.count(input)) {
        // if not visited and output changed, change input shape
        if (output_changed) {
          result_.changed_shapes[input] = output_shape;
          g_.visited_funcs.insert(input);
        }
      } else {
        // if visited, check input shape and out shape are same or not, if not, need reshape
        auto input_shape = result_.ShapeChanged(input) ? result_.changed_shapes[input] : g_.func_shape[input];
        if (!EqualShape(output_shape, input_shape) && !ShapeIsOne(input_shape)) {
          // b = op(a) -> t = trans(a); b = op(t)
          LOG(INFO) << "IS ELEMWISE, COLLECT RESHAPE";
          result_.CollectReshape(g_.func_stmts[output], input, output_shape, input_shape);
        }
      }
    }
  }

  void AnalysisOthers(const FunctionRef &output) {
    auto provide = g_.func_stmts[output];
    auto op_name = GetOpName(provide);
    auto output_shape = result_.ShapeChanged(output) ? result_.changed_shapes[output] : g_.func_shape[output];
    // if output shape changed, output need reshape
    // b = reduce(a) -> t = reduce(a); b = trans(t)
    g_.visited_funcs.insert(output);
    if (result_.ShapeChanged(output)) {
      LOG(INFO) << "IS UNELEMWISE, OUTPUT COLLECT RESHAPE";
      result_.CollectReshape(provide, output, g_.func_shape[output], output_shape);
    }
    if (!(IsReduce(op_name) && ShapeIsOne(output_shape))) {  // we consider that allreduce op's input shape is flexable
      // if input shape changed, input need reshape
      // b = reduce(a) -> t = trans(a); b = reduce(t)
      auto inputs = g_.pre_graph[output];
      for (const auto &input : inputs) {
        g_.visited_funcs.insert(output);
        if (result_.ShapeChanged(input)) {
          LOG(INFO) << "IS UNELEMWISE, INPUT COLLECT RESHAPE";
          result_.CollectReshape(provide, input, g_.func_shape[input], result_.changed_shapes[input]);
        }
      }
    }
  }

  void AnalysisInner(const FunctionRef &output) {
    if (!g_.func_stmts.count(output)) return;
    auto provide = g_.func_stmts[output];
    auto op_name = GetOpName(provide);
    if (IsTransform(op_name)) {
      AnalysisTransform(output);
    } else if (IsElemwise(op_name) && g_.CanChangeElem(output)) {
      AnalysisElemwise(output);
    } else {
      // the op which can not change shape
      AnalysisOthers(output);
    }
    auto inputs = g_.pre_graph[output];
    for (const auto &input : inputs) {
      AnalysisInner(input);
    }
  }

 private:
  Graph &g_;
  BuildInfoOpt &opt_;
  AnalysisResult &result_;
};

Stmt ElimTransformOp(Stmt &s, const FuncRefSet &input_funcs, const FuncRefSet &output_funcs, BuildInfoOpt &opt) {
  auto f = StmtToGraph(input_funcs, output_funcs);
  f.Visit(s);
  AnalysisResult result;
  auto analysis = ElimTransformAnalysis(f.g_, opt, result);
  analysis.Run();
  result.Dump();
  return DoAnalysis(result).Mutate(s);
}
}  // namespace akg
