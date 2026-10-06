import unittest

from ledger.money import format_amount, parse_amount


class Money(unittest.TestCase):
    def test_parse(self):
        self.assertEqual(parse_amount("1,234.50"), 123450)
        self.assertEqual(parse_amount("7"), 700)
        self.assertEqual(parse_amount("0.5"), 50)
        self.assertEqual(parse_amount("-12.00"), -1200)
        self.assertEqual(parse_amount("(12.34)"), -1234)
        self.assertEqual(parse_amount(" -0.07 "), -7)

    def test_parse_rejects(self):
        for bad in ("", "abc", "1.234", "1,23.00"):
            with self.assertRaises(ValueError, msg=bad):
                parse_amount(bad)

    def test_format(self):
        self.assertEqual(format_amount(123450), "1,234.50")
        self.assertEqual(format_amount(5), "0.05")
        self.assertEqual(format_amount(-1200), "-12.00")
        self.assertEqual(format_amount(-7), "-0.07")
        self.assertEqual(format_amount(100000000), "1,000,000.00")


if __name__ == "__main__":
    unittest.main()
