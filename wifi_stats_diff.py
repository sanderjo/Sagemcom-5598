#!/usr/bin/env python3
"""Compare two `sagemcom5598.py --wifi_stats` or `--wan_stats` text captures
and print the MB received/sent, per row (wifi band, or the wan interface),
between them."""

import argparse

_LABEL_COLUMNS = ("band", "interface")


def _load(path: str) -> tuple[str, dict]:
    """Parse a table printed by `sagemcom5598.py --wifi_stats` or
    `--wan_stats`. Returns (label column name, {label: row dict})."""
    with open(path) as f:
        lines = f.readlines()

    header = None
    rows = {}
    for line in lines:
        parts = line.split()
        if not parts:
            continue
        if header is None:
            if parts[0] in _LABEL_COLUMNS and "rx_Mbytes" in parts and "tx_Mbytes" in parts:
                header = parts
            continue
        if all(set(p) == {"-"} for p in parts):
            continue
        if len(parts) != len(header):
            continue
        rows[parts[0]] = dict(zip(header, parts))

    if header is None:
        raise ValueError(f"{path}: no 'wifi stats' or 'wan stats' table found")
    return header[0], rows


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Diff two 'sagemcom5598.py --wifi_stats/--wan_stats' text captures"
    )
    parser.add_argument("before", nargs="?", default="wifi_stats.before.txt")
    parser.add_argument("after", nargs="?", default="wifi_stats.after.txt")
    args = parser.parse_args()

    try:
        label, before = _load(args.before)
        _, after = _load(args.after)
    except (FileNotFoundError, ValueError) as exc:
        parser.error(str(exc))

    rows = sorted(set(before) & set(after))
    for row in sorted(set(before) ^ set(after)):
        print(f"skipping {label} {row}: missing from one of the files")

    print(f"{label:<10}{'rx MB':>10}{'tx MB':>10}  notes")
    for row in rows:
        rx_delta = float(after[row]["rx_Mbytes"]) - float(before[row]["rx_Mbytes"])
        tx_delta = float(after[row]["tx_Mbytes"]) - float(before[row]["tx_Mbytes"])
        notes = []
        if rx_delta < 0:
            notes.append("rx counter reset")
        if tx_delta < 0:
            notes.append("tx counter reset")
        print(f"{row:<10}{rx_delta:>10.2f}{tx_delta:>10.2f}  {', '.join(notes)}")


if __name__ == "__main__":
    main()
