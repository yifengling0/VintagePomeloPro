# Archived device harnesses

These former device automation and smoke sources are retained outside `entry/src/main` for offline contract tests and historical diagnosis. They are not application modules, dependencies, native targets or packaged runtime payloads. Their old relative imports describe the original device environment; this directory is not an installable application.

Use the normal game library and per-game settings for product launches. The release boundary rejects the former automation entry points and `libsmoke_napi.so`, including copies left in build caches.
