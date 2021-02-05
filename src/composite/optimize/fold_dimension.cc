/**
 * Copyright 2021 Huawei Technologies Co., Ltd
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

#define FOLD_DIM_DUMP  0

namespace akg {
class DimensionFolderPlan : public IRVisitor {
 public:
  struct FoldTensor;
  struct Relation {
    Relation(FoldTensor* t)
    : to(t) {
    }
    FoldTensor* to;
    int forward_commit{-1};
    int backward_commit{-1};
    std::vector<int> forward_mapping;
    std::vector<int> backward_mapping;
  };

  struct FoldTensor {
    int update{0};
    std::vector<int64_t> shape;
    std::vector<int> fold_dims;
    std::vector<Relation> succ;
  };

  void Plan(const Stmt stmt) {
    IRVisitor::Visit(stmt);
    if (give_up_) {
      return;
    }
    std::vector<FoldTensor*> inputs;
    for (FunctionRef ref : inputs_) {
      auto t = tensors_[ref].get();
      CHECK(t);
      inputs.push_back(t);
    }
    int64_t old_fold_dims = 0;
    do {
#if FOLD_DIM_DUMP
      std::cout<<"start propagate..."<<std::endl;
#endif
      old_fold_dims = folded_dims_;
      for (auto t : inputs) {
        backward_visited_.clear();
        Propagation(t);
      }
    } while (folded_dims_ != old_fold_dims && folded_dims_ < total_dims_);
    if (folded_dims_ == total_dims_) {
      give_up_ = true;
    }
#if FOLD_DIM_DUMP
    Dump();
#endif
  }

  void Visit_(const AttrStmt* op) {
    if (op->attr_key == "attrs") {
      auto attrs = Downcast<Map<std::string, NodeRef>>(op->node);
      if (attrs.find("axis") != attrs.end()) {
        reduce_axis_.clear();
        Array<Expr> axis = Downcast<Array<Expr>>(attrs["axis"]);
        auto axis_vec = ExtractIntVector(axis);
        if (axis_vec.empty()) {
          reduce_axis_.insert(0);
        } else {
          reduce_axis_.insert(axis_vec.begin(), axis_vec.end());
        }
        IRVisitor::Visit_(op);
        return;
      }
    }
    IRVisitor::Visit_(op);
  }

  void Visit_(const Provide* op) {
    auto prim = op->value.as<Call>();
    FoldTensor *output = GetTensor(op->func, op->args, false);
    std::vector<FoldTensor*> inputs;
    for (Expr arg : prim->args) {
      auto t = arg.as<Call>();
      if (t) {
        inputs.push_back(GetTensor(t->func, t->args, true));
      }
    }
#if FOLD_DIM_DUMP
    std::cout<<"[Provide] "<<op->func->func_name()<<"("<<output<<") = (";
    for (FoldTensor* t : inputs) std::cout<<t<<",";
    std::cout<<")\n";
#endif
    if (IsElemwise(prim->name) || prim->name == "BroadcastTo") {
      for (FoldTensor *input : inputs) {
        AddElemBroadRelation(input, output);
      }
    } else if (IsReduce(prim->name)) {
      CHECK(inputs.size() == 1 && !reduce_axis_.empty());
      AddReduceRelation(inputs[0], output, reduce_axis_);
    } else if (prim->name == "InplaceAssign") {
      CHECK(inputs.size() == 3);
      AddElemBroadRelation(inputs[1], inputs[0]);
      AddElemBroadRelation(inputs[2], output);
    } else {
      for (FoldTensor *input : inputs) {
        Relation rel(output);
        input->succ.emplace_back(std::move(rel));
      }
      give_up_ = true;
    }
  }

  std::unordered_map<FunctionRef, std::shared_ptr<FoldTensor>, NodeHash, NodeEqual> tensors_;
  bool give_up_{false};

 private:
  std::unordered_set<int64_t> reduce_axis_;
  std::vector<FunctionRef> inputs_;
  int64_t total_dims_{0};
  int64_t folded_dims_{0};
  std::unordered_set<FoldTensor*> forward_visited_;
  std::unordered_set<FoldTensor*> backward_visited_;

  void AddElemBroadRelation(FoldTensor *input, FoldTensor *output) {
    size_t dim_offset = output->shape.size() - input->shape.size();
    Relation rel(output);
    std::vector<int> domain;
    for (size_t i = 0; i < dim_offset; ++i) {
      rel.backward_mapping.push_back(-1);
    }
    bool in_elemwise = true;
    bool shape_broadcasting = true;
    for (size_t i = dim_offset; i < output->shape.size(); ++i) {
      bool is_elemwise = input->shape[i - dim_offset] == output->shape[i];
      if (shape_broadcasting && is_elemwise) {
        shape_broadcasting = false;
      }
      if (shape_broadcasting) {
        rel.forward_mapping.push_back(-1);
        rel.backward_mapping.push_back(-1);
      } else {
        rel.forward_mapping.push_back(i);
        rel.backward_mapping.push_back(i - dim_offset);
      }
      if (i == dim_offset) {
        domain.push_back(i - dim_offset);
        in_elemwise = is_elemwise;
      } else if (is_elemwise != in_elemwise) {
        domain.push_back(i - dim_offset);
        in_elemwise = is_elemwise;
      }
    }
    domain.push_back(input->shape.size());
    FoldRelation(input, &rel, domain);
    input->succ.emplace_back(std::move(rel));
  }

  void AddReduceRelation(FoldTensor *input, FoldTensor *output, const std::unordered_set<int64_t> &reduce_axis) {
    Relation rel(output);
    std::vector<int> domain;
    bool keep_dim = input->shape.size() == output->shape.size();
    bool in_reduce = false;
    int output_idx = 0;
    for (size_t i = 0; i < input->shape.size(); ++i)  {
      bool reduce_mode = reduce_axis.count(i) > 0;
      if (i == 0) {
        domain.push_back(0);
        in_reduce = reduce_mode;
      } else if (reduce_mode != in_reduce) {
        domain.push_back(i);
        in_reduce = reduce_mode;
      }
      if (!reduce_mode || keep_dim) {
        rel.backward_mapping.push_back(i);
        rel.forward_mapping.push_back(output_idx);
        output_idx++;
      } else {
        rel.forward_mapping.push_back(-1);
      }
    }
    if (rel.backward_mapping.empty()) {
      rel.backward_mapping.push_back(-1);
    }
    domain.push_back(input->shape.size());
    FoldRelation(input, &rel, domain);
    input->succ.emplace_back(std::move(rel));
  }

  void Dump() {
    std::cout<<"\nTensor          Split      Relation\n-----------------------\n";
    for (auto val : tensors_) {
      std::cout<<val.first->func_name()<<" : ";
      FoldTensor *t = val.second.get();
      for (size_t i = 0; i < t->fold_dims.size(); ++i) {
        if (t->fold_dims[i] == (int)i) {
          if (i > 0) {
            std::cout<<"), ";
          }
          std::cout<<'(';
        }
        std::cout<<t->shape[i]<<',';
      }
      std::cout<<"),";
      for (Relation& rel : t->succ) {
        std::cout<<rel.to<<", out_map=[";
        for (int map : rel.forward_mapping) {
          std::cout<<map<<",";
        }
        std::cout<<"],in_map=[";
        for (int map : rel.backward_mapping) {
          std::cout<<map<<",";
        }
        std::cout<<"]";
      }
      std::cout<<std::endl;
    } 
  }

  std::vector<int64_t> ExtractIntVector(Array<Expr> &vec) {
    std::vector<int64_t> res;
    for (Expr s : vec) {
      int64_t val = -1;
      if (s.as<IntImm>()) {
        val = s.as<IntImm>()->value;
      } else if (s.as<UIntImm>()) {
        val = s.as<UIntImm>()->value;
      } else {
        CHECK(0);
      }
      res.push_back(val);
    }
    return res;
  }

  FoldTensor* GetTensor(FunctionRef func, Array<Expr> shape, bool is_input) {
    auto it = tensors_.find(func);
    if (it != tensors_.end()) {
      return it->second.get();
    }
    std::shared_ptr<FoldTensor> t = std::make_shared<FoldTensor>();
    tensors_.emplace(func, t);
    t->shape = ExtractIntVector(shape);
    t->fold_dims.resize(t->shape.size(), 0);
    total_dims_ += t->shape.size();
    folded_dims_ += 1;
    if (is_input) {
      inputs_.emplace_back(func);
    }
    return t.get();
  }

  void FoldRelation(FoldTensor *t, Relation *r, const std::vector<int> &fold_domain) {
#if FOLD_DIM_DUMP
    std::cout<<"[FoldRelation] "<<t<<" -> "<<r->to<<", domain=";
    for (int i : fold_domain) std::cout<<i<<", ";
    std::cout<<std::endl;
#endif
    CHECK(fold_domain.size() >= 2);
    int start = fold_domain[0];
    for (size_t i = 1; i < fold_domain.size(); ++i) {
      int next_start = fold_domain[i];
      int end = next_start - 1;
      UpdateFoldDim(t, start, end);
      int out_start = r->forward_mapping[start];
      int out_end = r->forward_mapping[end];
      if (out_start != -1 && out_end != -1) {
        UpdateFoldDim(r->to, out_start, out_end);
      }
      start = next_start;
    }
  }

  void Propagation(FoldTensor *t) {
    auto get_output_fold_dim = [](Relation *rel, int i) -> int {
      int output_fold_dim = rel->forward_mapping[i];
      if (output_fold_dim >= 0) {
        output_fold_dim = rel->to->fold_dims[output_fold_dim];
      }
      return output_fold_dim; 
    };
    if (backward_visited_.count(t)) {
      return;
    }
    forward_visited_.insert(t);
    for (Relation &rel: t->succ) {
      PropagationForward(t, rel);
#if FOLD_DIM_DUMP
      std::cout<<"[Propagation] "<<rel.to<<" -> "<<t<<std::endl;
#endif
      if (rel.to->update > rel.backward_commit) {
        int cur_dim = -1;
        int cur_out_dim = -1;
        for (size_t i = 0; i < t->fold_dims.size(); ++i) {
          int output_fold_dim = get_output_fold_dim(&rel, i);
          if (cur_dim == -1 || t->fold_dims[i] != t->fold_dims[cur_dim]) {
            cur_dim = i;
            cur_out_dim = output_fold_dim;
          } else if (output_fold_dim != cur_out_dim) {
            UpdateFoldDim(t, cur_dim, i - 1);     
            cur_dim = i;
            cur_out_dim = output_fold_dim;
          }
        }
        rel.backward_commit = rel.to->update;
      }
    }
    forward_visited_.erase(t);
  }

  void PropagationForward(FoldTensor *top, Relation &relation) {
    auto get_input_fold_dim = [top, &relation](int i) -> int {
      int input_fold_dim = relation.backward_mapping[i];
      if (input_fold_dim >= 0) {
        input_fold_dim = top->fold_dims[input_fold_dim];
      }
      return input_fold_dim; 
    };
    FoldTensor* t = relation.to;
#if FOLD_DIM_DUMP
    std::cout<<"[PropagationForward] "<<top<<" -> "<<t<<std::endl;
#endif
    if (forward_visited_.count(t)) {
      return;
    }
    if (top->update > relation.forward_commit) {
      int cur_dim = -1;
      int cur_in_dim = -1;
      for (size_t i = 0; i < t->fold_dims.size(); ++i) {
        int input_fold_dim = get_input_fold_dim(i);
        if (cur_dim == -1 || t->fold_dims[i] != t->fold_dims[cur_dim]) {
          cur_dim = i;
          cur_in_dim = input_fold_dim;
        } else if (input_fold_dim != cur_in_dim) {
          UpdateFoldDim(t, cur_dim, i - 1);     
          cur_dim = i;
          cur_in_dim = input_fold_dim;
        }
      }
      relation.forward_commit = top->update;
    }
    Propagation(t);
    backward_visited_.insert(t);
  }

  void UpdateFoldDim(FoldTensor *t, int start, int end) {
#if FOLD_DIM_DUMP
    auto old = t->fold_dims; //for print
#endif
    auto &fold = t->fold_dims;
    int fold_size = fold.size();
    int start_fold = fold[start];
    int i_start = start; 
    int split_num = 0;
    if (fold[i_start] != start) {
      while (i_start <= end && fold[i_start] == start_fold) {
        fold[i_start++] = start; 
      }
      split_num++;
    }
    if (i_start <= end) {
      int i_end = end; 
      int end_fold = fold[end];
      while (i_end - 1 >= i_start && fold[i_end - 1] == end_fold) {
        i_end--;
      }
      if (fold[i_end] != i_end) {
        for (int i = i_end; i <= end; ++i) {
          fold[i] = i_end;
        }
      }
    }
    int next_start = end + 1;
    if (next_start < fold_size && fold[next_start] != next_start) {
      int next_fold = fold[next_start];
      for (int i = next_start; i < fold_size && fold[i] == next_fold; ++i) {
        fold[i] = next_start;
      }
      split_num++;
    }
    if (split_num > 0) {
      folded_dims_ += split_num;
      t->update++;
    }
#if FOLD_DIM_DUMP
    std::cout<<"[UpdateFoldDim] "<<t<<" : ["<<start<<", "<<end<<"], old_dim=";
    for (int d : old) {
     std::cout<<d<<", ";
    }
    std::cout<<", new_dim=";
    for (int d : fold) {
     std::cout<<d<<", ";
    }
    std::cout<<std::endl;
#endif
  }
};

class DimensionFolder : public IRMutator {
 public:
  Stmt Fold(Stmt stmt) {
    DimensionFolderPlan plan;
    plan.Plan(stmt);
    if (plan.give_up_) {
      return stmt;
    }
    for (auto val : plan.tensors_) {
      fold_dims_.emplace(val.first,  val.second->fold_dims);
    }
    return IRMutator::Mutate(stmt);
  }

  Stmt Mutate_(const AttrStmt* op, const Stmt& s) {
    if (op->attr_key == "attrs") {
      update_attr_.clear();
      Stmt stmt = IRMutator::Mutate_(op, s);
      if (update_attr_.empty()) {
        return stmt;
      }
      op = stmt.as<AttrStmt>();
      auto attrs = Downcast<Map<std::string, NodeRef>>(op->node);
      if (update_attr_ == "axis") {
        Array<Expr> val = Downcast<Array<Expr>>(attrs["axis"]);
        Array<Expr> new_axis = FoldShapeIndex(attr_func_, val);
        attrs.Set("axis", new_axis);
      }else if (update_attr_ == "shape") {
        Array<Expr> val = Downcast<Array<Expr>>(attrs["shape"]);
        Array<Expr> new_shape = FoldShape(attr_func_, val);
        attrs.Set("shape", new_shape);
      }
      return AttrStmt::make(attrs, op->attr_key, op->value, op->body);
    }
    return IRMutator::Mutate_(op, s);
  }

  Stmt Mutate_(const Provide* op, const Stmt& s) {
    auto prim_op = op->value.as<Call>();
    CHECK(prim_op);
    Array<Expr> args;
    bool is_reduce = IsReduce(prim_op->name);
    for (const auto &arg : prim_op->args) {
      if (auto tensor = arg.as<Call>()) {
        if (is_reduce) {
          update_attr_ = "axis";
          attr_func_ = tensor->func;
        }
        auto shape = FoldShape(tensor->func, tensor->args);
        args.push_back(Call::make(tensor->type, tensor->name,
                       shape, tensor->call_type, tensor->func));
      } else {
        args.push_back(arg);
      }
    }
    if (prim_op->name == "BroadcastTo") {
      update_attr_ = "shape";
      attr_func_ = op->func;
    }
    auto prim_expr = Call::make(prim_op->type, prim_op->name, args, prim_op->call_type, prim_op->func);
    auto output_shape = FoldShape(op->func, op->args);
    return Provide::make(op->func, op->value_index, prim_expr, output_shape);
  }

 private:
  Array<Expr> FoldShape(FunctionRef tensor, Array<Expr> shape) {
    std::vector<int> &fold_dim = fold_dims_[tensor];
    CHECK(shape.size() == fold_dim.size());
    Array<Expr> fold_shape;
    Expr val = shape[0];
    for (int i = 1; i < static_cast<int>(fold_dim.size()); ++i) {
      if (i == fold_dim[i]) {
        fold_shape.push_back(Simplify(val));
        val = shape[i];
      } else {
        val = val * shape[i];
      }
    }
    fold_shape.push_back(Simplify(val));
    return fold_shape;
  }

  Array<Expr> FoldShapeIndex(FunctionRef tensor, Array<Expr> axis) {
    std::vector<int> &dim_fold = fold_dims_[tensor];
    std::vector<int> axis_map;
    int axis_idx = -1;
    for (int i = 0; i < static_cast<int>(dim_fold.size()); ++i) {
      if (dim_fold[i] == i) {
        axis_idx++; 
      } 
      axis_map.push_back(axis_idx);
    }
    Array<Expr> new_axis;
    std::unordered_set<int> included;
    for (Expr a : axis) {
      auto val = a.as<IntImm>();
      CHECK(val != nullptr); 
      int dim = axis_map[val->value];
      if (!included.count(dim)) {
        new_axis.push_back(make_const(Int(32), dim));
        included.insert(dim);
      }
    }
    return new_axis;
  }

  std::unordered_map<FunctionRef, std::vector<int>, NodeHash, NodeEqual> fold_dims_;
  std::string update_attr_;
  FunctionRef attr_func_;
};

class AxisAttrNormalizer : public IRMutator {
 public:
  Stmt Mutate_(const AttrStmt* op, const Stmt& s) {
    if (op->attr_key == "attrs") {
      auto attrs = Downcast<Map<std::string, NodeRef>>(op->node);
      if (attrs.find("axis") != attrs.end()) {
        Stmt body = IRMutator::Mutate(op->body);
        if (attrs["axis"].as<IntImm>()) {
          int64_t idx = axis_len_ > 0 ? (attrs["axis"].as<IntImm>()->value + axis_len_) % axis_len_ : 0;
          Array<Expr> new_axis = {make_const(Int(32), idx)};
          attrs.Set("axis", new_axis);
        } else {
          Array<Expr> axis = Downcast<Array<Expr>>(attrs["axis"]); 
          Array<Expr> new_axis;
          for (auto val : axis) {
            auto imm = val.as<IntImm>();
            CHECK(imm);
            if (imm->value >= 0) {
              new_axis.push_back(val);
            } else {
              new_axis.push_back(make_const(Int(32), imm->value + axis_len_));
            }
            attrs.Set("axis", new_axis);
          }
        }
        return AttrStmt::make(attrs, op->attr_key, op->value, body);
      }
    }
    return s;
  }

  Stmt Mutate_(const Provide* op, const Stmt& s) {
    auto prim = op->value.as<Call>();
    CHECK(prim && prim->args.size() > 0);
    auto input = prim->args[0].as<Call>();
    CHECK(input);
    axis_len_ = input->args.size();
    return s;
  }

 private:
  int64_t axis_len_{0};
};

Stmt AxisAttrNormalize(Stmt stmt) {
  return AxisAttrNormalizer().Mutate(stmt);
}

Stmt FoldDimension(Stmt stmt) {
  return DimensionFolder().Fold(stmt);
}

} // namespace akg

