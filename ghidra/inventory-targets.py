"""Inventory native detour entry points before read-only Ghidra export."""
from pathlib import Path
import re

root = Path(__file__).resolve().parent.parent
targets = {}
for source in sorted((root / 'runtime').iterdir()):
    if source.suffix not in ('.cpp', '.inc'):
        continue
    code = source.read_text(encoding='utf-8')
    for kind, value in re.findall(r'\b(hook|uiHook|trackerHook|drawingHook)\(0x([0-9a-fA-F]+)', code):
        module = 'ChemDrawBase.dll' if kind == 'hook' else 'ChemDrawUI.dll'
        targets[module, int(value, 16)] = source.name
    for module, value in re.findall(r'resolveDetour\(L"([^"]+)",0x([0-9a-fA-F]+)\)', code):
        targets[module, int(value, 16)] = source.name
output = ''.join(f'{module}\t{address:x}\t{source}\n'
                 for (module, address), source in sorted(targets.items()))
(root / 'ghidra' / 'targets.tsv').write_text(output, encoding='utf-8', newline='\n')
print(f'{len(targets)} detour targets inventoried')
