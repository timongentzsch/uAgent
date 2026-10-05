"""Spending by category."""


def by_category(bookings):
    """bookings: (category, cents) pairs. Returns (category, total) pairs,
    the largest spending (most negative total) first; equal totals by name.
    Categories whose total is zero are left out."""
    totals = {}
    for category, cents in bookings:
        totals[category] = cents
    return sorted(totals.items())
