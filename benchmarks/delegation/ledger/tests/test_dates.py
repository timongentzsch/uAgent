import datetime
import unittest

from ledger.dates import month_end, parse_date


class Dates(unittest.TestCase):
    def test_parse(self):
        day = datetime.date(2026, 3, 9)
        self.assertEqual(parse_date("2026-03-09"), day)
        self.assertEqual(parse_date("09.03.2026"), day)
        self.assertEqual(parse_date("3/9/2026"), day)
        with self.assertRaises(ValueError):
            parse_date("31.02.2026")
        with self.assertRaises(ValueError):
            parse_date("yesterday")

    def test_month_end(self):
        self.assertEqual(month_end(datetime.date(2026, 1, 15)), datetime.date(2026, 1, 31))
        self.assertEqual(month_end(datetime.date(2024, 2, 1)), datetime.date(2024, 2, 29))
        self.assertEqual(month_end(datetime.date(2026, 12, 31)), datetime.date(2026, 12, 31))


if __name__ == "__main__":
    unittest.main()
