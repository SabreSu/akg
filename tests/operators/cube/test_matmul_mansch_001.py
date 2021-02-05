# Copyright 2019-2021 Huawei Technologies Co., Ltd
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

"""
matmul
"""
import datetime
import os
import pytest
from base import TestBase
from nose.plugins.attrib import attr
from test_run.matmul_run_mansch import matmul_run_mansch


class TestCase(TestBase):

    def setup(self):
        case_name = "test_akg_matmul_001"
        case_path = os.getcwd()
        self.params_init(case_name, case_path)
        self.caseresult = True
        self._log.info("============= {0} Setup case============".format(self.casename))
        self.testarg = [
            # caseflag,opfuncname,testRunArgs, dimArgs
            # (M,K,N), L1_tiling, L0_tiling, kernel_name
            # m-n tiling
            ("matmul_run_mansch_0", matmul_run_mansch, ((192, 256, 192), (80, 64, 80), (32, 64, 32),
                                                        "matmul_mansch_1")),
            # # k-tiling
            # ("matmul_run_mansch_1", matmul_run_mansch, ((192, 256, 160), (80, 128, 32), (80, 64, 32),
            #                                             "matmul_mansch_1")),
        ]

        self.testarg_rpc_cloud = [
            ("matmul_run_mansch_0", matmul_run_mansch, ((192, 256, 192), (80, 64, 80), (32, 64, 32),
                                                        "matmul_mansch_1")),
            # k-tiling
            ("matmul_run_mansch_1", matmul_run_mansch, ((192, 256, 160), (80, 128, 32), (80, 64, 32),
                                                        "matmul_mansch_1")),
        ]

        return

    def test_run(self):
        """
        run case.#
        :return:
        """
        self.common_run(self.testarg)

    def test_rpc_cloud(self):
        """
        run case.#
        :return:
        """
        self.common_run(self.testarg_rpc_cloud)

    def teardown(self):
        """
        clean environment
        :return:
        """
        self._log.info("============= {0} Teardown============".format(self.casename))
        return
