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
################################################

Testcase_PrepareCondition:

Testcase_TestSteps:

Testcase_ExpectedResult:
################################################
"""
import datetime
import os

from base import TestBase
import pytest
from test_run.mul_ad_run import mul_ad_run


############################################################
# TestCase= class: put to tests/*/
############################################################
class TestCase(TestBase):
    def setup(self):
        case_name = "test_akg_mul_001"
        case_path = os.getcwd()

        # params init
        self.params_init(case_name, case_path)

        self.caseresult = True
        self._log.info("============= {0} Setup case============".format(self.casename))
        self.testarg = [
            #testflag,opfuncname,testRunArgs, dimArgs
            ("001_mul_2_2_fp16", mul_ad_run, ([2], [2], "float16", "cce_mul_fp16")),
            ("001b_mul_16_16_fp16", mul_ad_run, ([16], [16], "float16", "cce_mul_fp16")),
            ("002_mul_1024_1024_fp16", mul_ad_run, ([1024], [1024], "float16", "cce_mul_fp16")),
            ("003_mul_4096_4096_fp16", mul_ad_run, ([4096], [4096], "float16", "cce_mul_fp16")),
            ("004_mul_30522_30522_fp16", mul_ad_run, ([30522], [30522], "float16", "cce_mul_fp16")),
            ("005_mul_2_1024_2_1024_fp16", mul_ad_run, ([2, 1024], [2, 1024], "float16", "cce_mul_fp16")),
            ("006_mul_160_1024_160_1024_fp16", mul_ad_run, ([160, 1024], [160, 1024], "float16", "cce_mul_fp16")),
            ("007_mul_512_1024_512_1024_fp16", mul_ad_run, ([512, 1024], [512, 1024], "float16", "cce_mul_fp16")),
            ("008_mul_1024_1024_1024_1024_fp16", mul_ad_run, ([1024, 1024], [1024, 1024], "float16", "cce_mul_fp16")),
            ("009_mul_1280_1024_1280_1024_fp16", mul_ad_run, ([1280, 1024], [1280, 1024], "float16", "cce_mul_fp16")),
            ("010_mul_4096_1024_4096_1024_fp16", mul_ad_run, ([4096, 1024], [4096, 1024], "float16", "cce_mul_fp16")),
            ("011_mul_4096_1024_4096_1024_fp16", mul_ad_run, ([4096, 1024], [4096, 1024], "float16", "cce_mul_fp16")),
        ]
        self.testarg_level1 = [

        ]
        return

    def test_run(self):
        self.common_run(self.testarg)

    def test_run_level1(self):
        self.common_run(self.testarg_level1)

    def teardown(self):
        """
        clean environment
        :return:
        """
        self._log.info("============= {0} Teardown============".format(self.casename))
        return
