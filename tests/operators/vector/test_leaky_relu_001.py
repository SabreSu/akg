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
# limitations under the License.

"""leaky_relu testcases"""

import os
from base import TestBase
import pytest


class TestCase(TestBase):
    def setup(self):
        case_name = "test_leaky_relu_001"
        case_path = os.getcwd()

        # params init
        self.params_init(case_name, case_path)

        self.caseresult = True
        self._log.info("============= {0} Setup case============".format(self.casename))
        self.testarg = [
            # testflag,opfuncname,testRunArgs, setdimArgs
            # shape, dtype, negative_slop
            ("leaky_relu_001", "leaky_relu_run", ((1, 1, 1, 16), "float16", 0),),
            ("leaky_relu_002", "leaky_relu_run", ((1, 1, 1, 1, 16), "float32", 0.5),),

        ]
        self.testarg_cloud = [
            # testflag, opfuncname, testRunArgs, setdimArgs
            # shape, dtype, negative_slop
            ("001_leaky_relu", "leaky_relu_run", ((1, 1, 1, 16), "float32", 0.5)),
        ]

        self.testarg_rpc_cloud = [
            # testflag, opfuncname, testRunArgs, setdimArgs
            # shape, dtype, negative_slop
            ("leaky_relu", "leaky_relu_run", ((1, 1, 1, 16), "float16", 0.5)),
            ("leaky_relu_013", "leaky_relu_run", ((1, 1, 1, 16), "float16", 0.5)),
        ]
        self.testarg_level1 = [
            # testflag, opfuncname, testRunArgs, setdimArgs
            # shape, dtype, negative_slop
            ("leaky_relu", "leaky_relu_run", ((1, 1, 1, 1, 16), "float16", 0.5 )),
        ]
        return

    def test_run(self):
        self.common_run(self.testarg)

    def test_run_cloud(self):
        self.common_run(self.testarg_cloud)

    def test_run_rpc_cloud(self):
        self.common_run(self.testarg_rpc_cloud)

    def test_run_level1(self):
        self.common_run(self.testarg_level1)

    def teardown(self):

        self._log.info("============= {0} Teardown============".format(self.casename))
        return
