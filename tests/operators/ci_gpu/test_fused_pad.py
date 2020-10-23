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

import numpy as np
from gen_random import random_gaussian
from akg.utils import kernel_exec as utils
from akg.utils.result_analysis import gpu_profiling
from akg.utils.format_transform import to_tvm_nd_array
from akg.ops.poly_gpu import fused_pad_manual, fused_pad_auto


def gen_data(shape, pad_before, pad_after, layout='NHWC', pad_value=0.0):
    support_list = {"float16": np.float16, "float32": np.float32}
    data = random_gaussian(shape, miu=1, sigma=0.1).astype('float32')
    if layout == "NCHW":
        data = np.transpose(data, axes=(0, 2, 3, 1))
    pad_width = list(zip(pad_before, pad_after))
    data_cast = data.astype('float16')
    expect = np.pad(data_cast, pad_width, 'constant', constant_values=(pad_value, pad_value))
    output = np.full(np.shape(expect), np.nan, 'float16') 
    return data, output, expect

def test_fused_pad(shape, pad_before, pad_after, layout='NHWC', pad_value=0.0, poly_sch=False):
    op_attrs = [pad_before, pad_after, layout, pad_value]
    if poly_sch:
        mod = utils.op_build_test(fused_pad_auto, [shape], ['float32'], kernel_name="fused_pad_auto", op_attrs=op_attrs, attrs={"target": "cuda"})
    else:
        mod = utils.op_build_test(fused_pad_manual, [shape], ['float32'], kernel_name="fused_pad_manual", op_attrs=op_attrs)
    data, output, expect = gen_data(shape, pad_before, pad_after, layout, pad_value)
    args = (data, output)
    output = utils.mod_launch(mod, args, expect = expect)
    res = np.allclose(output, expect, rtol=5e-03, atol=1.e-8)
    print("Test {}".format("Pass" if res else "Fail"))
    if not res:
        print("Error cuda:========================")
        print(mod.imported_modules[0].get_source())
        raise AssertionError("Test fail")
    
    data = to_tvm_nd_array(data)
    expect = to_tvm_nd_array(expect)
    return True
