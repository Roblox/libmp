// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

// WasmMemoryAllocator: zero-copy FlatBuffers allocator backed by WASM linear memory.
// All ByteBuffer instances sharing this allocator read directly from WASM memory
// via an unsafe pointer. When WASM memory grows (reallocation), call SetMemory()
// once to update the pointer — all existing FlatBuffer views automatically see
// the new memory. This mirrors the Luau SetGlobalBuffer(W_mem) pattern.

using System;
using System.Runtime.InteropServices;
using Google.FlatBuffers;

namespace LibMP
{
    public sealed unsafe class WasmMemoryAllocator : ByteBufferAllocator
    {
        private byte* _ptr;
        private int _length;
        private bool _valid;

        public WasmMemoryAllocator()
        {
            _ptr = null;
            _length = 0;
            _valid = false;
            Length = 0;
        }

        /// <summary>
        /// Update the backing memory pointer. Called only on WASM memory growth.
        /// All FlatBuffer views sharing this allocator will immediately read from
        /// the new memory region.
        /// </summary>
        public void SetMemory(byte* ptr, int length)
        {
            _ptr = ptr;
            _length = length;
            _valid = true;
            Length = length;
        }

        internal void Invalidate()
        {
            _ptr = null;
            _length = 0;
            _valid = false;
            Length = 0;
        }

        private void ThrowIfInvalid()
        {
            if (!_valid)
                throw new ObjectDisposedException(nameof(WasmMemoryAllocator));
        }

        public override Span<byte> Span
        {
            get
            {
                ThrowIfInvalid();
                return new Span<byte>(_ptr, _length);
            }
        }

        public override ReadOnlySpan<byte> ReadOnlySpan
        {
            get
            {
                ThrowIfInvalid();
                return new ReadOnlySpan<byte>(_ptr, _length);
            }
        }
        public override Memory<byte> Memory => throw new NotSupportedException("Unmanaged memory cannot be wrapped in Memory<byte>");
        public override ReadOnlyMemory<byte> ReadOnlyMemory => throw new NotSupportedException("Unmanaged memory cannot be wrapped in ReadOnlyMemory<byte>");

        public override void GrowFront(int newSize)
        {
            throw new NotSupportedException("WasmMemoryAllocator does not support GrowFront");
        }
    }
}
