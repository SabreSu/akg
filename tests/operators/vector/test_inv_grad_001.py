# Copyright 2020-2021 Huawei Technologies Co., Ltd
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
################################################

Testcase_PrepareCondition:

Testcase_TestSteps:

Testcase_ExpectedResult:

"""
import os
import pytest
from nose.plugins.attrib import attr
from base import TestBase
from test_run.inv_grad_run import inv_grad_run

############################################################
# TestCase= class: put to tests/*/
############################################################


class TestCase(TestBase):

    def setup(self):
        """set test case """
        case_name = "test_inv_grad_001"
        case_path = os.getcwd()
        self.params_init(case_name, case_path)
        self.caseresult = True
        self._log.info("============= {0} Setup case============".format(self.casename))
        self.testarg = [
            # testflag,opfuncname,testRunArgs, setdimArgs
            ("inv_grad_001", inv_grad_run, ((16,),  "float16")),
            ("inv_grad_002", inv_grad_run, ((16, 16), "float32")),
            ("inv_grad_003", inv_grad_run, ((16, 16, 16), "int32")),
            ("inv_grad_004", inv_grad_run, ((6,), "int8")),

        ]
        self.testarg_rpc_cloud = [
            # testflag,opfuncname,testRunArgs, setdimArgs
            ("inv_grad_001", inv_grad_run, ((16,),  "float16")),
            ("inv_grad_002", inv_grad_run, ((16, 16), "float32")),
            ("inv_grad_003", inv_grad_run, ((16, 16, 16), "int32")),
            ("inv_grad_004", inv_grad_run, ((6,), "int8")),

        ]

    def test_run(self):
        """
        run case.#
        :return:
        """
        self.common_run(self.testarg)

    def test_run_rpc_cloud(self):
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
