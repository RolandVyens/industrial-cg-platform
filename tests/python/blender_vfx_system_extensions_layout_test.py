import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SYSTEM_REPO = ROOT / "release" / "extensions" / "system"
EXPECTED_PACKAGES = (
    "blender_vfx_qt_runtime",
    "blender_vfx_viewlayer_manager",
)


class SystemExtensionsLayoutTest(unittest.TestCase):
    def test_expected_system_packages_exist_directly_under_repo_root(self):
        for package in EXPECTED_PACKAGES:
            manifest = SYSTEM_REPO / package / "blender_manifest.toml"
            self.assertTrue(
                manifest.is_file(),
                f"expected manifest at {manifest}",
            )

    def test_nested_system_repo_root_is_not_used(self):
        nested_repo = SYSTEM_REPO / "system"
        if not nested_repo.exists():
            return

        nested_manifests = sorted(nested_repo.glob("*/blender_manifest.toml"))
        self.assertEqual(
            [],
            nested_manifests,
            "system extensions must not be nested under release/extensions/system/system",
        )


if __name__ == "__main__":
    unittest.main()
