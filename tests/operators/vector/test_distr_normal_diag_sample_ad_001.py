# Copyright 2019 Huawei Technologies Co., Ltd
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
import datetime
import os
import pytest
from base import TestBase
from nose.plugins.attrib import attr
from test_run.distr_normal_diag_sample_ad_run import sample_ad_run

############################################################
# TestCase= class: put to tests/*/
############################################################


class TestCase(TestBase):

    def setup(self):
        case_name = "test_akg_dist_normal_diag_ad_001"
        case_path = os.getcwd()
        self.params_init(case_name, case_path)
        self.caseresult = True
        self._log.info("============= {0} Setup case============".format(self.casename))
        self.testarg = [
            #caseflag, testfuncname, testRunArgs
            ("001_dist_normal_diag_sample_ad_3_4", sample_ad_run, ((3, 4), "float16", "cce_normal_diag_sample_ad_fp16")),
            ("002_dist_normal_diag_sample_ad_3_4", sample_ad_run, ((100, 2), "float16", "cce_normal_diag_sample_ad_fp16")),
        ]
        self.testarg_cloud = [
            #caseflag,testfuncname,testRunArgs, dimArgs
        ]
        return

    def test_run(self):
        self.common_run(self.testarg)

    def test_run_cloud(self):
        self.common_run(self.testarg_cloud)

    def teardown(self):

        self._log.info("============= {0} Teardown============".format(self.casename))
        return
