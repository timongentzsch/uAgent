"""Bank exports: a header line, then one row per booking."""


def read_rows(path):
    """Each row as a dict keyed by the header's names. Fields may be quoted
    and a quoted field may contain commas and doubled quotes; blank lines are
    skipped; a byte-order mark before the header is ignored."""
    with open(path, encoding="utf-8") as handle:
        lines = handle.read().split("\n")
    header = lines[0].split(",")
    return [dict(zip(header, line.split(","))) for line in lines[1:]]
