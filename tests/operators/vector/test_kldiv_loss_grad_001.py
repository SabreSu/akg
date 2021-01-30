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
kldiv_loss_grad
"""
from test_run.kldiv_loss_grad_run import kldiv_loss_grad_run
import os
from base import TestBase
import pytest


class TestCase(TestBase):
    def setup(self):
        case_name = "test_auto_kldiv_loss_grad_001"
        case_path = os.getcwd()

        # params init
        self.params_init(case_name, case_path)

        self.caseresult = True
        self._log.info("============= {0} Setup case============".format(self.casename))
        self.testarg = [
            #caseflag,opfuncname,testRunArgs, dimArgs
            ("kldiv_loss_grad_none_run1", kldiv_loss_grad_run, ((2, 2), "float16", "kldiv_loss_grad_output")),
            ("kldiv_loss_grad_none_run2", kldiv_loss_grad_run, ((2, 2, 2), "float16", "kldiv_loss_grad_output")),
            ("kldiv_loss_grad_none_run3", kldiv_loss_grad_run, ((2, 128, 128), "float16", "kldiv_loss_grad_output")),
        ]
        self.testarg_cloud = [
            #caseflag,opfuncname,testRunArgs, dimArgs
            ("kldiv_loss_grad_none_cloud_run1", kldiv_loss_grad_run, ((4, 2), "float16", "kldiv_loss_grad_output")),
            ("kldiv_loss_grad_none_cloud_run2", kldiv_loss_grad_run, ((2, 2, 2), "float16", "kldiv_loss_grad_output")),
            ("kldiv_loss_grad_none_cloud_run3", kldiv_loss_grad_run, ((2, 128, 128), "float16", "kldiv_loss_grad_output")),
        ]
        self.testarg_aic = [
            #caseflag,opfuncname,testRunArgs, dimArgs
            ("kldiv_loss_grad_none_cloud_run0", kldiv_loss_grad_run, ((4, 2), "float16", "kldiv_loss_grad_output")),
            ("kldiv_loss_grad_mean_cloud_run1", kldiv_loss_grad_run, ((4, 2, 2), "float16", "kldiv_loss_grad_output")),
        ]
        return

    def test_run(self):
        """
        run case.#
        :return:
        """
        self.common_run(self.testarg)

    def test_run_aicmodel(self):
        """
        run case.#
        :return:
        """
        self.common_run(self.testarg_aic)

    def test_run_cloud(self):
        """
        run case.#
        :return:
        """
        self.common_run(self.testarg_cloud)

    def teardown(self):
        """
        clean environment
        :return:
        """
        self._log.info("============= {0} Teardown============".format(self.casename))
        return

# if __name__ == "__main__":
#    t = TestCase()
#    t.setup()
#    t.test_run()
#    t.teardown()
