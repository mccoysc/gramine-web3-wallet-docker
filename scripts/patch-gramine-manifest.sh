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
    if not import_added and 'from graminelibos import Manifest' in line:
        new_lines.append(line)
        new_lines.append('import ratls_inject')
        import_added = True
    
    elif 'manifest.dump(outfile)' in line:
        indent = len(line) - len(line.lstrip())
        indent_str = ' ' * indent
        
        injection_lines = [
            f'{indent_str}# RA-TLS LD_PRELOAD injection (added by patch-gramine-manifest.sh)',
            f'{indent_str}if infile and infile.name.endswith(\'.template\'):',
            f'{indent_str}    try:',
            f'{indent_str}        import io',
            f'{indent_str}        buffer = io.BytesIO()',
            f'{indent_str}        manifest.dump(buffer)',
            f'{indent_str}        manifest_text = buffer.getvalue().decode(\'utf-8\')',
            f'{indent_str}        modified_text = ratls_inject.inject_into_manifest_text(manifest_text)',
            f'{indent_str}        outfile.write(modified_text.encode(\'utf-8\'))',
            f'{indent_str}    except Exception as e:',
            f'{indent_str}        click.echo(f\'Warning: RA-TLS injection failed: {{e}}\', err=True)',
            f'{indent_str}        manifest.dump(outfile)',
            f'{indent_str}else:',
            f'{indent_str}    manifest.dump(outfile)',
        ]
        new_lines.extend(injection_lines)
    else:
        new_lines.append(line)

with open(manifest_path, 'w') as f:
    f.write('\n'.join(new_lines))

print(f"Successfully patched {manifest_path}")
EOF

echo "Gramine manifest patching completed successfully"
