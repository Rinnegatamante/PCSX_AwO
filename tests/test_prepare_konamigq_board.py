#!/usr/bin/env python3
"""Local System GQ media packaging and fail-closed HDD tests."""
from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import konamigq_boards as boards
import prepare_konamigq_board as prep


class KonamiGQPackageTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.registry = prep.load_registry()
        cls.profile = cls.registry["profiles"][0]

    def fixture_regions(self) -> dict[str, bytes]:
        return {row["tag"]: bytes([idx + 1]) * row["size"]
                for idx, row in enumerate(self.profile["rom_regions"])}

    def fixture_disk(self, content: bytes = b"synthetic CHD image bytes") -> tuple[bytes, dict]:
        source_disk = self.profile["disk_media"][0]
        report = {**source_disk, "package_file": f"{source_disk['name']}.chd",
                  "readonly": False,
                  "validation": {"file_version": 5, "header_sha1": source_disk["sha1"],
                                 "cylinders": 8, "heads": 4, "sectors_per_track": 32,
                                 "bytes_per_sector": 512, "logical_size": 524288},
                  "local_sha256": hashlib.sha256(content).hexdigest()}
        return content, report

    def test_package_uses_source_eeprom_defaults_not_persisted_state(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp) / "package"
            out.mkdir()
            (out / "keep.txt").write_text("preserve me", encoding="utf-8")
            data, report = self.fixture_disk()
            manifest = prep.package_regions("cryptklr", self.profile, self.fixture_regions(),
                                            out, {"rom_set": "cryptklr", "parent_chain": ["cryptklr"]},
                                            data, report)
            self.assertEqual("preserve me", (out / "keep.txt").read_text())
            self.assertEqual(128, (out / "default_eeprom.bin").stat().st_size)
            self.assertEqual(prep._default_eeprom(self.profile), (out / "default_eeprom.bin").read_bytes())
            self.assertEqual(data, (out / "420uaa04.chd").read_bytes())
            self.assertIn("eeprom_default_only=true", (out / "board.ini").read_text())
            self.assertFalse(manifest["game_execution_verified"])
            self.assertEqual(data, (out / manifest["disk_media"][0]["package_file"]).read_bytes())
            self.assertEqual(hashlib.sha256((out / "default_eeprom.bin").read_bytes()).hexdigest(),
                             manifest["eeprom_defaults"]["sha256"])
            self.assertEqual(hashlib.sha256((out / "default_eeprom_LICENSE.txt").read_bytes()).hexdigest(),
                             manifest["eeprom_defaults"]["license_sha256"])
            saved_nvram = bytes(range(128))
            (out / "nvram.bin").write_bytes(saved_nvram)
            prep.package_regions("cryptklr", self.profile, self.fixture_regions(), out,
                                 {"rom_set": "cryptklr", "parent_chain": ["cryptklr"]}, data, report)
            self.assertEqual(saved_nvram, (out / "nvram.bin").read_bytes())

    def test_invalid_geometry_report_or_region_sizes_leave_output_untouched(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp) / "package"
            out.mkdir()
            (out / "keep.txt").write_text("stable", encoding="utf-8")
            data, report = self.fixture_disk()
            bad = dict(report)
            bad["validation"] = dict(report["validation"], header_sha1="0" * 40)
            with self.assertRaises(prep.KonamiGQPreparationError):
                prep.package_regions("cryptklr", self.profile, self.fixture_regions(), out, {}, data, bad)
            bad_geometry = dict(report)
            bad_geometry["validation"] = dict(report["validation"], bytes_per_sector=1024)
            with self.assertRaisesRegex(prep.KonamiGQPreparationError, "geometry"):
                prep.package_regions("cryptklr", self.profile, self.fixture_regions(), out, {}, data, bad_geometry)
            for key, value in (("readonly", True), ("region", "cdrom"),
                               ("index", 1), ("package_file", "other.chd")):
                bad_identity = dict(report)
                bad_identity[key] = value
                with self.assertRaises(prep.KonamiGQPreparationError):
                    prep.package_regions("cryptklr", self.profile, self.fixture_regions(),
                                         out, {}, data, bad_identity)
            regions = self.fixture_regions()
            regions["soundcpu"] = b"short"
            with self.assertRaises(prep.KonamiGQPreparationError):
                prep.package_regions("cryptklr", self.profile, regions, out, {}, data, report)
            self.assertEqual(["keep.txt"], sorted(p.name for p in out.iterdir()))

    def test_malformed_previous_manifests_cannot_delete_unrelated_files(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp) / "package"
            out.mkdir()
            keep = out / "keep.txt"
            keep.write_text("untouched", encoding="utf-8")
            (out / "manifest.json").write_text(json.dumps({
                "system": "Konami System GQ", "mame_driver": boards.DRIVER,
                "mame_source_commit": boards.COMMIT, "mame_set": "cryptklr",
                "regions": {"keep.txt": {}}}), encoding="utf-8")
            data, report = self.fixture_disk()
            with self.assertRaisesRegex(prep.KonamiGQPreparationError, "valid GQ package"):
                prep.package_regions("cryptklr", self.profile, self.fixture_regions(), out, {}, data, report)
            self.assertEqual("untouched", keep.read_text())
            self.assertTrue((out / "manifest.json").exists())
            (out / "manifest.json").write_text(json.dumps({
                "system": "Konami System GQ", "mame_driver": boards.DRIVER,
                "mame_source_commit": boards.COMMIT, "mame_set": "cryptklr",
                "regions": {"../outside": {}}}), encoding="utf-8")
            outside = Path(temp) / "outside"
            outside.write_text("safe", encoding="utf-8")
            with self.assertRaisesRegex(prep.KonamiGQPreparationError, "valid GQ package"):
                prep.package_regions("cryptklr", self.profile, self.fixture_regions(), out, {}, data, report)
            self.assertEqual("safe", outside.read_text())
            self.assertEqual("untouched", keep.read_text())

    def test_unsafe_disk_filename_is_rejected_before_output_mutation(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp) / "package"
            out.mkdir()
            (out / "keep.txt").write_text("stable", encoding="utf-8")
            profile = json.loads(json.dumps(self.profile))
            profile["disk_media"][0]["name"] = "../outside"
            data, report = self.fixture_disk()
            with self.assertRaises(prep.KonamiGQPreparationError):
                prep.package_regions("cryptklr", profile, self.fixture_regions(), out, {}, data, report)
            self.assertEqual(["keep.txt"], sorted(p.name for p in out.iterdir()))

    def test_explicit_archive_resolves_nested_disk_member_without_extracting_paths(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            archive = root / "media.zip"
            content = b"synthetic local CHD member"
            with zipfile.ZipFile(archive, "w") as zf:
                zf.writestr("nested/420uaa04.chd", content)
            extracted, temporary = prep._disk_file(self.profile["disk_media"][0],
                                                   "cryptklr", root / "roms", archive)
            try:
                self.assertTrue(temporary)
                self.assertEqual(content, extracted.read_bytes())
                self.assertNotEqual(root / "nested" / "420uaa04.chd", extracted)
            finally:
                if temporary:
                    extracted.unlink(missing_ok=True)

    @unittest.skipUnless(shutil.which("chdman"), "MAME chdman required to generate an HDD CHD fixture")
    def test_real_hdd_chd_geometry_hash_verify_and_corruption(self) -> None:
        chdman = shutil.which("chdman")
        assert chdman
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            raw = root / "disk.raw"
            raw.write_bytes(bytes(128 * 2 * 8 * 512))
            chd = root / "disk.chd"
            subprocess.run([chdman, "createhd", "-i", str(raw), "-o", str(chd),
                            "-chs", "128,2,8", "-ss", "512"],
                           check=True, capture_output=True, text=True)
            info = subprocess.run([chdman, "info", "-i", str(chd), "-v"],
                                  check=True, capture_output=True, text=True).stdout
            sha = next(line.split(":", 1)[1].strip() for line in info.splitlines()
                       if line.startswith("SHA1:"))
            report = prep._verify_chd(chd, {"sha1": sha}, chdman)
            self.assertEqual({"cylinders": 128, "heads": 2, "sectors_per_track": 8,
                              "bytes_per_sector": 512, "logical_size": 128 * 2 * 8 * 512},
                             {key: report[key] for key in ("cylinders", "heads", "sectors_per_track",
                                                           "bytes_per_sector", "logical_size")})
            with self.assertRaisesRegex(prep.KonamiGQPreparationError, "SHA1"):
                prep._verify_chd(chd, {"sha1": "0" * 40}, chdman)

            # Preserve chdman verification but falsify only its GDDD geometry
            # report to make sure a geometrically inconsistent CHD is rejected.
            wrapper = root / "chdman-geometry-mismatch"
            wrapper.write_text(
                "#!" + sys.executable + "\nimport subprocess,sys\n"
                f"real={chdman!r}\n"
                "r=subprocess.run([real,*sys.argv[1:]],capture_output=True,text=True)\n"
                "if sys.argv[1]=='info' and r.returncode==0:\n"
                " print(r.stdout.replace('CYLS:128,HEADS:2,SECS:8,BPS:512',"
                "'CYLS:128,HEADS:2,SECS:8,BPS:1024'))\n"
                "else:\n print(r.stdout,end=''); print(r.stderr,end='',file=sys.stderr)\n"
                "sys.exit(r.returncode)\n", encoding="utf-8")
            wrapper.chmod(0o755)
            with self.assertRaisesRegex(prep.KonamiGQPreparationError, "does not match"):
                prep._verify_chd(chd, {"sha1": sha}, str(wrapper))

            corrupted = root / "corrupt.chd"
            damaged = bytearray(chd.read_bytes())
            damaged[-1] ^= 0xff
            corrupted.write_bytes(damaged)
            with self.assertRaises(prep.KonamiGQPreparationError):
                prep._verify_chd(corrupted, {"sha1": sha}, chdman)

    def test_registry_is_pinned_and_rejects_other_profiles(self) -> None:
        self.assertEqual(boards.COMMIT, self.registry["mame_source_commit"])
        self.assertEqual("cryptklr", self.profile["set"])
        self.assertFalse(self.profile["game_execution_verified"])
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "registry.json"
            for invalid in ([], {**self.registry, "profiles": [None]},
                            {**self.registry, "profiles": [{**self.profile, "year": 1996}]},
                            {**self.registry, "profiles": [{**self.profile, "gpu_model": True}]},
                            {**self.registry, "profiles": [{**self.profile, "cpu_clock_hz": 67737600.0}]},
                            {**self.registry, "profiles": [{**self.profile, "machine": "other"}]}):
                path.write_text(json.dumps(invalid), encoding="utf-8")
                with self.assertRaises(prep.KonamiGQPreparationError):
                    prep.load_registry(path)
            invalid = json.loads(json.dumps(self.registry))
            invalid["profiles"][0]["rom_regions"][0]["tag"] = ["soundcpu"]
            path.write_text(json.dumps(invalid), encoding="utf-8")
            with self.assertRaises(prep.KonamiGQPreparationError):
                prep.load_registry(path)
            invalid_profile = json.loads(json.dumps(self.profile))
            invalid_profile["year"] = 1996
            data, report = self.fixture_disk()
            with self.assertRaisesRegex(prep.KonamiGQPreparationError, "source/year/board"):
                prep.package_regions("cryptklr", invalid_profile, self.fixture_regions(),
                                     Path(temp) / "no-output", {}, data, report)


if __name__ == "__main__":
    unittest.main(verbosity=2)
