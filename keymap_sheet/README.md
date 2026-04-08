# Keymap Sheet Workflow

`lalapadgen2_keymap.csv` is a spreadsheet-friendly export of `config/lalapadgen2.keymap`.

## Open in Google Sheets

1. Upload `lalapadgen2_keymap.csv` to Google Drive.
2. Open it with Google Sheets.
3. Edit only the `k00` to `k11` cells.

Blank cells mean "there is no key at this physical position".
Do not move rows, remove columns, or rename the header row.

## Write Back to the Keymap

After editing in Google Sheets, download the sheet as CSV and overwrite `keymap_sheet/lalapadgen2_keymap.csv`.

Then run:

```powershell
python tools/keymap_sheet.py import --csv keymap_sheet/lalapadgen2_keymap.csv --keymap config/lalapadgen2.keymap
```

## Refresh the CSV from the Current Keymap

If you edit `config/lalapadgen2.keymap` directly and want to sync the sheet again, run:

```powershell
python tools/keymap_sheet.py export --keymap config/lalapadgen2.keymap --csv keymap_sheet/lalapadgen2_keymap.csv
```
