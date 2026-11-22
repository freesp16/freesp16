# The project's identity: the ONLY file that spells the project's name.
#
# Everything else derives from these values: CMake targets, the generated C++ header (ProjectIdentity.h), CI job
# names, package and archive names, the emulator mod id, UI strings and environment variable prefixes.
# tools/check-identity.sh (a CI stage) fails if any other tracked file contains a copy of them.
# To rename the project: tools/rename.sh <name> "<Display Name>" <reverse.dns.id>
#
# Keep the format: one set(KEY "value") per line. tools/identity.py parses it with a regular expression.
#
# NAME          machine name: lowercase letters, digits and hyphens; targets, packages, mod id, file names
# DISPLAY_NAME  shown to users (window title, about box, package description). Must not contain third-party
#               trademarks
# ID            reverse-DNS identifier (config folder names, desktop metadata)
# VERSION       semantic version of the app; the release tag is v<VERSION>
set(PROJECT_IDENTITY_NAME "freesp16")
set(PROJECT_IDENTITY_DISPLAY_NAME "Free Sixteen")
set(PROJECT_IDENTITY_ID "org.freesp16.app")
set(PROJECT_IDENTITY_VERSION "0.1.0")
