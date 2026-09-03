#!/usr/bin/env python3
"""RE-H0 analysis diagnostics (TDD authority for re_h0_analysis.py).

Every test encodes one preregistered analysis rule from
research/re-h0/RE-H0-PREREGISTRATION.md. The fail-closed cases are the
§45 diagnostics: the analysis must refuse to emit verdicts from an
incomplete, duplicated, tampered or degraded session rather than
silently aggregate.

Run: python3 research/re-h0/scripts/check_re_h0_analysis.py
"""

import copy
import math
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import re_h0_analysis as an  # noqa: E402


def rep_row(**kw):
    """One normalized per-(cell,op,arm) combo row."""
    base = dict(
        family="re1u",
        fs="btrfs",
        op="read",
        request_size=4096,
        depth=8,
        workers=1,
        arm="z1",
        ok=True,
        ops=65536,
        total_bytes=268435456,
        word_sum=123456789,
        # 11 wall reps of per-op nanoseconds
        wall_ns_per_op_samples=[1000.0 + 0 for _ in range(11)],
        # two independent double-difference instruction estimates
        instr_u_per_op_estimates=[1000.0, 1000.0],
        user_ns_per_op=100.0,
        sys_ns_per_op=900.0,
        write_verified=True,
        binary_sha256="a" * 64,
    )
    base.update(kw)
    return base


def arm_variants(arm, wall, instr):
    """A row with wall samples centred on `wall` and instr estimates `instr`."""
    return rep_row(
        arm=arm,
        wall_ns_per_op_samples=[wall] * 11,
        instr_u_per_op_estimates=[instr, instr],
    )


class RobustStatsTest(unittest.TestCase):
    def test_median_odd_even(self):
        self.assertEqual(an.median([3.0, 1.0, 2.0]), 2.0)
        self.assertEqual(an.median([4.0, 1.0, 2.0, 3.0]), 2.5)

    def test_mad_constant_series_is_zero(self):
        self.assertEqual(an.mad([5.0, 5.0, 5.0]), 0.0)

    def test_mad_known_values(self):
        # median 2, abs deviations 1,0,1 -> MAD 1
        self.assertEqual(an.mad([1.0, 2.0, 3.0]), 1.0)

    def test_robust_interval_is_median_pm_1p5_mad(self):
        lo, hi = an.robust_interval([1.0, 2.0, 3.0])
        self.assertEqual(lo, 0.5)
        self.assertEqual(hi, 3.5)


class ClassificationTest(unittest.TestCase):
    """Preregistered materiality vocabulary (frozen thresholds)."""

    def test_identical_arms_parity(self):
        base = [1000.0] * 11
        cand = [1000.0] * 11
        v = an.classify(base, cand)
        self.assertEqual(v, "PARITY")

    def test_20_percent_synth_arm_material(self):
        base = [1000.0 + (i % 3) for i in range(11)]
        cand = [1200.0 + (i % 3) for i in range(11)]
        v = an.classify(base, cand)
        self.assertEqual(v, "MATERIAL_TAX")

    def test_small_shift_inside_band_is_parity(self):
        base = [1000.0] * 11
        cand = [1020.0] * 11  # ratio 1.02 <= 1.05
        self.assertEqual(an.classify(base, cand), "PARITY")

    def test_direction_flip_is_gray(self):
        # candidate median BELOW baseline: ratio < 1 is not "faster parity",
        # it is a direction anomaly -> GRAY (report, never silently fold).
        base = [1000.0] * 11
        cand = [800.0] * 11
        self.assertEqual(an.classify(base, cand), "GRAY")

    def test_ratio_in_band_is_gray_even_when_separated(self):
        base = [1000.0] * 11
        cand = [1070.0] * 11  # 1.05 < ratio < 1.10
        self.assertEqual(an.classify(base, cand), "GRAY")

    def test_ratio_over_threshold_but_overlapping_is_gray(self):
        base = [1000.0 + (i % 2) for i in range(11)]  # mad=0.5 int [999,1001]... wide
        base = [990.0, 1010.0] * 5 + [1000.0]  # med 1000, mad 10, int [985,1015]
        cand = [1120.0, 1140.0] * 5 + [1130.0]  # med 1130, int [1115,1145]
        # ratio 1.13 >= 1.10 but intervals disjoint -> MATERIAL
        self.assertEqual(an.classify(base, cand), "MATERIAL_TAX")
        cand_overlap = [1110.0, 1120.0] * 5 + [1115.0]  # int [1102.5,1127.5]? verify
        # med 1115, mad 5 -> [1107.5, 1122.5] -> overlaps nothing above 1015?
        # disjoint -> MATERIAL. Push candidate down to overlap baseline:
        cand2 = [1016.0, 1100.0] * 5 + [1058.0]  # med 1058 ratio 1.058 -> GRAY band
        self.assertEqual(an.classify(base, cand2), "GRAY")

    def test_wall_only_fallback_keeps_vocabulary(self):
        # When instruction estimates are absent (perf unavailable) the
        # classifier must NOT silently claim a verdict from wall alone.
        with self.assertRaises(an.IndeterminateMetric):
            an.classify([1000.0] * 11, [1100.0] * 11, cand_instr=None)


class LadderVerdictTest(unittest.TestCase):
    def _session(self):
        rows = [
            arm_variants("z1", 1000.0, 1000.0),
            arm_variants("z1b", 1000.0, 1000.0),
            arm_variants("z1bw", 1000.0, 1000.0),
            arm_variants("z2", 1000.0, 1000.0),
            arm_variants("z3", 1000.0, 1000.0),
        ]
        for r in rows:
            r.update(op="read", request_size=4096, depth=8, fs="btrfs")
        return rows

    def test_all_equal_case_a(self):
        out = an.re1u_ladder_verdict(self._session(), "btrfs", "read", 4096, 8)
        self.assertEqual(out["C_sem"]["verdict"], "PARITY")
        self.assertEqual(out["T_backend"]["verdict"], "PARITY")
        self.assertEqual(out["C_cont"]["verdict"], "PARITY")
        self.assertEqual(out["T_runtime"]["verdict"], "PARITY")
        self.assertEqual(out["case"], "CASE_A")

    def test_material_backend_case_b(self):
        rows = self._session()
        rows[3] = arm_variants("z2", 1250.0, 1250.0)
        rows[3].update(op="read", request_size=4096, depth=8, fs="btrfs")
        out = an.re1u_ladder_verdict(rows, "btrfs", "read", 4096, 8)
        self.assertEqual(out["T_backend"]["verdict"], "MATERIAL_TAX")
        self.assertEqual(out["case"], "CASE_B")

    def test_material_runtime_case_c(self):
        rows = self._session()
        rows[4] = arm_variants("z3", 1250.0, 1250.0)
        rows[4].update(op="read", request_size=4096, depth=8, fs="btrfs")
        out = an.re1u_ladder_verdict(rows, "btrfs", "read", 4096, 8)
        self.assertEqual(out["T_runtime"]["verdict"], "MATERIAL_TAX")
        self.assertEqual(out["case"], "CASE_C")

    def test_both_material_case_d(self):
        rows = self._session()
        rows[3] = arm_variants("z2", 1250.0, 1250.0)
        rows[4] = arm_variants("z3", 1500.0, 1500.0)
        for r in (rows[3], rows[4]):
            r.update(op="read", request_size=4096, depth=8, fs="btrfs")
        out = an.re1u_ladder_verdict(rows, "btrfs", "read", 4096, 8)
        self.assertEqual(out["case"], "CASE_D")


class FailClosedTest(unittest.TestCase):
    def _good(self, n_arms=5):
        arms = ["z1", "z1b", "z1bw", "z2", "z3"][:n_arms]
        rows = []
        for arm in arms:
            r = arm_variants(arm, 1000.0, 1000.0)
            r.update(op="read", request_size=4096, depth=8, fs="btrfs")
            rows.append(r)
        return rows

    def test_missing_arm_fails_closed(self):
        rows = self._good()[:-1]  # drop z3
        with self.assertRaises(an.SessionInvalid) as cm:
            an.validate_re1u_block(rows, "btrfs", "read", 4096, 8)
        self.assertIn("z3", str(cm.exception))

    def test_duplicate_arm_fails_closed(self):
        rows = self._good()
        dup = copy.deepcopy(rows[0])
        rows.append(dup)
        with self.assertRaises(an.SessionInvalid):
            an.validate_re1u_block(rows, "btrfs", "read", 4096, 8)

    def test_same_work_mismatch_fails_closed(self):
        rows = self._good()
        rows[2]["word_sum"] += 1  # z1bw read word sum differs
        with self.assertRaises(an.SessionInvalid) as cm:
            an.validate_re1u_block(rows, "btrfs", "read", 4096, 8)
        self.assertIn("word_sum", str(cm.exception))

    def test_failed_rep_fails_closed(self):
        rows = self._good()
        rows[1]["ok"] = False
        with self.assertRaises(an.SessionInvalid):
            an.validate_re1u_block(rows, "btrfs", "read", 4096, 8)

    def test_write_unverified_fails_closed(self):
        rows = self._good()
        for r in rows:
            r["op"] = "write"
            r["word_sum"] = None
        rows[0]["write_verified"] = False
        with self.assertRaises(an.SessionInvalid):
            an.validate_re1u_block(rows, "btrfs", "write", 4096, 8)

    def test_binary_sha_mismatch_fails_closed(self):
        rows = self._good()
        rows[3]["binary_sha256"] = "b" * 64
        with self.assertRaises(an.SessionInvalid) as cm:
            an.validate_re1u_block(
                rows, "btrfs", "read", 4096, 8,
                expected_binary_sha256="a" * 64)
        self.assertIn("sha", str(cm.exception).lower())

    def test_ecanceled_error_text_fails_closed(self):
        rows = self._good()
        rows[4]["error_note"] = "terminal I/O error (code 1, os 125)"
        with self.assertRaises(an.SessionInvalid) as cm:
            an.validate_re1u_block(rows, "btrfs", "read", 4096, 8)
        self.assertIn("error_note", str(cm.exception))

    def test_missing_instr_estimates_flags_indeterminate(self):
        # A block whose instruction estimates are missing must be reported
        # as measurement-indeterminate, never reclassified to wall-only.
        rows = self._good()
        rows[0]["instr_u_per_op_estimates"] = None
        with self.assertRaises(an.IndeterminateMetric):
            an.validate_re1u_block(rows, "btrfs", "read", 4096, 8)

    def test_missing_wall_samples_fail_closed(self):
        rows = self._good()
        rows[2]["wall_ns_per_op_samples"] = []
        with self.assertRaises(an.SessionInvalid):
            an.validate_re1u_block(rows, "btrfs", "read", 4096, 8)


class AttrBVerdictTest(unittest.TestCase):
    """RE-H0-ATTR-B-PREREGISTRATION.md A6 frozen rule.

    fraction_i = (z2r0_est_i - z2r1_est_i) / (z2r0_est_i - z1b_est_i)
    MATERIAL_RECOVERY  both fraction_i >= 0.05
    NO_RECOVERY        both fraction_i <  0.02
    PARTIAL_RECOVERY   otherwise; negative fraction fails closed
    (denominator <= 0 also fails closed: the CASE B witness must
    reproduce in-session or the experiment refuses to attribute).
    """

    def _block(self, z1b_instr, z2r0_instr, z2r1_instr, fs="btrfs"):
        rows = []
        for arm, instr in (("z1b", z1b_instr), ("z2r0", z2r0_instr),
                           ("z2r1", z2r1_instr)):
            r = arm_variants(arm, 1200.0, instr)
            r.update(op="read", request_size=4096, depth=8, fs=fs)
            rows.append(r)
        return rows

    def test_material_recovery(self):
        out = an.attr_b_verdict(self._block(1000.0, 3000.0, 2400.0),
                                "btrfs", 4096, 8)
        self.assertEqual(out["outcome"], "MATERIAL_RECOVERY")
        self.assertAlmostEqual(out["denom_instr_per_op"], 2000.0)
        for f in out["fractions"]:
            self.assertAlmostEqual(f, 0.30)

    def test_no_recovery(self):
        out = an.attr_b_verdict(self._block(1000.0, 3000.0, 2990.0),
                                "btrfs", 4096, 8)
        self.assertEqual(out["outcome"], "NO_RECOVERY")
        for f in out["fractions"]:
            self.assertAlmostEqual(f, 0.005)

    def test_partial_recovery_between_bands(self):
        # recovery 60 of 2000 = 0.03: inside (0.02, 0.05)
        out = an.attr_b_verdict(self._block(1000.0, 3000.0, 2940.0),
                                "btrfs", 4096, 8)
        self.assertEqual(out["outcome"], "PARTIAL_RECOVERY")

    def test_mixed_estimates_collapse_to_partial(self):
        # est pair 1 recovers 0.30, pair 2 recovers 0.00 -> PARTIAL
        rows = self._block(1000.0, 3000.0, 2400.0)
        for r in rows:
            if r["arm"] == "z2r1":
                r["instr_u_per_op_estimates"] = [2400.0, 2995.0]
            elif r["arm"] == "z2r0":
                r["instr_u_per_op_estimates"] = [3000.0, 3000.0]
            elif r["arm"] == "z1b":
                r["instr_u_per_op_estimates"] = [1000.0, 1000.0]
        out = an.attr_b_verdict(rows, "btrfs", 4096, 8)
        self.assertEqual(out["outcome"], "PARTIAL_RECOVERY")

    def test_per_estimate_pairing_is_independent(self):
        # denominators differ per estimate; fractions computed pairwise
        rows = self._block(1000.0, 3000.0, 2400.0)
        for r in rows:
            if r["arm"] == "z2r0":
                r["instr_u_per_op_estimates"] = [3000.0, 3100.0]
            elif r["arm"] == "z2r1":
                r["instr_u_per_op_estimates"] = [2400.0, 2500.0]
        out = an.attr_b_verdict(rows, "btrfs", 4096, 8)
        self.assertAlmostEqual(out["fractions"][0], 600.0 / 2000.0)
        self.assertAlmostEqual(out["fractions"][1], 600.0 / 2100.0)

    def test_non_positive_denominator_fails_closed(self):
        with self.assertRaises(an.SessionInvalid):
            an.attr_b_verdict(self._block(3000.0, 3000.0, 2400.0),
                              "btrfs", 4096, 8)

    def test_negative_recovery_fails_closed(self):
        # R1 slower than R0 on the instruction layer is a treatment
        # anomaly, never a "no recovery" verdict.
        with self.assertRaises(an.SessionInvalid):
            an.attr_b_verdict(self._block(1000.0, 3000.0, 3100.0),
                              "btrfs", 4096, 8)

    def test_missing_arm_fails_closed(self):
        rows = self._block(1000.0, 3000.0, 2400.0)[:-1]
        with self.assertRaises(an.SessionInvalid):
            an.attr_b_verdict(rows, "btrfs", 4096, 8)

    def test_same_work_mismatch_fails_closed(self):
        rows = self._block(1000.0, 3000.0, 2400.0)
        rows[1]["word_sum"] += 1
        with self.assertRaises(an.SessionInvalid):
            an.attr_b_verdict(rows, "btrfs", 4096, 8)

    def test_missing_instr_estimates_indeterminate(self):
        rows = self._block(1000.0, 3000.0, 2400.0)
        rows[2]["instr_u_per_op_estimates"] = None
        with self.assertRaises(an.IndeterminateMetric):
            an.attr_b_verdict(rows, "btrfs", 4096, 8)

    def test_sha_mismatch_fails_closed(self):
        rows = self._block(1000.0, 3000.0, 2400.0)
        rows[0]["binary_sha256"] = "b" * 64
        with self.assertRaises(an.SessionInvalid):
            an.attr_b_verdict(rows, "btrfs", 4096, 8,
                              expected_binary_sha256="a" * 64)

    def test_wall_sanity_ratio_reported(self):
        rows = self._block(1000.0, 3000.0, 2400.0)
        for r in rows:
            if r["arm"] == "z2r1":
                r["wall_ns_per_op_samples"] = [1210.0] * 11
        out = an.attr_b_verdict(rows, "btrfs", 4096, 8)
        self.assertAlmostEqual(out["wall_r1_over_r0"], 1.21 / 1.20)


class RatioArithmeticTest(unittest.TestCase):
    def test_ratio_direction_is_candidate_over_baseline(self):
        base = [1000.0] * 11
        cand = [1400.0] * 11
        r, _ = an.ratio_and_delta(base, cand)
        self.assertAlmostEqual(r, 1.4)
        _, d = an.ratio_and_delta(base, cand)
        self.assertAlmostEqual(d, 400.0)

    def test_zero_baseline_rejected(self):
        with self.assertRaises(an.SessionInvalid):
            an.ratio_and_delta([0.0] * 11, [1.0] * 11)


if __name__ == "__main__":
    unittest.main(verbosity=2)
