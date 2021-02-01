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

"""xdivy test case"""

import os

from base import TestBase
import pytest
from test_run.xdivy_run import xdivy_run


class TestCos(TestBase):
    def setup(self):
        case_name = "test_akg_xdivy_001"
        case_path = os.getcwd()

        # params init
        self.params_init(case_name, case_path)

        self.caseresult = True
        self._log.info("========================{0}  Setup case=================".format(self.casename))
        self.testarg = [
            # testflag, opfuncname, (shape1, shape2, dtype) 
            ("xdivy_f16_01", xdivy_run, ((32, 16), (32, 16), "float16")),
            ("xdivy_f32_02", xdivy_run, ((32, 16), (32, 16), "float32")),
        ]
        return

    def test_run(self):
        """
        run case.#
        :return:
        """
        self.common_run(self.testarg)

    def teardown(self):
        """
        clean environment
        :return:
        """
        self._log.info("============= {0} Teardown============".format(self.casename))
        return
