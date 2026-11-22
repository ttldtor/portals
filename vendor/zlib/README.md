# zlib provenance

The optional gzip adapter uses the inflate-only subset of [zlib 1.4](https://www.zlib.net/),
released November 4, 2026. The unmodified upstream files and their SHA-256 hashes
are listed in `manifest.json`. The archive hash is independently published on
the upstream home page. CMake verifies these bytes offline when gzip is enabled.

Upstream copyright and the complete [zlib license](LICENSE) are preserved.
Portals does not claim authorship of these files. Modified versions must be
identified; the upstream notice must remain in source distributions. Portals'
BSL-1.0 license remains unchanged.

Only inflate, checksums and their support code are compiled. Deflate, gz-file
wrappers, minizip and contrib code are excluded. All compiled linked symbols
are prefixed by private compiler definitions in `symbols.cmake`; upstream source
bytes remain unchanged. No zlib types or headers are exposed through Portals'
public headers. The resulting objects are included in the optional gzip archive,
so installed consumers need neither a zlib package nor network access.

The default build does not compile or link this component. Enable
`PORTALS_ENABLE_GZIP` and link `portals::gzip` explicitly. Updating the version
requires checking upstream security changes, license, archive provenance,
restart/cancellation tests and installed consumers with and without the adapter.
