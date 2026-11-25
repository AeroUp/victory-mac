"""--vs-single-lane: a vertex shader translated as a wave whose only running lane is lane 0 (rdna2spirv vs_single_lane).

MoltenVK cannot convert a vertex shader that reads SubgroupLocalInvocationId (Metal vertex functions have no SIMD-lane
index: "Subgroup builtins are not available in this type of function", MoltenVK 1.4.2), and 1,113 of the 2,236 corpus
VS read it. With the flag the VS keeps no subgroup operation. A wave with one running lane is a state the hardware
reaches (the last wave of a draw can hold one vertex), so the check is an equivalence: the default translation run as
a 32-wide subgroup with only lane 0 running (spirv_mini_interp.run_workgroup) and the single-lane translation run on
its own (run_fragment) must write the same outputs, for each vertex.
"""
import unittest

import support
import rdna2spirv
from spirv_mini_interp import run_fragment, run_workgroup

# Lane index (v_mbcnt), a ballot into VCC and its population count, EXEC narrowed by it and restored, the first running
# lane's value (v_readfirstlane), a uniform-branch vote's input (s_and_saveexec sets SCC), all exported per vertex.
VS = """v_mbcnt_lo_u32_b32 v1, -1, 0
v_mbcnt_hi_u32_b32 v1, -1, v1
v_cvt_f32_u32_e32 v2, v0
v_cmp_gt_u32_e32 vcc, 2, v0
s_bcnt1_i32_b64 s6, vcc
v_mov_b32_e32 v3, 1.0
s_and_saveexec_b64 s[4:5], vcc
v_mov_b32_e32 v3, 0.5
s_mov_b64 exec, s[4:5]
v_readfirstlane_b32 s7, v2
v_mov_b32_e32 v4, s6
v_mov_b32_e32 v5, s7
v_cvt_f32_u32_e32 v4, v4
v_cvt_f32_u32_e32 v6, v1
v_mov_b32_e32 v7, 0
v_mov_b32_e32 v8, 1.0
exp pos0 v2, v7, v7, v8 done
exp param0 v3, v4, v5, v6
s_endpgm
"""


def translate(single_lane):
    old = rdna2spirv.VS_SINGLE_LANE
    rdna2spirv.VS_SINGLE_LANE = single_lane
    try:
        return support.translate_lines("vs_single_lane", VS.splitlines())
    finally:
        rdna2spirv.VS_SINGLE_LANE = old


class SingleLaneVertexShader(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.wave = translate(False)
        cls.single = translate(True)

    def test_default_translation_uses_the_lane_and_subgroup_operations(self):
        # Otherwise the equivalence below would not test the rewrite.
        self.assertIn("BuiltIn SubgroupLocalInvocationId", self.wave)
        for op in ("OpGroupNonUniformBallot", "OpGroupNonUniformShuffle", "OpGroupNonUniformAny",
                   "OpGroupNonUniformBitwiseOr"):
            self.assertIn(op, self.wave)

    def test_no_subgroup_operation_or_lane_index_is_left(self):
        self.assertNotIn("GroupNonUniform", self.single)
        self.assertNotIn("SubgroupLocalInvocationId", self.single)
        self.assertNotIn("%sg_lane", self.single)

    def test_same_outputs_as_a_wave_with_one_running_lane(self):
        for vid in range(4):
            with self.subTest(vertex=vid):
                inputs = {"%vertex_index": vid, "%instance_index": 0}
                _, outs = run_workgroup(self.wave, 1, 32, inputs_for=lambda i: dict(inputs, **{"%sg_lane": 0}),
                                        return_outputs=True)
                self.assertEqual(run_fragment(self.single, inputs=inputs), outs[0])

    def test_values_are_those_of_a_one_vertex_wave(self):
        """Lane 0; the ballot holds this vertex alone (count 1 if v0 < 2, else 0); the first lane is this vertex."""
        f = lambda w: __import__("struct").unpack("<f", __import__("struct").pack("<I", w))[0]   # noqa: E731
        for vid in range(4):
            with self.subTest(vertex=vid):
                out = run_fragment(self.single, inputs={"%vertex_index": vid, "%instance_index": 0})
                p = [f(w) for w in out["%out_param0"]]
                self.assertEqual(p, [0.5 if vid < 2 else 1.0, 1.0 if vid < 2 else 0.0, float(vid), 0.0])


if __name__ == "__main__":
    unittest.main()
