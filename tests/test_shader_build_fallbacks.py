"""runtime/test/shaders/build.py under the PerVertexKHR fallbacks (LEONIDA_BARY_FALLBACK=1, LEONIDA_F16_INTERP_FALLBACK=1).

Both fallbacks take PerVertexKHR inputs away. The translator used to refuse a computed I/J pair (ps_ij_computed)
there, and build.py left it out (before that, the refusal failed the whole batch: 0 of 147 on a Mac, where check.py
needs LEONIDA_F16_INTERP_FALLBACK=1). Since 2026-11-25 the translator rebuilds the pair's vertex values from
screen-space derivatives instead, so build.py asks for every shader under a fallback too, and a missing module is a
failed batch again.

Standard library only; the translator is replaced by a stand-in that writes the index and modules. No llvm-mc, SPIR-V
tools, GPU or game data.

    python -I tests/test_shader_build_fallbacks.py
"""
import contextlib
import importlib.util
import io
import json
import os
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock
from types import SimpleNamespace

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "runtime", "test", "shaders", "build.py")
FALLBACKS = ("LEONIDA_BARY_FALLBACK", "LEONIDA_F16_INTERP_FALLBACK")


def load_build():
    spec = importlib.util.spec_from_file_location("shaders_build_under_test", BUILD)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class BuildUnderFallbackTest(unittest.TestCase):
    def setUp(self):
        self.fresh()

    def fresh(self):
        """A new build.py, output folder and record of what the translator was asked for (one per subtest)."""
        self.build = load_build()
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.out = Path(tmp.name) / "out"
        self.asked = []

    def fake_translate(self, work, names):
        """What rdna2spirv.py leaves in <work>/spirv: a module and an index entry per shader it validated."""
        self.asked.append(list(names))
        spirv = Path(work) / "spirv"
        spirv.mkdir(parents=True)
        index = {}
        for n in names:
            (spirv / (n + ".spv")).write_bytes(b"\x03\x02\x23\x07")
            (spirv / (n + ".spvasm")).write_text("; SPIR-V\n", encoding="utf-8")
            index[n] = {"stage": n[:2]}
        (spirv / "index.json").write_text(json.dumps(index), encoding="utf-8")
        return 0

    def run_build(self, names, env):
        stubs = {"spirv_stubs": types.SimpleNamespace(generate_for_index=lambda *a: None),
                 "bary_fallback_gs": types.SimpleNamespace(build_for_index=lambda *a: None)}
        clean = {k: v for k, v in os.environ.items() if k not in FALLBACKS}
        stdout = io.StringIO()
        with mock.patch.dict(os.environ, {**clean, **env}, clear=True), \
                mock.patch.dict(sys.modules, stubs), \
                mock.patch.object(self.build, "translate_tests", self.fake_translate), \
                mock.patch.object(sys, "argv", ["build.py", str(self.out), *names]), \
                contextlib.redirect_stdout(stdout):
            try:
                status = self.build.main()
            except SystemExit as e:
                status = e.code
        return status, stdout.getvalue()

    def test_each_fallback_builds_ps_ij_computed_too(self):
        for var in FALLBACKS:
            with self.subTest(fallback=var):
                self.fresh()
                status, printed = self.run_build(["cs_store", "ps_ij_computed"], {var: "1"})
                self.assertEqual(status, 0, printed)
                self.assertEqual(self.asked, [["cs_store", "ps_ij_computed"]])
                self.assertTrue((self.out / "spirv" / "cs_store.spv").is_file())
                self.assertTrue((self.out / "spirv" / "ps_ij_computed.spv").is_file())
                self.assertNotIn("not built", printed)

    def test_missing_mullit_fixture_is_not_accepted_as_a_build_target(self):
        status, printed = self.run_build(["ps_mullit"], {})
        self.assertEqual(status, "Unknown test shaders: ps_mullit", printed)
        self.assertEqual(self.asked, [])

    def test_the_whole_list_builds_under_a_fallback(self):
        # Before: "Translation did not produce validated SPIR-V for: ps_ij_computed", and an empty <out>/spirv.
        status, printed = self.run_build([], {"LEONIDA_F16_INTERP_FALLBACK": "1"})
        self.assertEqual(status, 0, printed)
        self.assertIn("ps_ij_computed", self.asked[0])
        published = {p.stem for p in (self.out / "spirv").glob("*.spv")}
        self.assertEqual(published, set(self.asked[0]))
        self.assertIn("ps_interp_f16_high", published)   # the f16 halves still translate under this fallback

    def test_without_a_fallback_ps_ij_computed_is_still_built(self):
        status, printed = self.run_build(["cs_store", "ps_ij_computed"], {"LEONIDA_BARY_FALLBACK": "0"})
        self.assertEqual(status, 0, printed)
        self.assertEqual(self.asked, [["cs_store", "ps_ij_computed"]])
        self.assertTrue((self.out / "spirv" / "ps_ij_computed.spv").is_file())

    def test_standalone_barycentric_override_and_probe(self):
        for value, probe, want in (("1", False, True), ("0", True, False),
                                   ("", True, True), ("", False, False)):
            with self.subTest(value=value, probe=probe), \
                    mock.patch.dict(os.environ, {"LEONIDA_BARY_FALLBACK": value}, clear=False):
                proc = SimpleNamespace(stdout=json.dumps({"barycentric_fallback": probe}) + "\n")
                with mock.patch.object(self.build.subprocess, "run", return_value=proc):
                    self.assertIs(self.build.barycentric_fallback(), want)

    def test_a_missing_module_without_a_fallback_still_fails_the_batch(self):
        keep = self.fake_translate

        def refuse(work, names):
            keep(work, names)
            os.remove(os.path.join(work, "spirv", "cs_store.spv"))
            return 0
        self.fake_translate = refuse
        try:
            status, _ = self.run_build(["cs_store"], {})
        finally:
            self.fake_translate = keep
        self.assertIn("did not produce validated SPIR-V for: cs_store", str(status))


if __name__ == "__main__":
    unittest.main()
