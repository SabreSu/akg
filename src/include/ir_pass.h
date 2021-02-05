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

#ifndef INCLUDE_AKG_IR_PASS_H_
#define INCLUDE_AKG_IR_PASS_H_

#include <tvm/expr.h>
#include <tvm/buffer.h>
#include <tvm/schedule.h>
#include <tvm/lowered_func.h>
#include <tvm.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>

namespace akg {
namespace ir {
/*!
 * \brief Simplify just the combiner of the given reduce node.
 *
 *  This function applies Simplify to the components of the top reduction's
 *  combiner, but not to the source or condition of the reduction.
 *  By default it also removes all components which are not used to
 *  compute the resulting value (the value_index-th value).
 *
 * \param expr The expression to be simplifed. Must be a reduce expression.
 * \param prune_unused_components Whether to remove components which are not really used.
 * \return Simplified expression.
 */
Expr SimplifyCombiner(const Expr &expr, bool prune_unused_components = true);

/*!
 * \brief rewrite tensor.value[0] to tensor_v0.
 *
 * \param stmt The stmt to be transformed
 * \return Transformed stmt.
 */
Stmt RewriteMultiValueFunc(Stmt stmt);

/*!
 * \brief Rename the attr in LocalUB.
 *
 * \param stmt The stmt to be transformed
 * \return Transformed stmt.
 */
Stmt RenameRealize(Stmt stmt, const Map<Tensor, Buffer> &extern_buffer, const Map<Tensor, Tensor> &replace);

Array<NodeRef> AutoPoly(const Stmt &body, const Map<Tensor, Buffer> &extern_buffer,
                        const Map<std::string, NodeRef> &attrs, const bool is_specgemm, const bool is_dynamic);

NodeRef GenTuningSpace(const Stmt &body, const Map<Tensor, Buffer> &extern_buffer,
                       const Map<std::string, NodeRef> &attrs, const bool is_specgemm);

Expr CastNormalize(const Expr &expr, const air::DataType cast_type);

/*!
 * \brief Simplify expr using custom cce simplifiers.
 *
 * \param expr The expression to be simplified.
 * \return The result.
 *
 * \note Analyzer will call into sub-analyzers to get the result.
 */

Expr Simplify_cce(Expr expr, const Map<Var, Range> &vrange = Map<Var, Range>());
/*!
 * \brief Simplify stmt using custom cce simplifiers.
 *
 * \param expr The statement to be simplified.
 * \return The result.
 *
 * \note Analyzer will call into sub-analyzers to get the result.
 */
Stmt Simplify_cce(const Stmt &stmt, const Map<Var, Range> &vrange = Map<Var, Range>());

Stmt ElementwiseFlatten(Stmt stmt, const Map<Tensor, Buffer> &extern_buffer,
                        const Map<Tensor, Buffer> &new_extern_buffer);

Expr CastNormalize(const Expr &expr, const air::DataType cast_type);

Stmt TestInferBoundWithCond(const Expr &expr, const Array<Expr> &constraints);

Stmt TestReduceInequality(const air::Expr &e, const Var &reduce_var, bool scale, bool getlarger);

Stmt TestSimplify(const Expr &expr);

Stmt TestCanProveWithPosParam(const air::Expr &e);
}  // namespace ir
}  // namespace akg

namespace air {
namespace ir {
/** Substitute variables with the given pointer with the replacement
 * expression within expr. */
Expr substitute(const Variable *var, Expr replacement, Expr expr);

/** Substitute variables with the given pointer with the replacement
 * expression within stmt. */
Stmt substitute(const Variable *var, Expr replacement, Stmt stmt);

inline Expr substitute(const VarExpr &var, const Expr replacement, const Expr expr) {
  return substitute(var.get(), replacement, expr);
}

inline Stmt substitute(const VarExpr &var, const Expr replacement, const Stmt stmt) {
  return substitute(var.get(), replacement, stmt);
}

/** Substitute variables with pointers in the map. */
// @{
Expr substitute(const std::map<const Variable *, Expr> &replacements, Expr expr);
Stmt substitute(const std::map<const Variable *, Expr> &replacements, Stmt stmt);
// @}

/** Substitute expressions for other expressions. */
// @{
Expr substitute(Expr find, Expr replacement, Expr expr);
Stmt substitute(Expr find, Expr replacement, Stmt stmt);
// @}

/* align_partition.cc needs to call this function from tvm */
Stmt AppendStmts(const Stmt &a, const Stmt &b);

/* simplify_passes_cce.cc needs to call this function from tvm */
bool ExprUseVars(const Expr &expr, const std::unordered_set<const Variable *> &vars);

/*!
 * \brief partition loops in the stmt
 * \param stmt The stmt to do loop partition
 * \param split_const_loop flag to enable partition for const loop
 * \param remove_div_mod removes the division and modulo in the indexing of a tensor by partitioning the loop
 * \param partition_conv: whether to partition the convolution or not
 * \return Transformed stmt.
 */
Stmt LoopPartitionCCE(Stmt stmt, bool split_const_loop, bool remove_div_mod = false, bool partition_conv = false);
}  // namespace ir
}  // namespace air

#endif  // INCLUDE_AKG_IR_PASS_H_
