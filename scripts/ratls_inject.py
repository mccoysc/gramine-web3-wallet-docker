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


def is_dcap_manifest(manifest_text):
    """
    Check if a manifest uses DCAP attestation.
    
    Looks for sgx.remote_attestation = "dcap" in both dotted key and table syntax.
    
    Args:
        manifest_text: String containing the manifest in TOML format
        
    Returns:
        bool: True if DCAP attestation is configured, False otherwise
    """
    lines = manifest_text.split('\n')
    in_sgx_section = False
    
    for line in lines:
        stripped = line.strip()
        
        if stripped.startswith('#'):
            continue
        
        if stripped == '[sgx]':
            in_sgx_section = True
            continue
        
        if in_sgx_section and stripped.startswith('['):
            in_sgx_section = False
        
        if re.match(r'^\s*sgx\.remote_attestation\s*=\s*["\']dcap["\']', line, re.IGNORECASE):
            return True
        
        if in_sgx_section and re.match(r'^\s*remote_attestation\s*=\s*["\']dcap["\']', line, re.IGNORECASE):
            return True
    
    return False


def inject_into_manifest_text(manifest_text):
    """
    Inject LD_PRELOAD and trusted_files into manifest text (TOML format).
    
    This modifies the manifest text to add libratls-quote-verify.so
    to loader.env.LD_PRELOAD and sgx.trusted_files, but ONLY if the manifest
    uses DCAP attestation (sgx.remote_attestation = "dcap").
    
    Handles both TOML table syntax ([loader.env]) and dotted key syntax (loader.env.X).
    
    Args:
        manifest_text: String containing the manifest in TOML format
        
    Returns:
        str: Modified manifest text with LD_PRELOAD and trusted_files injected (if DCAP)
    """
    disable_flags = ['1', 'true', 'yes']
    if os.environ.get('DISABLE_RATLS_PRELOAD', '').lower() in disable_flags:
        return manifest_text
    
    if not is_dcap_manifest(manifest_text):
        return manifest_text
    
    lib_path = find_ratls_library()
    if not lib_path:
        return manifest_text
    
    if re.search(r'LD_PRELOAD\s*=\s*["\'].*' + re.escape(lib_path), manifest_text):
        return manifest_text
    
    lines = manifest_text.split('\n')
    result_lines = []
    injected = False
    in_loader_env = False
    last_loader_env_dotted_idx = None
    
    for i, line in enumerate(lines):
        if line.strip() == '[loader.env]':
            in_loader_env = True
            result_lines.append(line)
            result_lines.append(f'LD_PRELOAD = "{lib_path}"')
            injected = True
        elif in_loader_env and line.strip().startswith('LD_PRELOAD'):
            match = re.match(r'^(\s*LD_PRELOAD\s*=\s*")(.*?)("\s*)$', line)
            if match:
                prefix, existing, suffix = match.groups()
                result_lines.append(f'{prefix}{lib_path}:{existing}{suffix}')
                injected = True
            else:
                result_lines.append(line)
        elif in_loader_env and (line.strip().startswith('[') or not line.strip()):
            in_loader_env = False
            result_lines.append(line)
        # Handle dotted key syntax: loader.env.LD_PRELOAD
        elif re.match(r'^\s*loader\.env\.LD_PRELOAD\s*=\s*"', line):
            match = re.match(r'^(\s*loader\.env\.LD_PRELOAD\s*=\s*")(.*?)("\s*)$', line)
            if match:
                prefix, existing, suffix = match.groups()
                result_lines.append(f'{prefix}{lib_path}:{existing}{suffix}')
                injected = True
            else:
                result_lines.append(line)
        elif re.match(r'^\s*loader\.env\.', line):
            result_lines.append(line)
            last_loader_env_dotted_idx = len(result_lines) - 1
        else:
            result_lines.append(line)
    
    if not injected:
        if last_loader_env_dotted_idx is not None:
            result_lines.insert(last_loader_env_dotted_idx + 1, 
                              f'loader.env.LD_PRELOAD = "{lib_path}"')
            injected = True
        else:
            for i, line in enumerate(result_lines):
                if re.match(r'^\s*(loader|libos)\.entrypoint\s*=', line):
                    result_lines.insert(i + 1, f'loader.env.LD_PRELOAD = "{lib_path}"')
                    injected = True
                    break
    
    if not injected:
        result_lines.append(f'loader.env.LD_PRELOAD = "{lib_path}"')
    
    result_text = '\n'.join(result_lines)
    if f'file:{lib_path}' not in result_text:
        result_lines = result_text.split('\n')
        sgx_files_added = False
        
        for i, line in enumerate(result_lines):
            if re.match(r'^\s*sgx\.trusted_files\s*=\s*\[', line):
                bracket_depth = 1
                for j in range(i + 1, len(result_lines)):
                    if '[' in result_lines[j]:
                        bracket_depth += result_lines[j].count('[')
                    if ']' in result_lines[j]:
                        bracket_depth -= result_lines[j].count(']')
                        if bracket_depth == 0:
                            result_lines.insert(j, f'  "file:{lib_path}",')
                            sgx_files_added = True
                            break
                break
        
        if not sgx_files_added:
            last_sgx_line_idx = None
            for i, line in enumerate(result_lines):
                if re.match(r'^\s*sgx\.', line):
                    last_sgx_line_idx = i
            
            if last_sgx_line_idx is not None:
                result_lines.insert(last_sgx_line_idx + 1, '')
                result_lines.insert(last_sgx_line_idx + 2, 'sgx.trusted_files = [')
                result_lines.insert(last_sgx_line_idx + 3, f'  "file:{lib_path}",')
                result_lines.insert(last_sgx_line_idx + 4, ']')
        
        result_text = '\n'.join(result_lines)
    
    return result_text


def inject_into_manifest_object(manifest):
    """
    Inject LD_PRELOAD into a Gramine Manifest object before it's dumped.
    
    This modifies the manifest object in-place to add libratls-quote-verify.so
    to loader.env.LD_PRELOAD. This works for both file and stdout output.
    
    Args:
        manifest: A graminelibos.Manifest object
        
    Returns:
        bool: True if injection was performed or skipped (success), False on error
    """
    disable_flags = ['1', 'true', 'yes']
    if os.environ.get('DISABLE_RATLS_PRELOAD', '').lower() in disable_flags:
        return True
    
    lib_path = find_ratls_library()
    if not lib_path:
        return True
    
    try:
        if 'loader' not in manifest._manifest:
            manifest._manifest['loader'] = {}
        if 'env' not in manifest._manifest['loader']:
            manifest._manifest['loader']['env'] = {}
        
        existing_preload = manifest._manifest['loader']['env'].get('LD_PRELOAD', '')
        if lib_path in existing_preload:
            return True
        
        # Prepend the library to LD_PRELOAD
        if existing_preload:
            manifest._manifest['loader']['env']['LD_PRELOAD'] = f'{lib_path}:{existing_preload}'
        else:
            manifest._manifest['loader']['env']['LD_PRELOAD'] = lib_path
        
        return True
        
    except Exception as e:
        print(f'Warning: Failed to inject LD_PRELOAD into manifest object: {e}',
              file=sys.stderr)
        return True


def find_ratls_library():
    """
    Find the libratls-quote-verify.so library path.
    
    Returns:
        str: Path to the library, or None if not found
    """
    if 'RATLS_PRELOAD_PATH' in os.environ:
        path = os.environ['RATLS_PRELOAD_PATH']
        if os.path.isfile(path):
            is_link=os.path.islink(path)
            if is_link:
                path=os.path.realpath(path)
                if os.path.isfile(path):
                    return path
            return path
    
    search_paths = [
        '/usr/local/lib/libratls-quote-verify.so',
        '/usr/local/lib/x86_64-linux-gnu/libratls-quote-verify.so',
        '/usr/lib/x86_64-linux-gnu/libratls-quote-verify.so',
    ]
    
    for path in search_paths:
        if os.path.isfile(path):
            is_link=os.path.islink(path)
            if is_link:
                path=os.path.realpath(path)
                if os.path.isfile(path):
                    return path
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
                        is_link=os.path.islink(path)
                        if is_link:
                            path=os.path.realpath(path)
                            if os.path.isfile(path):
                                return path
                        return path
    except Exception:
        print("error find_ratls_library")
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
