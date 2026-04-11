# Julia Version Pin

Julia cannot be vendored directly into Git — the binary distribution
contains files exceeding GitHub's 100 MB hard limit (sys.so is ~252 MB).

**Pinned version:** 1.10.4
**Download URL:** see DOWNLOAD_URL file

The build workflows (build-linux.yml) download Julia at build time
and cache it using the version string as the cache key.
To update Julia: re-run the Vendor Dependencies workflow with a new
`julia_version` input.
