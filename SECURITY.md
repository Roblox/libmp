# Security Guarantees

LibMP is used both as a native C/C++ library and through sandboxed, VM-based language
wrappers such as Luau. It accepts performance-profile dumps in GPRX format and a restricted
set of GPRX-formatted requests and responses through the MicroProfiler control channel.

LibMP guarantees that GPRX data it produces in memory can be parsed without memory-safety
errors or crashes. The native reader also performs defensive validation of arbitrary GPRX
input, but such input does not carry the same strict guarantee. Untrusted captures should be
processed through a sandboxed wrapper such as Luau.

Native trusted-input modes require immutable, well-formed data from a trusted source. Data
crossing a file system or network trust boundary should be signed or protected with
authenticated encryption and verified before parsing.

The control channel is designed to process arbitrary incoming data without memory-safety
errors or crashes. It accepts only its restricted command set and validates messages before
using their contents.

LibMP does not provide termination guarantees. An input may consume unbounded CPU time or
memory.

# Reporting a Vulnerability

Report security bugs through [Roblox HackerOne](https://hackerone.com/roblox). Please follow
the program rules and do not disclose security vulnerabilities through public issues.
