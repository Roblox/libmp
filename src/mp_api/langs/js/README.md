# JavaScript wrapper

`LibMP.js` is a single-file library for browsers and Node.js, with no runtime
package dependencies. Build it with `python3 src/scripts/build.py --js` or take
it from [LibMP releases](https://github.com/Roblox/libmp/releases). See
[Single-file wrappers](../README.md) for behavior shared by all wrappers, the
[API reference](../../../../docs/api-reference.md) for the complete API, and
[examples/js](../../../../examples/js) for a complete script.

Node.js (CommonJS):

```js
const LibMP = require("./LibMP.js");

async function main() {
    const libmp = await LibMP.create();
    const session = await libmp.Session.OpenFromFile("capture.x.html"); // or "capture.gprx"
    console.log(session.FetchGlobalDesc().tickToMsCpu);
    session.Dispose();
}

main();
```

In a browser, load `LibMP.js` as a regular script and use the global `LibMP`:

```js
const libmp = await LibMP.create();
const session = libmp.Session.OpenFromBuffer(captureArrayBuffer);
```

JavaScript-specific behavior:

- `LibMP.create()` and `Session.OpenFromFile` are asynchronous.
- Custom providers passed to `OpenFromExternalProvider` implement synchronous
  `getSize()` and `read(destination, blockId, offset, size)` methods, and
  optionally `open()` and `close()`; `offset` is a `bigint`, and `read` returns
  the number of bytes copied.
- `Tools.ConvertToBuffer` returns a `Uint8Array`. `Tools.ConvertToFile` is
  asynchronous; it writes a file under Node.js and starts a download in a
  browser.
- The top-level instance has no `Dispose()`. Call `Dispose()` on sessions,
  arrays, and iterators; garbage-collection finalizers are only a fallback.
