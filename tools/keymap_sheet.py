#!/usr/bin/env python3
"""Export/import the LalaPad Gen2 keymap to/from a spreadsheet-friendly CSV."""

from __future__ import annotations

import argparse
import csv
import re
from pathlib import Path

LAYER_ORDER = [
    "DEFAULT_LAYER",
    "SECONDARY_LAYER",
    "TERTIARY_LAYER",
    "SYSTEM_LAYER",
    "NAGINATA_LAYER",
]

ROW_ACTIVE_COLS = {
    0: [0, 1, 2, 3, 4, 7, 8, 9, 10, 11],
    1: [0, 1, 2, 3, 4, 7, 8, 9, 10, 11],
    2: [0, 1, 2, 3, 4, 7, 8, 9, 10, 11],
    3: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11],
    4: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10],
    5: [0, 1, 2, 9, 10, 11],
    6: [0, 1, 2, 3, 4, 7, 8, 9, 10, 11],
}

KEY_COLUMNS = [f"k{i:02d}" for i in range(12)]


class KeymapError(RuntimeError):
    """Raised when the keymap or CSV cannot be parsed safely."""


def split_bindings(line: str) -> list[str]:
    stripped = line.strip()
    if not stripped:
        return []
    return [part.strip() for part in re.split(r"\s{2,}", stripped) if part.strip()]


def find_layer_ranges(lines: list[str]) -> dict[str, tuple[int, int, str]]:
    ranges: dict[str, tuple[int, int, str]] = {}
    for layer in LAYER_ORDER:
        layer_pattern = re.compile(rf"^\s*{re.escape(layer)}\s*\{{\s*$")
        start_idx = next((i for i, line in enumerate(lines) if layer_pattern.match(line)), None)
        if start_idx is None:
            raise KeymapError(f"Layer '{layer}' was not found in the keymap.")

        bindings_idx = None
        for i in range(start_idx, len(lines)):
            if "bindings = <" in lines[i]:
                bindings_idx = i
                break
        if bindings_idx is None:
            raise KeymapError(f"Layer '{layer}' does not contain a bindings block.")

        end_idx = None
        for i in range(bindings_idx + 1, len(lines)):
            if lines[i].strip() == ">;":
                end_idx = i
                break
        if end_idx is None:
            raise KeymapError(f"Layer '{layer}' bindings block is not closed.")

        row_indent = ""
        if bindings_idx + 1 < len(lines):
            indent_match = re.match(r"^(\s*)", lines[bindings_idx + 1])
            row_indent = indent_match.group(1) if indent_match else ""
        ranges[layer] = (bindings_idx + 1, end_idx, row_indent)
    return ranges


def extract_keymap_rows(keymap_path: Path) -> dict[str, dict[int, dict[str, str]]]:
    lines = keymap_path.read_text(encoding="utf-8").splitlines()
    ranges = find_layer_ranges(lines)
    exported: dict[str, dict[int, dict[str, str]]] = {}

    for layer, (start, end, _indent) in ranges.items():
        layer_lines = lines[start:end]
        if len(layer_lines) != len(ROW_ACTIVE_COLS):
            raise KeymapError(
                f"Layer '{layer}' row count changed. Expected {len(ROW_ACTIVE_COLS)}, got {len(layer_lines)}."
            )

        exported[layer] = {}
        for row_index, line in enumerate(layer_lines):
            bindings = split_bindings(line)
            expected_count = len(ROW_ACTIVE_COLS[row_index])
            if len(bindings) != expected_count:
                raise KeymapError(
                    f"Layer '{layer}' row {row_index} expected {expected_count} bindings, got {len(bindings)}."
                )

            row_data = {column: "" for column in KEY_COLUMNS}
            for column_index, binding in zip(ROW_ACTIVE_COLS[row_index], bindings):
                row_data[f"k{column_index:02d}"] = binding
            exported[layer][row_index] = row_data

    return exported


def export_csv(keymap_path: Path, csv_path: Path) -> None:
    exported = extract_keymap_rows(keymap_path)
    csv_path.parent.mkdir(parents=True, exist_ok=True)

    with csv_path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=["layer", "row"] + KEY_COLUMNS)
        writer.writeheader()
        for layer in LAYER_ORDER:
            for row_index in range(len(ROW_ACTIVE_COLS)):
                row_data = {"layer": layer, "row": str(row_index)}
                row_data.update(exported[layer][row_index])
                writer.writerow(row_data)


def load_csv_rows(csv_path: Path) -> dict[str, dict[int, dict[str, str]]]:
    with csv_path.open("r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.DictReader(handle)
        expected_headers = ["layer", "row"] + KEY_COLUMNS
        if reader.fieldnames != expected_headers:
            raise KeymapError(
                "CSV headers do not match the expected sheet format. "
                f"Expected: {expected_headers}, got: {reader.fieldnames}"
            )

        grouped: dict[str, dict[int, dict[str, str]]] = {}
        for csv_row in reader:
            layer = (csv_row.get("layer") or "").strip()
            if layer not in LAYER_ORDER:
                raise KeymapError(f"Unknown layer '{layer}' in CSV.")

            try:
                row_index = int((csv_row.get("row") or "").strip())
            except ValueError as exc:
                raise KeymapError(f"Invalid row value '{csv_row.get('row')}' for layer '{layer}'.") from exc

            if row_index not in ROW_ACTIVE_COLS:
                raise KeymapError(f"Unknown row '{row_index}' for layer '{layer}'.")

            grouped.setdefault(layer, {})
            if row_index in grouped[layer]:
                raise KeymapError(f"Duplicate CSV entry for layer '{layer}', row {row_index}.")

            normalized = {column: (csv_row.get(column) or "").strip() for column in KEY_COLUMNS}
            for column in ROW_ACTIVE_COLS[row_index]:
                key = f"k{column:02d}"
                if not normalized[key]:
                    raise KeymapError(
                        f"Layer '{layer}' row {row_index} column {key} is empty. "
                        "Only unused positions may be left blank."
                    )

            grouped[layer][row_index] = normalized

    for layer in LAYER_ORDER:
        if layer not in grouped:
            raise KeymapError(f"Layer '{layer}' is missing from the CSV.")
        for row_index in ROW_ACTIVE_COLS:
            if row_index not in grouped[layer]:
                raise KeymapError(f"Layer '{layer}' row {row_index} is missing from the CSV.")
    return grouped


def format_row(row_data: dict[str, str], row_index: int, indent: str) -> str:
    cells = [row_data[f"k{column:02d}"] if column in ROW_ACTIVE_COLS[row_index] else "" for column in range(12)]
    return f"{indent}{'  '.join(cells).rstrip()}"


def import_csv(csv_path: Path, keymap_path: Path) -> None:
    csv_rows = load_csv_rows(csv_path)
    lines = keymap_path.read_text(encoding="utf-8").splitlines()
    ranges = find_layer_ranges(lines)

    for layer in LAYER_ORDER:
        start, end, indent = ranges[layer]
        replacement = [format_row(csv_rows[layer][row_index], row_index, indent) for row_index in range(len(ROW_ACTIVE_COLS))]
        lines[start:end] = replacement

    keymap_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    export_parser = subparsers.add_parser("export", help="Export the current keymap to CSV.")
    export_parser.add_argument("--keymap", type=Path, required=True, help="Path to lalapadgen2.keymap")
    export_parser.add_argument("--csv", type=Path, required=True, help="Output CSV path")

    import_parser = subparsers.add_parser("import", help="Import a CSV and rewrite the keymap bindings.")
    import_parser.add_argument("--csv", type=Path, required=True, help="Input CSV path")
    import_parser.add_argument("--keymap", type=Path, required=True, help="Path to lalapadgen2.keymap")

    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    if args.command == "export":
        export_csv(args.keymap, args.csv)
    elif args.command == "import":
        import_csv(args.csv, args.keymap)
    else:
        parser.error(f"Unsupported command: {args.command}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
