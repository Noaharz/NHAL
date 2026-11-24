"""Tests für tools/nhalgen: Katalog prüfen, Auswahl auflösen, Ausgabe erzeugen."""

import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "nhalgen"))
import nhalgen  # noqa: E402

PLATFORMS = """
[desk]
name = "Desktop"

[tiny]
name = "Tiny"
freestanding = true

[box]
name = "Box"
external = true
"""


class TempCatalog:
    def __init__(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)
        self.write("catalog/platforms.toml", PLATFORMS)

    def write(self, rel, text):
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(textwrap.dedent(text), encoding="utf-8")

    def feature(self, fid, requires=(), desk="native", tiny="reject", status="stable"):
        parts = fid.split(".")
        src = f"backends/desk/{'_'.join(parts)}.c"
        reqs = ", ".join(f'"{r}"' for r in requires)
        body = f'id = "{fid}"\nversion = 1\nstatus = "{status}"\nsummary = "Test"\nrequires = [{reqs}]\n'
        for pid, policy in (("desk", desk), ("tiny", tiny), ("box", "external")):
            body += f'\n[platforms.{pid}]\npolicy = "{policy}"\n'
            if policy in ("native", "emulated") and status != "draft":
                body += f'sources = ["{src}"]\n'
                self.write(src, "/* leer */\n")
        self.write("catalog/" + "/".join(parts) + ".toml", body)
        if status != "draft":
            self.write("include/nhal/features/" + "/".join(parts) + ".h", "/* leer */\n")

    def load(self):
        return nhalgen.load_catalog(self.root)

    def close(self):
        self.dir.cleanup()


class NhalgenTest(unittest.TestCase):
    def setUp(self):
        self.cat = TempCatalog()

    def tearDown(self):
        self.cat.close()

    def test_hash_is_stable(self):
        self.assertEqual(nhalgen.fnv1a32("time.monotonic"), nhalgen.fnv1a32("time.monotonic"))
        self.assertEqual(nhalgen.fnv1a32(""), 0x811C9DC5)

    def test_valid_catalog_loads(self):
        self.cat.feature("a.base")
        self.cat.feature("a.top", requires=["a.base"])
        cat = self.cat.load()
        self.assertEqual(sorted(cat.features), ["a.base", "a.top"])

    def test_unknown_dependency(self):
        self.cat.feature("a.top", requires=["a.missing"])
        with self.assertRaises(nhalgen.CatalogError) as ctx:
            self.cat.load()
        self.assertTrue(any("a.missing" in e for e in ctx.exception.errors))

    def test_cycle_detected(self):
        self.cat.feature("a.one", requires=["a.two"])
        self.cat.feature("a.two", requires=["a.one"])
        with self.assertRaises(nhalgen.CatalogError) as ctx:
            self.cat.load()
        self.assertTrue(any("Zyklische" in e for e in ctx.exception.errors))

    def test_missing_source_detected(self):
        self.cat.feature("a.base")
        (self.cat.root / "backends/desk/a_base.c").unlink()
        with self.assertRaises(nhalgen.CatalogError):
            self.cat.load()

    def test_wrong_file_location(self):
        self.cat.write("catalog/a/wrong.toml", 'id = "a.right"\nversion = 1\nstatus = "draft"\nsummary = "x"\n')
        with self.assertRaises(nhalgen.CatalogError):
            self.cat.load()

    def test_dependencies_come_first(self):
        self.cat.feature("a.base")
        self.cat.feature("b.mid", requires=["a.base"])
        self.cat.feature("c.top", requires=["b.mid"])
        sel = nhalgen.resolve(self.cat.load(), "desk", ["c.top"], [])
        self.assertEqual([f.id for f in sel.order], ["a.base", "b.mid", "c.top"])
        self.assertEqual(sel.need["a.base"], "required")

    def test_required_rejected_is_build_error(self):
        self.cat.feature("a.base")
        with self.assertRaises(nhalgen.CatalogError) as ctx:
            nhalgen.resolve(self.cat.load(), "tiny", ["a.base"], [])
        self.assertTrue(any("a.base" in e for e in ctx.exception.errors))

    def test_optional_rejected_is_dropped_with_dependents(self):
        self.cat.feature("a.base", tiny="reject")
        self.cat.feature("b.top", requires=["a.base"], tiny="native")
        self.cat.feature("c.solo", tiny="native")
        sel = nhalgen.resolve(self.cat.load(), "tiny", [], ["b.top", "c.solo"])
        self.assertEqual([f.id for f in sel.order], ["c.solo"])
        self.assertEqual(len(sel.notes), 2)

    def test_draft_cannot_be_required(self):
        self.cat.feature("a.idea", status="draft")
        with self.assertRaises(nhalgen.CatalogError):
            nhalgen.resolve(self.cat.load(), "desk", ["a.idea"], [])

    def test_generated_files(self):
        self.cat.feature("a.base")
        self.cat.feature("b.top", requires=["a.base"])
        cat = self.cat.load()
        sel = nhalgen.resolve(cat, "desk", ["b.top"], [])
        out = self.cat.root / "out"
        nhalgen.generate(cat, sel, out)
        header = (out / "include/nhal/nhal_features.h").read_text(encoding="utf-8")
        registry = (out / "src/nhal_registry.c").read_text(encoding="utf-8")
        sources = (out / "nhal_sources.cmake").read_text(encoding="utf-8")
        self.assertIn("#define NHAL_HAS_B_TOP 1", header)
        self.assertIn("#define NHAL_HAS_A_BASE 1", header)
        self.assertIn("deps_b_top[] = { 0u }", registry)
        self.assertIn("NHAL_REQUIRED", registry)
        self.assertIn("backends/desk/a_base.c", sources)

    def test_empty_selection_still_compiles(self):
        self.cat.feature("a.base")
        cat = self.cat.load()
        sel = nhalgen.resolve(cat, "tiny", [], ["a.base"])
        self.assertEqual(sel.order, [])
        registry = nhalgen.render_registry_c(cat, sel)
        self.assertIn("nhal_registry_count = 0u", registry)
        self.assertIn("Platzhalter", registry)

    def test_expand_targets(self):
        cat = self.cat.load()
        self.assertEqual(nhalgen.expand_targets(cat, ["all"]), ["desk", "tiny", "box"])
        self.assertEqual(nhalgen.expand_targets(cat, ["tiny", "desk", "tiny"]), ["tiny", "desk"])
        with self.assertRaises(nhalgen.CatalogError):
            nhalgen.expand_targets(cat, ["mars"])

    def test_analyse_reports_per_platform(self):
        self.cat.feature("a.base", tiny="reject")
        cat = self.cat.load()
        desk = nhalgen.analyse(cat, "desk", ["a.base"], [])
        tiny = nhalgen.analyse(cat, "tiny", ["a.base"], [])
        self.assertEqual(desk.states["a.base"][0], nhalgen.STATE_NATIVE)
        self.assertEqual(desk.errors, [])
        self.assertEqual(tiny.states["a.base"][0], nhalgen.STATE_MISSING)
        self.assertEqual(len(tiny.errors), 1)

    def test_external_platform_is_checked_not_built(self):
        self.cat.feature("a.base")
        cat = self.cat.load()
        box = nhalgen.analyse(cat, "box", ["a.base"], [])
        self.assertEqual(box.errors, [])
        self.assertEqual(box.states["a.base"][0], nhalgen.STATE_EXTERNAL)
        self.assertEqual(box.order, [])
        with self.assertRaises(nhalgen.CatalogError):
            nhalgen.resolve(cat, "box", ["a.base"], [])

    def test_manifest_with_targets(self):
        self.cat.write("app.toml", """
            [platforms]
            targets = ["desk", "tiny"]
            [features]
            required = ["a.base"]
        """)
        m = nhalgen.load_manifest(self.cat.root / "app.toml")
        self.assertEqual(m.targets, ["desk", "tiny"])
        self.assertEqual(m.required, ["a.base"])

    def test_split_list(self):
        self.assertEqual(nhalgen.split_list(["a,b", " c ", ""]), ["a", "b", "c"])


if __name__ == "__main__":
    unittest.main()
