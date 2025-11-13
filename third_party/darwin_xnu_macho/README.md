# Darwin XNU and Mach-O Headers

This directory contains header files from Apple's open source XNU kernel and cctools projects.
These headers define the Mach-O binary format and related kernel types used by bloaty to parse Mach-O binaries.

## Source Repositories

The headers are obtained from:

- **XNU (mach/ headers)**: https://github.com/apple-oss-distributions/xnu
  - `mach/machine.h` - CPU type definitions
  - `mach/vm_prot.h` - Virtual memory protection flags

- **cctools (mach-o/ headers)**: https://github.com/apple-oss-distributions/cctools
  - `mach-o/fat.h` - Universal binary format
  - `mach-o/loader.h` - Mach-O file structure
  - `mach-o/nlist.h` - Symbol table format

The specific versions are documented in the `METADATA` file.

## Local Modifications

To make these headers work cross-platform, we apply minimal local modifications:

1. **Cross-platform includes**: Add `__has_include()` guards for system headers
2. **Fallback definitions**: Provide fallback typedefs when system headers aren't available
3. **Bundled headers**: Reference bundled copies instead of system headers

All local modifications are documented as patch files in the `patches/` directory.

## Reproducibility

The header update process is fully reproducible using the provided Python scripts:

### Updating Headers

To update to newer versions of the headers:

1. Edit `METADATA` to specify new git commit hashes
2. Run the update script:
   ```bash
   python3 update_headers.py
   ```

This will:
- Fetch the specified headers from GitHub
- Apply local modifications from `patches/`
- Install the modified headers

### Regenerating Patches

If you need to add new local modifications:

1. Manually edit the header files as needed
2. Regenerate the patches:
   ```bash
   python3 generate_patches.py
   ```

This will:
- Fetch the original upstream headers
- Compare them to your modified versions
- Generate/update patch files in `patches/`

### Dry Run

To see what would happen without making changes:

```bash
python3 update_headers.py --dry-run
```

## Patch Files

Current patches:

- **`mach_machine.h.patch`**: Adds `__has_include()` guards and fallback `integer_t` typedef for cross-platform compatibility
- **`mach-o_loader.h.patch`**: Changes `#include <mach/vm_prot.h>` to `#include "../mach/vm_prot.h"` to use bundled header
