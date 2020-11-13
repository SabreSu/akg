# Copyright 2020 Huawei Technologies Co., Ltd
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License

""" XLA fused operator No.1420"""
from __future__ import absolute_import
import numpy as np
import akg
import akg.topi as topi
from gen_random import random_gaussian
from akg.utils import kernel_exec as utils
from comm_functions import test_single_out, test_multi_out
from akg.topi.cuda.reduce_opt import schedule_reduce
from akg.topi.cuda.injective_single_kernel import schedule_injective

def compute_topi(param_0, param_1, param_2, param_3, param_4, param_5, param_6, param_7):
    const_0 = topi.full([4096, 768], "float32", 1.11111)
    const_1 = topi.full([4096, 1], "float32", 2)
    const_2 = topi.full([4096, 1], "float32", 0.00130208)
    const_3 = topi.full([4096], "float32", -0.5)
    const_4 = topi.full([4096, 768], "float32", 0.1)

    brd_3530 = topi.expand_dims(param_5, axis=1, num_newaxis=1)
    brd_3528 = topi.expand_dims(param_7, axis=0, num_newaxis=1)
    mul_4041 = topi.multiply(brd_3530, brd_3528)
    btc_475 = topi.reshape(param_6, [4096, 768])
    mul_3218 = topi.multiply(mul_4041, btc_475)
    btc_398 = topi.reshape(param_5, [4096, 1])
    mul_3217 = topi.multiply(btc_398, btc_398)
    mul_3216 = topi.multiply(mul_3217, btc_398)
    mul_3215 = topi.multiply(param_4, const_3)
    btc_397 = topi.expand_dims(mul_3215, axis=1, num_newaxis=1)
    mul_3213 = topi.multiply(mul_3216, btc_397)
    mul_3212 = topi.multiply(const_2, mul_3213)
    mul_3210 = topi.multiply(const_1, mul_3212)
    btc_746 = topi.broadcast_to(mul_3210, [4096, 768])
    tmp = topi.expand_dims(param_3, axis=1, num_newaxis=1)
    mul_4293 = topi.multiply(tmp, const_2)
    mul_4293 = topi.broadcast_to(mul_4293, [4096, 768])
    sub_257 = topi.subtract(param_2, mul_4293)
    mul_3209 = topi.multiply(btc_746, sub_257)
    add_1491 = topi.add(mul_3218, mul_3209)
    mul_3208 = topi.multiply(topi.expand_dims(param_1, axis=1), const_2)
    add_1490 = topi.add(add_1491, mul_3208)
    mul_3207 = topi.multiply(const_0, add_1490)
    cmp_40 = topi.greater_equal(param_0, const_4)
    cvt_40 = topi.cast(cmp_40, "float32")
    mul_3206 = topi.multiply(mul_3207, cvt_40)
    red_633 = topi.sum(mul_3206, axis=(0))
    mul_4328 = topi.multiply(param_2, btc_475)
    neg_60 = topi.negative(btc_475)
    mul_4326 = topi.multiply(topi.broadcast_to(mul_4293, [4096, 768]), neg_60)
    add_1610 = topi.add(mul_4328, mul_4326)
    mul_2272 = topi.multiply(brd_3530, add_1610)
    red_223 = topi.sum(mul_2272, axis=(0))

    return [red_633, mul_3206, add_1490, red_223]

def compute_expect(param_0, param_1, param_2, param_3, param_4, param_5, param_6, param_7):
    const_0 = np.full([4096, 768], 1.11111, "float32")
    const_1 = np.full([4096, 1], 2, "float32")
    const_2 = np.full([4096, 1], 0.00130208, "float32")
    const_3 = np.full([4096], -0.5, "float32")
    const_4 = np.full([4096, 768], 0.1, "float32")

    brd_3530 = np.expand_dims(param_5, axis=1)
    brd_3528 = np.expand_dims(param_7, axis=0)

    mul_4041 = np.multiply(brd_3530, brd_3528)
    btc_475 = np.reshape(param_6, [4096, 768])
    mul_3218 = np.multiply(mul_4041, btc_475)
    btc_398 = np.reshape(param_5, [4096, 1])
    mul_3217 = np.multiply(btc_398, btc_398)
    mul_3216 = np.multiply(mul_3217, btc_398)
    mul_3215 = np.multiply(param_4, const_3)
    btc_397 = np.expand_dims(mul_3215, axis=1)
    mul_3213 = np.multiply(mul_3216, btc_397)
    mul_3212 = np.multiply(const_2, mul_3213)
    mul_3210 = np.multiply(const_1, mul_3212)
    btc_746 = np.broadcast_to(mul_3210, [4096, 768])
    tmp = np.expand_dims(param_3, axis=1)
    mul_4293 = np.multiply(tmp, const_2)
    mul_4293 = np.broadcast_to(mul_4293, [4096, 768])
    sub_257 = np.subtract(param_2, mul_4293)
    mul_3209 = np.multiply(btc_746, sub_257)
    add_1491 = np.add(mul_3218, mul_3209)
    mul_3208 = np.multiply(np.expand_dims(param_1, axis=1), const_2)
    add_1490 = np.add(add_1491, mul_3208)
    mul_3207 = np.multiply(const_0, add_1490)
    cmp_40 = np.greater_equal(param_0, const_4)
    cvt_40 = cmp_40.astype("float32")
    mul_3206 = np.multiply(mul_3207, cvt_40)
    red_633 = np.sum(mul_3206, axis=(0))
    mul_4328 = np.multiply(param_2, btc_475)
    neg_60 = np.negative(btc_475)
    mul_4326 = np.multiply(np.broadcast_to(mul_4293, [4096, 768]), neg_60)
    add_1610 = np.add(mul_4328, mul_4326)
    mul_2272 = np.multiply(brd_3530, add_1610)
    red_223 = np.sum(mul_2272, axis=(0))


    return [red_633, mul_3206, add_1490, red_223]


def compute_1420_auto(param_0, param_1, param_2, param_3, param_4, param_5, param_6, param_7):
    return compute_topi(param_0, param_1, param_2, param_3, param_4, param_5, param_6, param_7)

@akg.schedule(schedule_injective)
def compute_1420_manual(param_0, param_1, param_2, param_3, param_4, param_5, param_6, param_7):
    return compute_topi(param_0, param_1, param_2, param_3, param_4, param_5, param_6, param_7)

def gen_data(shape_0, shape_1, shape_2, shape_3, shape_4, shape_5, shape_6, shape_7, dtype):
    support_list = {"float16": np.float16, "float32": np.float32}
    param_0 = random_gaussian(shape_0, miu=1, sigma=0.1).astype(support_list[dtype])
    param_1 = random_gaussian(shape_1, miu=1, sigma=0.1).astype(support_list[dtype])
    param_2 = random_gaussian(shape_2, miu=1, sigma=0.1).astype(support_list[dtype])
    param_3 = random_gaussian(shape_3, miu=1, sigma=0.1).astype(support_list[dtype])
    param_4 = random_gaussian(shape_4, miu=1, sigma=0.1).astype(support_list[dtype])
    param_5 = random_gaussian(shape_5, miu=1, sigma=0.1).astype(support_list[dtype])
    param_6 = random_gaussian(shape_6, miu=1, sigma=0.1).astype(support_list[dtype])
    param_7 = random_gaussian(shape_7, miu=1, sigma=0.1).astype(support_list[dtype])

    expect = compute_expect(param_0, param_1, param_2, param_3, param_4, param_5, param_6, param_7)
    if isinstance(expect, (list, tuple)): 
            output = [np.full(np.shape(e), 0.0, e.dtype) for e in expect]
    else:
        output = np.full(np.shape(expect), 0.0, expect.dtype)
    input = [param_0, param_1, param_2, param_3, param_4, param_5, param_6, param_7]
    return input, output, expect

def test_compute_1420(shape_0, shape_1, shape_2, shape_3, shape_4, shape_5, shape_6, shape_7,
    dtype, multi_out=True, poly_sch=False):
    shape_list = [shape_0, shape_1, shape_2, shape_3, shape_4, shape_5, shape_6, shape_7]
    dtype_list = [dtype] * 8
    if poly_sch:
        mod = utils.op_build(compute_1420_auto, shape_list, dtype_list,
            attrs={"target":"cuda", "enable_akg_reduce_lib":True})
    else:    
        mod = utils.op_build(compute_1420_manual, shape_list, dtype_list)
    
    input, output, expect = gen_data(shape_0, shape_1, shape_2, shape_3, shape_4, shape_5, shape_6,
    shape_7, dtype)
    if multi_out:
        test_multi_out(mod, input, output, expect)
    else:
        test_single_out(mod, input, output, expect)

if __name__ == "__main__":
    test_compute_1420((4096, 768), (4096,), (4096, 768), (4096,), (4096,), (4096,),
    (32, 128, 768),  (768,), 'float32', poly_sch=False)
    test_compute_1420((4096, 768), (4096,), (4096, 768), (4096,), (4096,), (4096,),
    (32, 128, 768),  (768,), 'float32', poly_sch=True)
