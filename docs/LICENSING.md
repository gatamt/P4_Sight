# Licensing and redistribution

P4_Sight is published as source under multiple licenses. The top-level `LICENSE`
is a scope map, not a single license grant covering the entire repository.

## Source-level rules

- Preserve every file-level SPDX identifier and copyright notice.
- Preserve the nearest component license when redistributing vendored code.
- Treat the active `imx708_regs.h` and `imx708_settings.h` files as
  GPL-2.0-only.
- Do not describe the repository or its complete firmware as Apache-2.0-only.
- Do not move GPL-derived register material into an Apache-only file without a
  documented independent source or permission from the relevant rightsholder.

## Firmware builds

The repository does not distribute a prebuilt firmware image. A locally built
image includes material under several licenses, including the active
GPL-2.0-only IMX708 register files and vendored Espressif components. Apache-2.0
alone is therefore not a sufficient description of the resulting image.

No representation is made that every component can be redistributed together
under one license. Anyone distributing a binary must perform their own license
compatibility review and provide all notices and source required by the
applicable licenses.

## Upstream contributions

An upstream Espressif submission should contain only material whose provenance
and proposed license can be demonstrated. The current GPL-derived register files
must not be submitted as Apache-2.0-only. A permissive upstream version requires
separate permission or independently sourced register material with documented
provenance.
