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
################################################
"""
import datetime
import os
import pytest
from base import TestBase
from nose.plugins.attrib import attr
from test_run.dynamic_stitch_run import dynamic_stitch_run


############################################################
# TestCase= class: put to tests/*/
############################################################
class TestCase(TestBase):

    def setup(self):
        case_name = "test_akg_dynamic_stitch_001"
        case_path = os.getcwd()
        self.params_init(case_name, case_path)
        self.caseresult = True
        self._log.info("============= {0} Setup case============".format(self.casename))
        self.testarg = [
            ## testflag,opfuncname,testRunArgs, dimArgs
            ('dynamic_stitch_001', dynamic_stitch_run, ([8], [8, 8], "int32", 'float32'), ((16, 1), (16, 1))),
            ('dynamic_stitch_002', dynamic_stitch_run, ([16], [16], "int32", 'float16'), ((16, 1), (16, 1))),
            ('dynamic_stitch_003', dynamic_stitch_run, ([16], [16, 16], "int32", 'float16'), ((256, 1), (256, 1))),
            ('dynamic_stitch_004', dynamic_stitch_run, ([8], [8, 8, 8], "int32", 'float32'), ((16, 1), (16, 1), (16, 1))),
            ('dynamic_stitch_005', dynamic_stitch_run, ([8], [8, 8, 8, 1], "int32", 'float32'), ((16, 1), (16, 1), (16, 1), (256, 1))),
        ]
        return

    def test_run(self):
        self.common_run(self.testarg)

    def teardown(self):
        """
        clean environment
        :return:
        """
        self._log.info("============= {0} Teardown============".format(self.casename))
        return


if __name__ == "__main__":
    a = TestCase()
    a.setup()
    a.test_run()
    a.teardown()
