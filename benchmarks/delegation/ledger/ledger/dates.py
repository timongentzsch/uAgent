"""Dates are datetime.date."""
import datetime


def parse_date(text):
    """Accepts 2026-03-09, 09.03.2026 and 3/9/2026 (month first)."""
    return datetime.date.fromisoformat(text)


def month_end(day):
    """The last day of `day`'s month."""
    return datetime.date(day.year, day.month + 1, 1) - datetime.timedelta(days=1)
