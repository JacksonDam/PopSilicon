#ifndef LP32_STEAM_UNWRAP_H
#define LP32_STEAM_UNWRAP_H

/*
 * Recover a clean, loadable i386 game image from a Steam-DRM-wrapped Peggle
 * Deluxe executable.  The DRM decryptor is 32-bit Intel code (Valve's
 * steamloader.dylib plus an appended stub) that cannot run natively on modern
 * macOS, so the compat runtime executes it against the already-mapped image and
 * this dumps the decrypted result.
 *
 * Requires that macho_image32_load() and compat_runtime32_initialize() have
 * already been called on `drm_image_path` (the image is mapped at its native
 * vmaddrs).  Writes a retail-equivalent image to `output_path`.  Returns 0 on
 * success.
 */
int steam_unwrap_run(const char *drm_image_path, const char *output_path);

#endif
