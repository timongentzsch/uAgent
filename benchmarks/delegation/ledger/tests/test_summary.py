import os
import tempfile
import unittest

import ledger

EXPORT = '''date,payee,category,amount
2026-03-02,Landlord,rent,"-1,200.00"
03.03.2026,"Bakery, Main St",food,-4.50
3/9/2026,Employer,pay,"3,000.00"
2026-03-31,Cinema,fun,(17.00)
2026-04-01,Bakery,food,-3.00
'''


class Summary(unittest.TestCase):
    def test_month_summary(self):
        with tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False, encoding="utf-8") as handle:
            handle.write(EXPORT)
        self.addCleanup(os.unlink, handle.name)
        # ledger.summary(path, year, month): the month's spending by category
        # as text, one line per category, then the month's balance.
        self.assertEqual(
            ledger.summary(handle.name, 2026, 3),
            "rent       -1,200.00\n"
            "fun           -17.00\n"
            "food           -4.50\n"
            "pay         3,000.00\n"
            "balance     1,778.50\n",
        )

    def test_month_without_bookings(self):
        with tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False, encoding="utf-8") as handle:
            handle.write(EXPORT)
        self.addCleanup(os.unlink, handle.name)
        self.assertEqual(ledger.summary(handle.name, 2025, 1), "balance         0.00\n")


if __name__ == "__main__":
    unittest.main()
