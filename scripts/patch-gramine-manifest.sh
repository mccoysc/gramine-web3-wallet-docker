#!/bin/bash

set -e

GRAMINE_MANIFEST="/usr/local/bin/gramine-manifest"
RATLS_INJECT_MODULE="/usr/local/lib/python3.10/dist-packages/ratls_inject.py"

if [ ! -f "$GRAMINE_MANIFEST" ]; then
    echo "Error: gramine-manifest not found at $GRAMINE_MANIFEST" >&2
    exit 1
fi

if [ ! -f "$RATLS_INJECT_MODULE" ]; then
    echo "Error: ratls_inject.py not found at $RATLS_INJECT_MODULE" >&2
    exit 1
fi

cp "$GRAMINE_MANIFEST" "${GRAMINE_MANIFEST}.orig"

python3 << 'EOF'
import sys

manifest_path = "/usr/local/bin/gramine-manifest"

with open(manifest_path, 'r') as f:
    content = f.read()

lines = content.split('\n')
new_lines = []
import_added = False

for i, line in enumerate(lines):
    new_lines.append(line)
    
    if not import_added and 'from graminelibos import Manifest' in line:
        new_lines.append('import ratls_inject')
        import_added = True
    
    elif 'manifest.dump(outfile)' in line:
        indent = len(line) - len(line.lstrip())
        indent_str = ' ' * indent
        
        injection_lines = [
            '',
            f'{indent_str}# RA-TLS LD_PRELOAD injection (added by patch-gramine-manifest.sh)',
            f'{indent_str}# Close the file first so injection can read/write it',
            f'{indent_str}outfile_name = outfile.name',
            f'{indent_str}outfile_is_stdout = (outfile_name == \'<stdout>\' or outfile_name == \'-\')',
            f'{indent_str}if hasattr(outfile, \'close\'):',
            f'{indent_str}    outfile.close()',
            f'{indent_str}',
            f'{indent_str}# Inject LD_PRELOAD if processing a template file',
            f'{indent_str}if infile and infile.name.endswith(\'.template\') and not outfile_is_stdout:',
            f'{indent_str}    try:',
            f'{indent_str}        ratls_inject.inject_ld_preload(outfile_name)',
            f'{indent_str}    except Exception as e:',
            f'{indent_str}        click.echo(f\'Warning: RA-TLS injection failed: {{e}}\', err=True)',
        ]
        new_lines.extend(injection_lines)

with open(manifest_path, 'w') as f:
    f.write('\n'.join(new_lines))

print(f"Successfully patched {manifest_path}")
EOF

echo "Gramine manifest patching completed successfully"
