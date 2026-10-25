"""Check the default Android APK's native libraries without an Android device."""

import os
import struct
import unittest
import zipfile


class AndroidApkTests(unittest.TestCase):
    def test_default_apk_supports_32_bit_arm_and_existing_64_bit_abis(self):
        expected = {
            "armeabi-v7a": (1, 40),  # ELFCLASS32, EM_ARM
            "arm64-v8a": (2, 183),  # ELFCLASS64, EM_AARCH64
            "x86_64": (2, 62),      # ELFCLASS64, EM_X86_64
        }
        apk_path = os.environ.get("IGNITION_ANDROID_APK", "build-android/ignition.apk")
        with zipfile.ZipFile(apk_path) as apk:
            for abi, (elf_class, machine) in expected.items():
                with self.subTest(abi=abi):
                    name = f"lib/{abi}/libignition.so"
                    self.assertIn(name, apk.namelist())
                    with apk.open(name) as library:
                        header = library.read(20)
                    self.assertEqual(len(header), 20)
                    self.assertEqual(header[:4], b"\x7fELF")
                    self.assertEqual(header[4], elf_class)
                    self.assertEqual(header[5], 1, "the library must be little-endian")
                    self.assertEqual(struct.unpack_from("<H", header, 18)[0], machine)
