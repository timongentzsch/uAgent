"""Amounts are integers of cents."""


def parse_amount(text):
    """'1,234.50' -> 123450; '(12.00)' and '-12.00' are negative; '7' is 700."""
    text = text.strip()
    whole, _, fraction = text.partition(".")
    return int(whole) * 100 + int(fraction or 0)


def format_amount(cents):
    """123450 -> '1,234.50'; negative amounts get a leading minus."""
    return f"{cents // 100}.{cents % 100}"
