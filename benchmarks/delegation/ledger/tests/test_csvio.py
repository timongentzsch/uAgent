import os
import tempfile
import unittest

from ledger.csvio import read_rows


class Csv(unittest.TestCase):
    def rows(self, text):
        with tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False, encoding="utf-8") as handle:
            handle.write(text)
        self.addCleanup(os.unlink, handle.name)
        return read_rows(handle.name)

    def test_plain(self):
        self.assertEqual(
            self.rows("date,payee,amount\n2026-03-09,Bakery,-4.50\n"),
            [{"date": "2026-03-09", "payee": "Bakery", "amount": "-4.50"}],
        )

    def test_quotes_blank_lines_and_bom(self):
        text = '﻿date,payee,amount\r\n\r\n2026-03-09,"Smith, ""Jo"" & Co","1,200.00"\r\n\r\n'
        self.assertEqual(
            self.rows(text),
            [{"date": "2026-03-09", "payee": 'Smith, "Jo" & Co', "amount": "1,200.00"}],
        )

    def test_short_row_is_an_error(self):
        with self.assertRaises(ValueError):
            self.rows("date,payee,amount\n2026-03-09,Bakery\n")


if __name__ == "__main__":
    unittest.main()
