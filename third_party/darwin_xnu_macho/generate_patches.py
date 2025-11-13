#!/usr/bin/env python3
"""
Script to generate patch files documenting local modifications to headers.

This script should be run after manually editing headers to add cross-platform
compatibility fixes. It will generate patch files that can be applied by
update_headers.py.

Usage: python3 generate_patches.py

The script will:
1. Fetch the original upstream headers
2. Compare them to the current headers
3. Generate patch files
"""

import os
import subprocess
import sys
from pathlib import Path
from update_headers import (
    MetadataParser,
    fetch_file_from_github,
    XNU_HEADERS,
    CCTOOLS_HEADERS,
)


def generate_patch(original_content: bytes, modified_file: Path, output_patch: Path) -> bool:
    """
    Generate a patch file comparing original content to modified file.

    Args:
        original_content: Original file content from upstream
        modified_file: Path to the modified file
        output_patch: Path where patch should be written

    Returns:
        True if a patch was generated, False otherwise
    """
    # Write original content to a temporary file
    temp_original = modified_file.parent / (modified_file.name + ".orig")
    with open(temp_original, 'wb') as f:
        f.write(original_content)

    try:
        # Generate diff
        result = subprocess.run(
            ["diff", "-u", str(temp_original), str(modified_file)],
            capture_output=True,
            text=True
        )

        if result.returncode == 0:
            return False
        elif result.returncode == 1:
            # Files differ, generate new patch content
            # Adjust paths in diff output to be relative to script directory
            new_patch_content = result.stdout
            new_patch_content = new_patch_content.replace(
                str(temp_original), str(modified_file.name + ".orig")
            )
            new_patch_content = new_patch_content.replace(
                str(modified_file), str(modified_file.name)
            )

            # Check if existing patch has the same content (ignoring timestamp lines)
            if output_patch.exists():
                with open(output_patch, 'r') as f:
                    old_patch_content = f.read()

                # Compare patches ignoring the first two lines (which contain timestamps)
                old_lines = old_patch_content.split('\n')[2:]  # Skip timestamp lines
                new_lines = new_patch_content.split('\n')[2:]  # Skip timestamp lines

                if old_lines == new_lines:
                    # Patch content is identical, don't update
                    return False

            with open(output_patch, 'w') as f:
                f.write(new_patch_content)
            return True
        else:
            raise RuntimeError(f"diff failed: {result.stderr}")

    finally:
        if temp_original.exists():
            temp_original.unlink()


def process_headers(
    repo_name: str,
    repo_url: str,
    version: str,
    headers: list,
    script_dir: Path,
    patch_dir: Path
) -> list:
    """
    Process headers from a repository and generate patches.

    Args:
        repo_name: Human-readable repository name (e.g., "XNU")
        repo_url: Git repository URL
        version: Git commit hash
        headers: List of (source_path, dest_path) tuples
        script_dir: Script directory Path
        patch_dir: Patches directory Path

    Returns:
        List of patch filenames that were generated or unchanged

    Raises:
        Exception: If an error occurs processing a header
    """
    patches_generated = []
    print(f"=== Processing {repo_name} headers ===")

    for source_path, dest_path in headers:
        print(f"  Checking: {dest_path}")
        modified_file = script_dir / dest_path

        if not modified_file.exists():
            print(f"    Warning: Modified file not found, skipping")
            continue

        original_content = fetch_file_from_github(repo_url, version, source_path)

        patch_name = dest_path.replace("/", "_") + ".patch"
        patch_file = patch_dir / patch_name

        if generate_patch(original_content, modified_file, patch_file):
            print(f"    Generated patch: {patch_name}")
            patches_generated.append(patch_name)
        else:
            if patch_file.exists():
                print(f"    Patch unchanged: {patch_name}")
                patches_generated.append(patch_name)
            else:
                print(f"    No modifications detected")

    print()
    return patches_generated


def main():
    script_dir = Path(__file__).parent.resolve()
    os.chdir(script_dir)

    print("=== Darwin XNU/Mach-O Header Patch Generator ===")
    print()

    metadata_path = script_dir / "METADATA"
    if not metadata_path.exists():
        print(f"Error: METADATA file not found at {metadata_path}")
        return 1

    parser = MetadataParser(metadata_path)

    try:
        xnu_url, xnu_version = parser.get_repo_info("xnu")
        cctools_url, cctools_version = parser.get_repo_info("cctools")
    except ValueError as e:
        print(f"Error parsing METADATA: {e}")
        return 1

    print(f"XNU repository:     {xnu_url}")
    print(f"XNU version:        {xnu_version}")
    print(f"cctools repository: {cctools_url}")
    print(f"cctools version:    {cctools_version}")
    print()

    patch_dir = script_dir / "patches"
    patch_dir.mkdir(exist_ok=True)
    print(f"Patches will be written to: {patch_dir}")
    print()

    patches_generated = []
    try:
        patches_generated.extend(
            process_headers("XNU", xnu_url, xnu_version, XNU_HEADERS, script_dir, patch_dir)
        )
        patches_generated.extend(
            process_headers("cctools", cctools_url, cctools_version, CCTOOLS_HEADERS, script_dir, patch_dir)
        )
    except Exception as e:
        print(f"Error: {e}")
        return 1

    print("=== Patch Generation Complete ===")
    if patches_generated:
        print(f"Generated {len(patches_generated)} patch file(s):")
        for patch in patches_generated:
            print(f"  - {patch}")
        print()
        print("These patches will be automatically applied by update_headers.py")
    else:
        print("No patches generated - files are identical to upstream")

    return 0


if __name__ == "__main__":
    sys.exit(main())
