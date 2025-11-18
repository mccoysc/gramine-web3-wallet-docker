#!/usr/bin/env python3
"""
Wrapper for gramine-manifest that automatically adds libratls-quote-verify.so to LD_PRELOAD.

This wrapper:
1. Calls the original gramine-manifest command
2. Post-processes the generated manifest to inject libratls-quote-verify.so into loader.env.LD_PRELOAD
3. Ensures the library is loaded first (prepended to LD_PRELOAD)
4. Is idempotent (won't add duplicates)
5. Can be disabled via DISABLE_RATLS_PRELOAD=1 environment variable

The library path can be customized via RATLS_PRELOAD_SO environment variable.
"""

import os
import sys
import subprocess
import re
from pathlib import Path


def find_ratls_library():
    """Find the libratls-quote-verify.so library path."""
    env_path = os.environ.get('RATLS_PRELOAD_SO')
    if env_path and os.path.exists(env_path):
        return env_path
    
    search_paths = [
        '/usr/local/lib/libratls-quote-verify.so',
        '/usr/lib/libratls-quote-verify.so',
        '/usr/lib/x86_64-linux-gnu/libratls-quote-verify.so',
        '/usr/local/lib/x86_64-linux-gnu/libratls-quote-verify.so',
    ]
    
    for path in search_paths:
        if os.path.exists(path):
            return path
    
    try:
        result = subprocess.run(
            ['find', '/usr/local/lib', '/usr/lib', '-name', 'libratls-quote-verify.so', '-type', 'f'],
            capture_output=True,
            text=True,
            timeout=5
        )
        if result.returncode == 0 and result.stdout.strip():
            return result.stdout.strip().split('\n')[0]
    except (subprocess.TimeoutExpired, FileNotFoundError):
        pass
    
    return None


def extract_output_file(args):
    """Extract the output manifest file path from gramine-manifest arguments."""
    i = 0
    while i < len(args):
        arg = args[i]
        if arg.startswith('-D'):
            i += 1
            continue
        if arg.startswith('-'):
            i += 1
            if i < len(args) and not args[i].startswith('-'):
                i += 1
            continue
        if i == len(args) - 1:
            return arg
        i += 1
    
    return None


def process_manifest(manifest_path, ratls_lib):
    """Post-process the manifest to add libratls-quote-verify.so to LD_PRELOAD."""
    if not os.path.exists(manifest_path):
        return
    
    with open(manifest_path, 'r') as f:
        content = f.read()
    
    if ratls_lib in content:
        return
    
    lines = content.split('\n')
    new_lines = []
    found_ld_preload = False
    in_loader_env_block = False
    
    for i, line in enumerate(lines):
        stripped = line.strip()
        
        if re.match(r'^loader\.env\.LD_PRELOAD\s*=', stripped):
            found_ld_preload = True
            match = re.match(r'^(\s*loader\.env\.LD_PRELOAD\s*=\s*)"([^"]*)"(.*)$', line)
            if match:
                indent, existing_value, rest = match.groups()
                if ratls_lib not in existing_value:
                    if existing_value:
                        new_value = f'{ratls_lib}:{existing_value}'
                    else:
                        new_value = ratls_lib
                    new_lines.append(f'{indent}"{new_value}"{rest}')
                else:
                    new_lines.append(line)
            else:
                new_lines.append(line)
        
        elif re.match(r'^loader\.env\s*=\s*\[', stripped):
            in_loader_env_block = True
            new_lines.append(line)
            
            block_lines = [line]
            j = i + 1
            while j < len(lines):
                block_lines.append(lines[j])
                if ']' in lines[j]:
                    break
                j += 1
            
            block_content = '\n'.join(block_lines)
            
            ld_preload_match = re.search(r'"LD_PRELOAD=([^"]*)"', block_content)
            if ld_preload_match:
                found_ld_preload = True
                existing_value = ld_preload_match.group(1)
                if ratls_lib not in existing_value:
                    if existing_value:
                        new_value = f'{ratls_lib}:{existing_value}'
                    else:
                        new_value = ratls_lib
                    
                    for k in range(i + 1, j + 1):
                        line_to_check = lines[k]
                        if 'LD_PRELOAD=' in line_to_check:
                            updated_line = re.sub(
                                r'"LD_PRELOAD=([^"]*)"',
                                f'"LD_PRELOAD={new_value}"',
                                line_to_check
                            )
                            new_lines.append(updated_line)
                        else:
                            new_lines.append(line_to_check)
                else:
                    for k in range(i + 1, j + 1):
                        new_lines.append(lines[k])
            else:
                for k in range(i + 1, j):
                    new_lines.append(lines[k])
                
                indent_match = re.match(r'^(\s*)', lines[i + 1] if i + 1 < len(lines) else '')
                indent = indent_match.group(1) if indent_match else '  '
                new_lines.append(f'{indent}"LD_PRELOAD={ratls_lib}",')
                new_lines.append(lines[j])
            
            for k in range(i + 1, j + 1):
                lines[k] = None
        
        elif line is not None:
            new_lines.append(line)
    
    if not found_ld_preload:
        insert_index = -1
        for i, line in enumerate(new_lines):
            if re.match(r'^loader\.env\.', line.strip()):
                insert_index = i + 1
        
        if insert_index == -1:
            for i, line in enumerate(new_lines):
                if re.match(r'^libos\.entrypoint\s*=', line.strip()):
                    insert_index = i + 1
                    break
        
        if insert_index == -1:
            for i, line in enumerate(new_lines):
                if re.match(r'^loader\.entrypoint\s*=', line.strip()):
                    insert_index = i + 1
                    break
        
        if insert_index > 0:
            new_lines.insert(insert_index, f'loader.env.LD_PRELOAD = "{ratls_lib}"')
        else:
            new_lines.insert(0, f'loader.env.LD_PRELOAD = "{ratls_lib}"')
    
    with open(manifest_path, 'w') as f:
        f.write('\n'.join(new_lines))


def main():
    if os.environ.get('DISABLE_RATLS_PRELOAD') == '1':
        real_cmd = ['/usr/local/bin/gramine-manifest.real'] + sys.argv[1:]
        os.execv('/usr/local/bin/gramine-manifest.real', real_cmd)
        return
    
    ratls_lib = find_ratls_library()
    if not ratls_lib:
        print('Warning: libratls-quote-verify.so not found, skipping LD_PRELOAD injection', file=sys.stderr)
        real_cmd = ['/usr/local/bin/gramine-manifest.real'] + sys.argv[1:]
        os.execv('/usr/local/bin/gramine-manifest.real', real_cmd)
        return
    
    output_file = extract_output_file(sys.argv[1:])
    
    real_cmd = ['/usr/local/bin/gramine-manifest.real'] + sys.argv[1:]
    result = subprocess.run(real_cmd)
    
    if result.returncode == 0 and output_file:
        try:
            process_manifest(output_file, ratls_lib)
        except Exception as e:
            print(f'Warning: Failed to inject LD_PRELOAD: {e}', file=sys.stderr)
    
    sys.exit(result.returncode)


if __name__ == '__main__':
    main()
