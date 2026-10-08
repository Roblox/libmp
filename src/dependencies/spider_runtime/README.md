# Spider Luau runtime

LibMP's Luau build uses [Spider](https://github.com/SovereignSatellite/Spider) to
translate its WebAssembly core to Luau. Spider embeds parts of its Luau runtime
into the resulting `LibMP.luau` file.

The runtime is distributed under the Mozilla Public License 2.0; see
[LICENSE](LICENSE). Its source is in the
[Spider repository](https://github.com/SovereignSatellite/Spider) at
`Targets/Luau/Printer/runtime`.
