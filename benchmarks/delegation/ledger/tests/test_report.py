import unittest

from ledger.report import by_category


class Report(unittest.TestCase):
    def test_totals_and_order(self):
        bookings = [("food", -450), ("rent", -90000), ("food", -1250), ("pay", 250000),
                    ("fun", -1700), ("refund", 500), ("refund", -500)]
        self.assertEqual(
            by_category(bookings),
            [("rent", -90000), ("food", -1700), ("fun", -1700), ("pay", 250000)],
        )

    def test_empty(self):
        self.assertEqual(by_category([]), [])


if __name__ == "__main__":
    unittest.main()
