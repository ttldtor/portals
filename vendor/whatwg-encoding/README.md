# WHATWG Encoding index data

The Windows-1251 and Windows-1252 mappings in `vendor/whatwg-encoding/windows_125x.hpp`
and independent golden test tables in `tests/fixtures/encoding/windows_125x_expected.hpp` derive from the WHATWG
Encoding Standard indexes dated 2024-09-18:

- [Windows-1251](https://encoding.spec.whatwg.org/index-windows-1251.txt), identifier `7592ef921679ba168b00a9e9afa3b4eebd67bf13dc7e84c4b6e120de856826e0`.
- [Windows-1252](https://encoding.spec.whatwg.org/index-windows-1252.txt), identifier `e56d49d9176e9a412283cf29ac9bd613f5620462f2a080a84eceaf974cfa18b7`.

The incorporated source-code portions use the BSD 3-Clause terms stated in the
[upstream license](LICENSE). No WHATWG runtime library is required. The index
identifiers pin the data; an upstream repository commit was not established.

The data headers retain WHATWG copyright and BSD-3-Clause notices. Portals decoder
and test logic use separate files under the project license; their copyright
notices do not apply to these imported mapping tables.
