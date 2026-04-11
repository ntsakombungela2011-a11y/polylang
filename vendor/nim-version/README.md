# Nim Version Pin
Nim binary (~110 MB) is too large to commit to Git directly.
Build workflows download and cache it using the version in VERSION.
To update: re-run the Vendor Dependencies workflow with a new nim_version.
