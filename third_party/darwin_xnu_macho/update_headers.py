#!/usr/bin/env python3
"""
Script to update darwin_xnu_macho headers from upstream sources.

This script fetches headers from Apple's open source repositories and applies
local modifications to make them work cross-platform in bloaty.

Usage: python3 update_headers.py [--dry-run]

The script will:
1. Parse METADATA to get the git repository URLs and commit hashes
2. Fetch the specified headers from those commits
3. Apply local modifications via patch files
4. Update the headers in place
"""

import argparse
import os
import re
import subprocess
import sys
import urllib.request
import urllib.error
from pathlib import Path
from typing import Dict, List, Tuple


# Format: (source_path_in_repo, destination_path)
XNU_HEADERS = [
    ("osfmk/mach/machine.h", "mach/machine.h"),
    ("osfmk/mach/vm_prot.h", "mach/vm_prot.h"),
]

CCTOOLS_HEADERS = [
    ("include/mach-o/fat.h", "mach-o/fat.h"),
    ("include/mach-o/loader.h", "mach-o/loader.h"),
    ("include/mach-o/nlist.h", "mach-o/nlist.h"),
]


class MetadataParser:
    """Parser for the METADATA file format."""

    def __init__(self, metadata_path: Path):
        self.metadata_path = metadata_path
        with open(metadata_path, 'r') as f:
            self.content = f.read()

    def parse_third_party_blocks(self) -> List[Dict[str, str]]:
        """Parse all third_party blocks from METADATA."""
        blocks = []
        current_block = None
        in_third_party = False
        brace_depth = 0

        for line in self.content.split('\n'):
            original_line = line
            line = line.strip()

            if line == 'third_party {':
                in_third_party = True
                brace_depth = 1
                current_block = {}
            elif line.endswith('{') and in_third_party:
                # Nested block like "url {", just track depth
                brace_depth += 1
            elif line == '}' and in_third_party:
                brace_depth -= 1
                if brace_depth == 0:
                    # End of third_party block
                    in_third_party = False
                    if current_block:
                        blocks.append(current_block)
                        current_block = None
            elif in_third_party and current_block is not None:
                # Parse fields like: value: "https://..." or version: "abc123"
                # Also handle unquoted fields like: version: abc123
                match = re.match(r'(\w+):\s*"([^"]+)"', line)
                if match:
                    key, value = match.groups()
                    current_block[key] = value
                else:
                    # Try without quotes
                    match = re.match(r'(\w+):\s*(\S+)', line)
                    if match:
                        key, value = match.groups()
                        current_block[key] = value

        return blocks

    def get_repo_info(self, repo_name: str) -> Tuple[str, str]:
        """
        Get repository URL and version for a specific repo.

        Args:
            repo_name: Either "xnu" or "cctools"

        Returns:
            Tuple of (url, version)
        """
        blocks = self.parse_third_party_blocks()

        for block in blocks:
            url = block.get('value', '')
            if repo_name in url:
                return url, block.get('version', '')

        raise ValueError(f"Could not find {repo_name} repository in METADATA")


def fetch_file_from_github(repo_url: str, commit: str, source_path: str) -> bytes:
    """
    Fetch a file from GitHub at a specific commit.

    Args:
        repo_url: Git repository URL (e.g., https://github.com/apple-oss-distributions/xnu)
        commit: Git commit hash
        source_path: Path to file within the repository

    Returns:
        File contents as bytes
    """
    # Convert git URL to raw.githubusercontent.com URL
    # https://github.com/apple-oss-distributions/xnu ->
    # https://raw.githubusercontent.com/apple-oss-distributions/xnu
    if 'github.com' not in repo_url:
        raise ValueError(f"Unsupported repository URL: {repo_url}")

    repo_path = repo_url.replace('https://github.com/', '')
    repo_path = repo_path.replace('.git', '')
    raw_url = f"https://raw.githubusercontent.com/{repo_path}/{commit}/{source_path}"

    print(f"    Downloading: {raw_url}")

    try:
        with urllib.request.urlopen(raw_url, timeout=30) as response:
            return response.read()
    except urllib.error.HTTPError as e:
        raise RuntimeError(f"Failed to download {raw_url}: {e}")
    except urllib.error.URLError as e:
        raise RuntimeError(f"Network error downloading {raw_url}: {e}")
    except Exception as e:
        raise RuntimeError(f"Unexpected error downloading {raw_url}: {type(e).__name__}: {e}")


def apply_patches(script_dir: Path, dry_run: bool) -> None:
    """
    Apply patch files from the patches/ directory to .new files.
    After applying, the .new files are renamed to their final names.

    Args:
        script_dir: Directory containing this script
        dry_run: If True, only print what would be done
    """
    patch_dir = script_dir / "patches"

    if not patch_dir.exists():
        print("  No patches directory found")
        if not dry_run:
            print("  Creating patches directory for future use")
            patch_dir.mkdir(parents=True, exist_ok=True)
        return

    patch_files = sorted(patch_dir.glob("*.patch"))
    if not patch_files:
        print("  No patch files found")
        return

    # Temporarily rename .new files to their final names so patches can apply
    renamed_files = []
    if not dry_run:
        for new_file in script_dir.rglob("*.new"):
            final_name = Path(str(new_file)[:-4])  # Remove '.new'
            if final_name.exists():
                final_name.unlink()
            new_file.rename(final_name)
            renamed_files.append(final_name)

    # Map patch files to the directories where they should be applied
    patch_dirs = {
        "mach_machine.h.patch": "mach",
        "mach_vm_prot.h.patch": "mach",
        "mach-o_loader.h.patch": "mach-o",
        "mach-o_fat.h.patch": "mach-o",
        "mach-o_nlist.h.patch": "mach-o",
    }

    for patch_file in patch_files:
        print(f"  Applying: {patch_file.name}")
        if not dry_run:
            # Determine which directory to apply the patch in
            patch_dir = script_dir / patch_dirs.get(patch_file.name, ".")

            result = subprocess.run(
                ["patch", "-p0", "-f", "--no-backup-if-mismatch", "-i", str(patch_file.absolute())],
                cwd=patch_dir,
                capture_output=True,
                text=True,
                input=""  # Provide empty stdin to avoid hanging on prompts
            )
            if result.returncode != 0:
                print(f"    Warning: patch returned {result.returncode}")
                if result.stderr:
                    print(f"    stderr: {result.stderr}")
                if result.stdout:
                    print(f"    stdout: {result.stdout}")
            else:
                print(f"    Success")

    if not dry_run and renamed_files:
        print("  Headers installed with local modifications applied")


def main():
    parser = argparse.ArgumentParser(
        description="Update darwin_xnu_macho headers from upstream sources"
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Show what would be done without making changes"
    )
    args = parser.parse_args()

    script_dir = Path(__file__).parent.resolve()
    os.chdir(script_dir)

    print("=== Darwin XNU Mach-O Header Update Script ===")
    print()

    if args.dry_run:
        print("Dry run mode - No files will be modified")
        print()

    metadata_path = script_dir / "METADATA"
    if not metadata_path.exists():
        print(f"Error: METADATA file not found at {metadata_path}")
        return 1

    metadata_parser = MetadataParser(metadata_path)

    try:
        xnu_url, xnu_version = metadata_parser.get_repo_info("xnu")
        cctools_url, cctools_version = metadata_parser.get_repo_info("cctools")
    except ValueError as e:
        print(f"Error parsing METADATA: {e}")
        return 1

    print(f"XNU repository:     {xnu_url}")
    print(f"XNU version:        {xnu_version}")
    print(f"cctools repository: {cctools_url}")
    print(f"cctools version:    {cctools_version}")
    print()

    print("=== Fetching XNU headers ===")
    for source_path, dest_path in XNU_HEADERS:
        print(f"  {source_path} -> {dest_path}")
        if not args.dry_run:
            try:
                content = fetch_file_from_github(xnu_url, xnu_version, source_path)
                dest_file = script_dir / (dest_path + ".new")
                dest_file.parent.mkdir(parents=True, exist_ok=True)
                with open(dest_file, 'wb') as f:
                    f.write(content)
                print(f"    Saved to: {dest_file}")
            except Exception as e:
                print(f"    Error: {e}")
                return 1
    print()

    print("=== Fetching cctools headers ===")
    for source_path, dest_path in CCTOOLS_HEADERS:
        print(f"  {source_path} -> {dest_path}")
        if not args.dry_run:
            try:
                content = fetch_file_from_github(cctools_url, cctools_version, source_path)
                dest_file = script_dir / (dest_path + ".new")
                dest_file.parent.mkdir(parents=True, exist_ok=True)
                with open(dest_file, 'wb') as f:
                    f.write(content)
                print(f"    Saved to: {dest_file}")
            except Exception as e:
                print(f"    Error: {e}")
                return 1
    print()

    if args.dry_run:
        print("=== Dry run complete ===")
        return 0

    print("=== Applying local modifications ===")
    apply_patches(script_dir, args.dry_run)
    print()

    print("=== Update complete ===")
    print()
    print("Next steps:")
    print("1. Review the changes: git diff")
    print("2. If there are new local modifications needed, edit the headers")
    print("3. Generate patches: python3 generate_patches.py")
    print("4. Update METADATA last_upgrade_date to today's date")
    print("5. Commit the changes")

    return 0


if __name__ == "__main__":
    sys.exit(main())
