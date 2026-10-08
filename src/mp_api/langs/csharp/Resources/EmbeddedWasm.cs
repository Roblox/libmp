// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

using System;
using System.IO;

namespace LibMP
{
    // Ships the WASM module as a binary embedded resource (see LibMP.csproj) and
    // exposes it through the same GetBytes() contract used by the base64 class that
    // scripts/build_csharp.py generates for the single-file bundle build.
    //
    // Kept in a subfolder so build_csharp.py's non-recursive glob over
    // mp_api/langs/csharp/*.cs does not pick it up (which would collide with the
    // generated base64 class in the bundle).
    public static class EmbeddedWasm
    {
        private const string ResourceName = "LibMP.LibMpCore.wasm";

        private static readonly Lazy<byte[]> _bytes = new(() =>
        {
            var asm = typeof(EmbeddedWasm).Assembly;
            using var stream = asm.GetManifestResourceStream(ResourceName)
                ?? throw new InvalidOperationException(
                    $"Embedded WASM resource '{ResourceName}' not found in {asm.FullName}.");
            using var ms = new MemoryStream();
            stream.CopyTo(ms);
            return ms.ToArray();
        });

        public static byte[] GetBytes() => _bytes.Value;
    }
}
