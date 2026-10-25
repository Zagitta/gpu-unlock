# SPDX-License-Identifier: GPL-2.0-only
import ctypes as C
import importlib.util
from pathlib import Path
from unittest import TestCase, mock

spec = importlib.util.spec_from_file_location(
    "sm_rates", Path(__file__).resolve().parents[1] / "tools/read_sm_rates.py")
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class SmRateProbeTests(TestCase):
    def test_only_alloc_and_the_one_public_getter_are_sent(self):
        requests = []

        def ioctl(fd, code, buffer, mutate):
            requests.append(code)
            self.assertEqual(fd, 7)
            if code == probe.ioctl_number(0x2B, probe.Alloc):
                p = probe.Alloc.from_buffer(buffer)
                self.assertIn(p.cls, (0x41, 0x80, 0x2080))
                p.new = 100 + len(requests)
            elif code == probe.ioctl_number(0x2A, probe.Control):
                p = probe.Control.from_buffer(buffer)
                self.assertEqual(p.cmd, probe.GET_SM_RATES)
                rates = C.cast(p.params, C.POINTER(probe.Rates)).contents
                rates.fmla32 = 1
                rates.imla2 = 1
                rates.imla4 = 1
            else:
                self.fail("unexpected ioctl")

        with mock.patch.object(probe, "gpu_identity", return_value=["GPU-exact, test"]), \
             mock.patch.object(probe.os, "open", side_effect=[7, 8]), \
             mock.patch.object(probe.os, "close") as close:
            result = probe.read_rates("GPU-exact", ioctl)
        self.assertEqual(result["rates"]["fmla32"], 1)
        self.assertEqual(len(requests), 4)
        self.assertEqual(close.call_args_list, [mock.call(8), mock.call(7)])

    def test_wrong_identity_prevents_any_device_open(self):
        with mock.patch.object(probe, "gpu_identity", return_value=["GPU-other, test"]), \
             mock.patch.object(probe.os, "open") as open_device:
            with self.assertRaisesRegex(RuntimeError, "exact supplied UUID"):
                probe.read_rates("GPU-exact")
            open_device.assert_not_called()

    def test_custom_executable_is_forwarded_to_identity_query(self):
        with mock.patch.object(probe, "gpu_identity", return_value=["GPU-exact, test"]) as ident, \
             mock.patch.object(probe.os, "open") as open_device:
            with self.assertRaisesRegex(RuntimeError, "exact supplied UUID"):
                probe.read_rates("GPU-other", executable="/opt/driver/bin/nvidia-smi")
            ident.assert_called_once_with("/opt/driver/bin/nvidia-smi")
            open_device.assert_not_called()

    def test_failed_getter_does_not_report_zero_as_full_speed(self):
        def ioctl(fd, code, buffer, mutate):
            if code == probe.ioctl_number(0x2B, probe.Alloc):
                probe.Alloc.from_buffer(buffer).new = 1
            else:
                probe.Control.from_buffer(buffer).status = 0x56

        with mock.patch.object(probe, "gpu_identity", return_value=["GPU-exact, test"]), \
             mock.patch.object(probe.os, "open", side_effect=[7, 8]), \
             mock.patch.object(probe.os, "close") as close:
            with self.assertRaisesRegex(RuntimeError, "status 0x56"):
                probe.read_rates("GPU-exact", ioctl)
        self.assertEqual(close.call_count, 2)

    def test_custom_executable_is_used_without_a_shell(self):
        with mock.patch.object(probe.subprocess, "run") as run:
            run.return_value.stdout = "GPU-exact, test\n"
            self.assertEqual(probe.gpu_identity("/opt/nvidia/bin/nvidia-smi"),
                             ["GPU-exact, test"])
        args, kwargs = run.call_args
        self.assertEqual(args[0][0], "/opt/nvidia/bin/nvidia-smi")
        self.assertNotIn("shell", kwargs)
        self.assertTrue(kwargs["check"])

    def test_second_open_failure_closes_first_descriptor(self):
        with mock.patch.object(probe, "gpu_identity", return_value=["GPU-exact, test"]), \
             mock.patch.object(probe.os, "open", side_effect=[7, PermissionError]), \
             mock.patch.object(probe.os, "close") as close:
            with self.assertRaises(PermissionError):
                probe.read_rates("GPU-exact")
        close.assert_called_once_with(7)

    def test_abi_layouts_and_ioctl_numbers(self):
        self.assertEqual([C.sizeof(s) for s in
                          (probe.Alloc, probe.Control, probe.Device, probe.Rates)],
                         [48, 32, 56, 32])
        self.assertEqual(probe.Rates.imla0.offset, 16)
        self.assertEqual(probe.Rates.imla4.offset, 24)
        self.assertEqual(probe.ioctl_number(0x2B, probe.Alloc), 0xC030462B)
        self.assertEqual(probe.ioctl_number(0x2A, probe.Control), 0xC020462A)

    def test_rate_ranges_are_validated_per_field(self):
        expected = dict(zip(probe.RATE_FIELDS, (6, 5, 1, 5, 5, 6, 6, 6, 6)))
        for invalid_field in (None,) + probe.RATE_FIELDS:
            with self.subTest(invalid_field=invalid_field):
                def ioctl(fd, code, buffer, mutate):
                    if code == probe.ioctl_number(0x2B, probe.Alloc):
                        probe.Alloc.from_buffer(buffer).new = 1
                    else:
                        request = probe.Control.from_buffer(buffer)
                        rates = C.cast(request.params, C.POINTER(probe.Rates)).contents
                        for name, maximum in expected.items():
                            setattr(rates, name, maximum + (name == invalid_field))

                with mock.patch.object(probe, "gpu_identity", return_value=["GPU-exact, test"]), \
                     mock.patch.object(probe.os, "open", side_effect=[7, 8]), \
                     mock.patch.object(probe.os, "close"):
                    if invalid_field is None:
                        self.assertEqual(probe.read_rates("GPU-exact", ioctl)["rates"], expected)
                    else:
                        with self.assertRaisesRegex(RuntimeError, "unknown issue-rate"):
                            probe.read_rates("GPU-exact", ioctl)
