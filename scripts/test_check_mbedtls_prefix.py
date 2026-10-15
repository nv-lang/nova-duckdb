#!/usr/bin/env python3
"""Regression tests for parsing GNU/LLVM nm output from ELF and COFF archives."""
import unittest
import importlib.util
from pathlib import Path

checker_path = Path(__file__).with_name("check-mbedtls-prefix.py")
spec = importlib.util.spec_from_file_location("check_mbedtls_prefix", checker_path)
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)
parse_nm_symbols = checker.parse_nm_symbols


class ParseNmSymbolsTests(unittest.TestCase):
    def test_ignores_standalone_archive_member_headers(self):
        output = """mbedtls_wrapper.cpp.o:
                 U mbedtls_ct_zero
0000000000000000 T nova_ddb_mbedtls_aes_init
"""
        self.assertEqual(parse_nm_symbols(output), [
            "mbedtls_ct_zero",
            "nova_ddb_mbedtls_aes_init",
        ])

    def test_keeps_unprefixed_symbols_as_guard_inputs(self):
        output = "0000000000000000 D mbedtls_rsa_info\n                 U mbedtls_ct_zero\n"
        self.assertEqual(parse_nm_symbols(output), [
            "mbedtls_rsa_info",
            "mbedtls_ct_zero",
        ])

    def test_ignores_empty_lines_and_member_only_output(self):
        self.assertEqual(parse_nm_symbols("\nmbedtls_wrapper.cpp.o:\n"), [])

    def test_prefix_header_maps_linux_global_data_symbols(self):
        header = Path(__file__).parents[1] / "native" / "duckdb_mbedtls_prefix.h"
        text = header.read_text(encoding="utf-8")
        self.assertIn("#define mbedtls_ct_zero nova_ddb_mbedtls_ct_zero", text)
        self.assertIn("#define mbedtls_rsa_info nova_ddb_mbedtls_rsa_info", text)


if __name__ == "__main__":
    unittest.main()
