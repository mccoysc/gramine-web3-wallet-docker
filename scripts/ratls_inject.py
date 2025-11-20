#!/usr/bin/env python3
"""
RA-TLS LD_PRELOAD Injection Module

This module automatically injects libratls-quote-verify.so into the LD_PRELOAD
environment variable of Gramine manifest files. This enables transparent RA-TLS
quote verification for applications running in Gramine without requiring manual
manifest modifications.
"""

import os
import re
import sys
import subprocess


def find_ratls_library():
    """
    Find the libratls-quote-verify.so library path.
    
    Returns:
        str: Path to the library, or None if not found
    """
    if 'RATLS_PRELOAD_PATH' in os.environ:
        path = os.environ['RATLS_PRELOAD_PATH']
        if os.path.isfile(path):
            return path
    
    search_paths = [
        '/usr/local/lib/libratls-quote-verify.so',
        '/usr/local/lib/x86_64-linux-gnu/libratls-quote-verify.so',
        '/usr/lib/x86_64-linux-gnu/libratls-quote-verify.so',
    ]
    
    for path in search_paths:
        if os.path.isfile(path):
            return path
    
    try:
        result = subprocess.run(
            ['ldconfig', '-p'],
            capture_output=True,
            text=True,
            check=False
        )
        for line in result.stdout.splitlines():
            if 'libratls-quote-verify.so' in line:
                match = re.search(r'=>\s+(.+)$', line)
                if match:
                    path = match.group(1).strip()
                    if os.path.isfile(path):
                        return path
    except Exception:
        pass
    
    return None


def inject_ld_preload(manifest_path):
    """
    Inject LD_PRELOAD with libratls-quote-verify.so into a Gramine manifest file.
    
    Args:
        manifest_path: Path to the manifest file to modify
        
    Returns:
        bool: True if injection was performed or skipped (success), False on error
    """
    disable_flags = ['1', 'true', 'yes']
    if os.environ.get('DISABLE_RATLS_PRELOAD', '').lower() in disable_flags:
        return True
    
    lib_path = find_ratls_library()
    if not lib_path:
        print('Warning: libratls-quote-verify.so not found, skipping LD_PRELOAD injection',
              file=sys.stderr)
        return True
    
    if not os.path.isfile(manifest_path):
        return True
    
    try:
        with open(manifest_path, 'r', encoding='utf-8') as f:
            content = f.read()
        
        if lib_path in content:
            return True
        
        lines = content.splitlines(keepends=True)
        modified = False
        
        loader_env_ld_preload_pattern = re.compile(
            r'^(\s*loader\.env\.LD_PRELOAD\s*=\s*")'
        )
        libos_env_ld_preload_pattern = re.compile(
            r'^(\s*libos\.env\.LD_PRELOAD\s*=\s*")'
        )
        
        for i, line in enumerate(lines):
            if loader_env_ld_preload_pattern.match(line):
                lines[i] = loader_env_ld_preload_pattern.sub(
                    rf'\1{lib_path}:', line
                )
                modified = True
                break
            elif libos_env_ld_preload_pattern.match(line):
                lines[i] = libos_env_ld_preload_pattern.sub(
                    rf'\1{lib_path}:', line
                )
                modified = True
                break
        
        if not modified:
            loader_env_pattern = re.compile(r'^\s*loader\.env\.')
            last_loader_env_idx = None
            for i, line in enumerate(lines):
                if loader_env_pattern.match(line):
                    last_loader_env_idx = i
            
            new_line = f'loader.env.LD_PRELOAD = "{lib_path}"\n'
            
            if last_loader_env_idx is not None:
                lines.insert(last_loader_env_idx + 1, new_line)
                modified = True
            else:
                loader_entrypoint_pattern = re.compile(
                    r'^\s*loader\.entrypoint\s*='
                )
                for i, line in enumerate(lines):
                    if loader_entrypoint_pattern.match(line):
                        lines.insert(i + 1, new_line)
                        modified = True
                        break
                
                if not modified:
                    lines.append(new_line)
                    modified = True
        
        if modified:
            with open(manifest_path, 'w', encoding='utf-8') as f:
                f.writelines(lines)
        
        return True
        
    except Exception as e:
        print(f'Warning: Failed to inject LD_PRELOAD into {manifest_path}: {e}',
              file=sys.stderr)
        return True


if __name__ == '__main__':
    if len(sys.argv) != 2:
        print(f'Usage: {sys.argv[0]} <manifest_path>', file=sys.stderr)
        sys.exit(1)
    
    success = inject_ld_preload(sys.argv[1])
    sys.exit(0 if success else 1)
