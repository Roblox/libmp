// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

// LibMP for C#
// Access Roblox MicroProfiler data from .NET applications.
// %LIBMP_SHARED_METADATA%
//
// Requirements:
// - .NET 8.
// - Wasmtime NuGet package: https://www.nuget.org/packages/Wasmtime
// - Required .csproj setting: <AllowUnsafeBlocks>true</AllowUnsafeBlocks>
//
// Minimal usage:
//   using var libmp = LibMP.LibMPInstance.Create();
//   using var session = libmp.OpenFromFile("capture.x.html"); // or "capture.gprx"
//   Console.WriteLine(session?.FetchGlobalDesc()?.TickToMsCpu);
//

using System;
using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Text;
using Google.FlatBuffers;
using Wasmtime;

namespace LibMP
{
    using Types = global::MpTypes;

    public delegate int ControlMessageHandler(
        ReadOnlySpan<byte> request, Span<byte> response);

    public static class Enums
    {
        public static class AppType
        {
            public const Types.AppType Unknown = Types.AppType.Unknown;
            public const Types.AppType Client = Types.AppType.Client;
            public const Types.AppType Studio = Types.AppType.Studio;
            public const Types.AppType Server = Types.AppType.Server;
        }

        public static class ContainerFormat
        {
            public const Types.ContainerFormat Raw = Types.ContainerFormat.Raw;
            public const Types.ContainerFormat Compressed = Types.ContainerFormat.Compressed;
            public const Types.ContainerFormat CompressedAscii = Types.ContainerFormat.CompressedAscii;
            public const Types.ContainerFormat CompressedHtml = Types.ContainerFormat.CompressedHtml;
        }

        public static class ContentMode
        {
            public const Types.ContentMode Full = Types.ContentMode.Full;
            public const Types.ContentMode CountersOnly = Types.ContentMode.CountersOnly;
        }

        public static class DataSlot
        {
            public const uint Live = (uint)Types.DataSlot.Live;
            public const uint FirstUserManaged = (uint)Types.DataSlot.FirstUserManaged;
            public const uint LastUserManaged = (uint)Types.DataSlot.LastUserManaged;
            public const uint Temporary = (uint)Types.DataSlot.Temporary;
        }
    }

    public sealed class CaptureParams
    {
        public uint MaxFrameCount { get; set; }
        public Types.ContainerFormat ContainerFormat { get; set; } = Enums.ContainerFormat.Raw;
    }

    internal delegate bool ToolOutputHandler(ReadOnlySpan<byte> data);

    // =========================================================================
    // WASM MODULE WRAPPER
    // =========================================================================

    public class WasmModule : IDisposable
    {
        private Engine _engine;
        private Store _store;
        private Linker _linker;
        private Instance? _instance;
        private Memory? _memory;

        // Zero-copy allocator: FlatBuffer views read directly from WASM linear memory
        // via a raw pointer. Updated only on memory growth.
        private WasmMemoryAllocator _wasmAllocator = new();
        internal ByteBuffer GlobalBuffer { get; private set; }

        internal Func<int, int, int, int, int>? ExtControlMsg;
        internal Func<int, int, int>? ExtToolOutput;
        internal Action<int, int[], int>? ExtClearObjCacheCallback;
        internal Action<int, int>? ExtSessionNotificationCallback;
        internal Func<int, int>? GprxExtDataOpen;
        internal Func<int, int>? GprxExtDataClose;
        internal Func<int, int, int, int>? GprxExtDataGetSize;
        internal Func<int, int, int, int, int, int, int>? GprxExtDataRead;
        internal readonly Dictionary<int, IDataProvider> IoRegistry = new();
        internal readonly Dictionary<int, WeakReference<Session>> SessionsByPtr = new();
        private int _nextSessionId;
        private readonly ConcurrentQueue<Action> _pendingReleases = new();
        private int _callDepth;

        public WasmModule()
        {
            _engine = new Engine();
            _store = new Store(_engine);
            _linker = new Linker(_engine);
            GlobalBuffer = new ByteBuffer(_wasmAllocator, 0);
        }

        public void Load(byte[] wasmBytes)
        {
            DefineImports();
            using var module = Module.FromBytes(_engine, "libmp", wasmBytes);
            _instance = _linker.Instantiate(_store, module);
            _memory = _instance.GetMemory("memory");
            if (_memory == null)
                throw new InvalidOperationException("WASM module does not export 'memory'");
            ResolveExports();
            UpdateGlobalBuffer();
        }

        private unsafe void UpdateGlobalBuffer()
        {
            if (_memory == null) return;
            var ptr = (byte*)_memory.GetPointer();
            var len = (int)_memory.GetLength();
            _wasmAllocator.SetMemory(ptr, len);
        }

        // Wasmtime's parameterless Memory.GetSpan() is obsolete (throws for memory
        // > ~2GB); use the GetSpan(address, length) overload with a bounded window.
        internal Span<byte> GetMemorySpan(int offset, int length)
        {
            return _memory!.GetSpan(offset, length);
        }

        internal long GetMemoryLength()
        {
            return _memory!.GetLength();
        }

        internal void WriteToMemory(int offset, byte[] data)
        {
            data.AsSpan().CopyTo(_memory!.GetSpan(offset, data.Length));
        }

        internal void WriteToMemory(int offset, ReadOnlySpan<byte> data)
        {
            data.CopyTo(_memory!.GetSpan(offset, data.Length));
        }

        internal byte[] ReadMemorySlice(int offset, int length)
        {
            return _memory!.GetSpan(offset, length).ToArray();
        }

        internal int AllocateSessionId()
        {
            if (_nextSessionId == int.MaxValue)
                throw new InvalidOperationException("External data handle space is exhausted");
            return ++_nextSessionId;
        }

        internal IDataProvider? GetIo(int handle)
        {
            IoRegistry.TryGetValue(handle, out var io);
            return io;
        }

        internal Session? GetSession(int ptr)
        {
            return SessionsByPtr.TryGetValue(ptr, out var weak) && weak.TryGetTarget(out var session)
                ? session : null;
        }

        private void DefineImports()
        {
            _linker.DefineFunction("env", "emscripten_notify_memory_growth", (int index) =>
            {
                UpdateGlobalBuffer();
            });

            _linker.DefineFunction("env", "LibMpExtConsoleLog", (int logLevel, int strPtr, int len) =>
            {
                var s = Encoding.UTF8.GetString(_memory!.GetSpan(strPtr, len));
                if (logLevel >= 2)
                    Console.Error.WriteLine(s);
                else
                    Console.WriteLine(s);
            });

            _linker.DefineFunction("env", "LibMpExtControlMsg", (int msgBufPtr, int msgSz, int respBufPtr, int respMaxSz) =>
            {
                if (ExtControlMsg != null)
                    return ExtControlMsg(msgBufPtr, msgSz, respBufPtr, respMaxSz);
                return 0;
            });

            _linker.DefineFunction("env", "LibMpExtToolOutput", (int dataPtr, int size) =>
            {
                return ExtToolOutput?.Invoke(dataPtr, size) ?? 0;
            });

            _linker.DefineFunction("env", "LibMpExtClearObjCacheCallback", (int handle, int bucketIdsPtr, int bucketCount) =>
            {
                if (ExtClearObjCacheCallback != null)
                {
                    var span = _memory!.GetSpan(bucketIdsPtr, bucketCount * 4);
                    var bucketIds = new int[bucketCount];
                    for (int i = 0; i < bucketCount; i++)
                    {
                        bucketIds[i] = BitConverter.ToInt32(span.Slice(i * 4, 4));
                    }
                    ExtClearObjCacheCallback(handle, bucketIds, bucketCount);
                }
            });

            _linker.DefineFunction("env", "LibMpExtSessionNotificationCallback",
                (int handle, int notificationFlags) =>
                    ExtSessionNotificationCallback?.Invoke(handle, notificationFlags));

            _linker.DefineFunction("env", "GprxExtDataOpen", (int handle) =>
            {
                return GprxExtDataOpen?.Invoke(handle) ?? 0;
            });

            _linker.DefineFunction("env", "GprxExtDataClose", (int handle) =>
            {
                return GprxExtDataClose?.Invoke(handle) ?? 0;
            });

            _linker.DefineFunction("env", "GprxExtDataGetSize", (int handle, int sizeLowPtr, int sizeHighPtr) =>
            {
                return GprxExtDataGetSize?.Invoke(handle, sizeLowPtr, sizeHighPtr) ?? 0;
            });

            _linker.DefineFunction("env", "GprxExtDataRead", (int handle, int dst, int blockId, int offsetLow, int offsetHigh, int sz) =>
            {
                return GprxExtDataRead?.Invoke(handle, dst, blockId, offsetLow, offsetHigh, sz) ?? 0;
            });

            // wasi_snapshot_preview1 stubs
            _linker.DefineFunction("wasi_snapshot_preview1", "fd_write", (int a, int b, int c, int d) => 0);
            _linker.DefineFunction("wasi_snapshot_preview1", "fd_close", (int a) => 0);
            _linker.DefineFunction("wasi_snapshot_preview1", "fd_seek", (int a, long b, int c, int d) => 0);
        }

        // Pre-resolved WASM function references (resolved once at load time)
        internal Function F_malloc = null!;
        internal Function F_free = null!;
        internal Function F_emscripten_stack_get_current = null!;
        internal Function F__emscripten_stack_alloc = null!;
        internal Function F__emscripten_stack_restore = null!;
        internal Function F_MpInit = null!;
        internal Function F_MpGetMemUsed = null!;
        internal Function F_MpGetMemReserved = null!;
        internal Function F_MpGetDataFormatVersion = null!;
        internal Function F_MpGetLibraryVersion = null!;
        internal Function F_MpToolConvertSession = null!;
        internal Function F_MpToolSetHtmlAssetsBaseUrl = null!;
        internal Function F_MpOpenFromMemTrusted = null!;
        internal Function F_MpOpenFromExternalDataTrusted = null!;
        internal Function F_MpCloseSession = null!;
        internal Function F_MpSyncWithDataSource = null!;
        internal Function F_MpSyncIfRequired = null!;
        internal Function F_MpSetSyncRequired = null!;
        internal Function F_MpSetClearCacheOnSyncRequired = null!;
        internal Function F_MpIsSessionValid = null!;
        internal Function F_MpGetSessionDataFormatVersion = null!;
        internal Function F_MpGetTrackCount = null!;
        internal Function F_MpGetTracks = null!;
        internal Function F_MpGetTrackDescFromArray = null!;
        internal Function F_MpGetSegmentCount = null!;
        internal Function F_MpGetSegments = null!;
        internal Function F_MpGetSegmentDescFromArray = null!;
        internal Function F_MpSelectTrack = null!;
        internal Function F_MpSelectSegment = null!;
        internal Function F_MpGetObjSize = null!;
        internal Function F_MpPushDataLock = null!;
        internal Function F_MpConsumeDataLock = null!;
        internal Function F_MpPopDataLock = null!;
        internal Function F_MpEnableObjectCache = null!;
        internal Function F_MpClearObjectCache = null!;
        internal Function F_MpGetLabelFromOffsetParts = null!;
        internal Function F_MpInternalGetLabelBufferRegions = null!;
        internal Function F_MpInternalGetLabelBufferRegionFromArray = null!;
        internal Function F_MpInternalGetLabelCacheChunkSize = null!;
        internal Function F_MpGetGlobalDesc = null!;
        internal Function F_MpGetGeneralInfo = null!;
        internal Function F_MpGetGroupIdMax = null!;
        internal Function F_MpGetGroupDesc = null!;
        internal Function F_MpGetTimerIds = null!;
        internal Function F_MpGetTimerDesc = null!;
        internal Function F_MpGetCounterIds = null!;
        internal Function F_MpGetCounterDesc = null!;
        internal Function F_MpGetCounterSamples = null!;
        internal Function F_MpGetLastCounterSample = null!;
        internal Function F_MpGetThreadIds = null!;
        internal Function F_MpGetThreadDesc = null!;
        internal Function F_MpGetThreadIdGpu = null!;
        internal Function F_MpGetFrameDesc = null!;
        internal Function F_MpGetFrameIdMin = null!;
        internal Function F_MpGetFrameIdMax = null!;
        internal Function F_MpGetPlaceIdSamples = null!;
        internal Function F_MpGetUtcTimestampSamples = null!;
        internal Function F_MpGetArraySize = null!;
        internal Function F_MpReleaseArray = null!;
        internal Function F_MpGetIdFromArray = null!;
        internal Function F_MpGetU64SampleFromArray = null!;
        internal Function F_MpGetCounterSampleFromArray = null!;
        internal Function F_MpGetCounterSampleFromArrayByFrameId = null!;
        internal Function F_MpGetLastCounterSampleFromArray = null!;
        internal Function F_MpCreateLogIterator = null!;
        internal Function F_MpDeleteLogIterator = null!;
        internal Function F_MpStepLogIterator = null!;
        internal Function F_MpRewindLogIterator = null!;
        internal Function F_MpRewindLogIteratorTo = null!;
        internal Function F_MpConfigureLogIterator = null!;
        internal Function F_MpGetLogIteratorConfigBuffer = null!;
        internal Function F_MpGetLogIteratorState = null!;
        internal Function F_MpGetLogIteratorThreadStackState = null!;
        internal Function F_MpGetLogIteratorThreadStackElement = null!;
        internal Function F_MpGetLogIteratorCurrentThreadStackElement = null!;
        internal Function F_MpGetLogIteratorCurrentEntryCount = null!;
        internal Function F_MpGetLogIteratorCurrentEntryPart = null!;
        internal Function F_MpGetLogIteratorCurrentLabelOffset = null!;
        internal Function F_MpGetLogIteratorScopeLabelCount = null!;
        internal Function F_MpGetLogIteratorCurrentScopeLabelCount = null!;
        internal Function F_MpGetLogIteratorScopeLabelOffset = null!;
        internal Function F_MpGetLogIteratorCurrentScopeLabelOffset = null!;
        internal Function F_MpCreateCounterIterator = null!;
        internal Function F_MpDeleteCounterIterator = null!;
        internal Function F_MpStepCounterIterator = null!;
        internal Function F_MpRewindCounterIterator = null!;
        internal Function F_MpConfigureCounterIterator = null!;
        internal Function F_MpGetCounterIteratorConfigBuffer = null!;
        internal Function F_MpGetCounterIteratorState = null!;
        internal Function F_MpGetCounterIteratorStackElement = null!;
        internal Function F_MpUseControlChannel = null!;
        internal Function F_MpIsBackendVersionCompatible = null!;
        internal Function F_MpIsBackendAccessible = null!;
        internal Function F_MpIsBackendReady = null!;
        internal Function F_MpControlEnableProfiler = null!;
        internal Function F_MpControlEnableCapture = null!;
        internal Function F_MpControlShowUI = null!;
        internal Function F_MpControlSetFrameLimit = null!;
        internal Function F_MpControlSetMemTargetMB = null!;
        internal Function F_MpGetTimerIdsByGroupId = null!;
        internal Function F_MpFindGroupId = null!;
        internal Function F_MpFindGroupIds = null!;
        internal Function F_MpFindTimerId = null!;
        internal Function F_MpFindTimerIds = null!;
        internal Function F_MpGetTimerIdsByGroupNames = null!;
        internal Function F_MpFindCounterId = null!;
        internal Function F_MpFindCounterIds = null!;
        internal Function F_MpFindThreadId = null!;
        internal Function F_MpFindThreadIds = null!;

        private void ResolveExports()
        {
            Function Resolve(string name) => _instance!.GetFunction(name)
                ?? throw new InvalidOperationException($"WASM export not found: {name}");

            F_malloc = Resolve("malloc");
            F_free = Resolve("free");
            F_emscripten_stack_get_current = Resolve("emscripten_stack_get_current");
            F__emscripten_stack_alloc = Resolve("_emscripten_stack_alloc");
            F__emscripten_stack_restore = Resolve("_emscripten_stack_restore");
            F_MpInit = Resolve("MpInit");
            F_MpGetMemUsed = Resolve("MpGetMemUsed");
            F_MpGetMemReserved = Resolve("MpGetMemReserved");
            F_MpGetDataFormatVersion = Resolve("MpGetDataFormatVersion");
            F_MpGetLibraryVersion = Resolve("MpGetLibraryVersion");
            F_MpToolConvertSession = Resolve("MpToolConvertSession");
            F_MpToolSetHtmlAssetsBaseUrl = Resolve("MpToolSetHtmlAssetsBaseUrl");
            F_MpOpenFromMemTrusted = Resolve("MpOpenFromMemTrusted");
            F_MpOpenFromExternalDataTrusted = Resolve("MpOpenFromExternalDataTrusted");
            F_MpCloseSession = Resolve("MpCloseSession");
            F_MpSyncWithDataSource = Resolve("MpSyncWithDataSource");
            F_MpSyncIfRequired = Resolve("MpSyncIfRequired");
            F_MpSetSyncRequired = Resolve("MpSetSyncRequired");
            F_MpSetClearCacheOnSyncRequired = Resolve("MpSetClearCacheOnSyncRequired");
            F_MpIsSessionValid = Resolve("MpIsSessionValid");
            F_MpGetSessionDataFormatVersion = Resolve("MpGetSessionDataFormatVersion");
            F_MpGetTrackCount = Resolve("MpGetTrackCount");
            F_MpGetTracks = Resolve("MpGetTracks");
            F_MpGetTrackDescFromArray = Resolve("MpGetTrackDescFromArray");
            F_MpGetSegmentCount = Resolve("MpGetSegmentCount");
            F_MpGetSegments = Resolve("MpGetSegments");
            F_MpGetSegmentDescFromArray = Resolve("MpGetSegmentDescFromArray");
            F_MpSelectTrack = Resolve("MpSelectTrack");
            F_MpSelectSegment = Resolve("MpSelectSegment");
            F_MpGetObjSize = Resolve("MpGetObjSize");
            F_MpPushDataLock = Resolve("MpPushDataLock");
            F_MpConsumeDataLock = Resolve("MpConsumeDataLock");
            F_MpPopDataLock = Resolve("MpPopDataLock");
            F_MpEnableObjectCache = Resolve("MpEnableObjectCache");
            F_MpClearObjectCache = Resolve("MpClearObjectCache");
            F_MpGetLabelFromOffsetParts = Resolve("MpGetLabelFromOffsetParts");
            F_MpInternalGetLabelBufferRegions = Resolve("MpInternalGetLabelBufferRegions");
            F_MpInternalGetLabelBufferRegionFromArray = Resolve("MpInternalGetLabelBufferRegionFromArray");
            F_MpInternalGetLabelCacheChunkSize = Resolve("MpInternalGetLabelCacheChunkSize");
            F_MpGetGlobalDesc = Resolve("MpGetGlobalDesc");
            F_MpGetGeneralInfo = Resolve("MpGetGeneralInfo");
            F_MpGetGroupIdMax = Resolve("MpGetGroupIdMax");
            F_MpGetGroupDesc = Resolve("MpGetGroupDesc");
            F_MpGetTimerIds = Resolve("MpGetTimerIds");
            F_MpGetTimerDesc = Resolve("MpGetTimerDesc");
            F_MpGetCounterIds = Resolve("MpGetCounterIds");
            F_MpGetCounterDesc = Resolve("MpGetCounterDesc");
            F_MpGetCounterSamples = Resolve("MpGetCounterSamples");
            F_MpGetLastCounterSample = Resolve("MpGetLastCounterSample");
            F_MpGetThreadIds = Resolve("MpGetThreadIds");
            F_MpGetThreadDesc = Resolve("MpGetThreadDesc");
            F_MpGetThreadIdGpu = Resolve("MpGetThreadIdGpu");
            F_MpGetFrameDesc = Resolve("MpGetFrameDesc");
            F_MpGetFrameIdMin = Resolve("MpGetFrameIdMin");
            F_MpGetFrameIdMax = Resolve("MpGetFrameIdMax");
            F_MpGetPlaceIdSamples = Resolve("MpGetPlaceIdSamples");
            F_MpGetUtcTimestampSamples = Resolve("MpGetUtcTimestampSamples");
            F_MpGetArraySize = Resolve("MpGetArraySize");
            F_MpReleaseArray = Resolve("MpReleaseArray");
            F_MpGetIdFromArray = Resolve("MpGetIdFromArray");
            F_MpGetU64SampleFromArray = Resolve("MpGetU64SampleFromArray");
            F_MpGetCounterSampleFromArray = Resolve("MpGetCounterSampleFromArray");
            F_MpGetCounterSampleFromArrayByFrameId = Resolve("MpGetCounterSampleFromArrayByFrameId");
            F_MpGetLastCounterSampleFromArray = Resolve("MpGetLastCounterSampleFromArray");
            F_MpCreateLogIterator = Resolve("MpCreateLogIterator");
            F_MpDeleteLogIterator = Resolve("MpDeleteLogIterator");
            F_MpStepLogIterator = Resolve("MpStepLogIterator");
            F_MpRewindLogIterator = Resolve("MpRewindLogIterator");
            F_MpRewindLogIteratorTo = Resolve("MpRewindLogIteratorTo");
            F_MpConfigureLogIterator = Resolve("MpConfigureLogIterator");
            F_MpGetLogIteratorConfigBuffer = Resolve("MpGetLogIteratorConfigBuffer");
            F_MpGetLogIteratorState = Resolve("MpGetLogIteratorState");
            F_MpGetLogIteratorThreadStackState = Resolve("MpGetLogIteratorThreadStackState");
            F_MpGetLogIteratorThreadStackElement = Resolve("MpGetLogIteratorThreadStackElement");
            F_MpGetLogIteratorCurrentThreadStackElement = Resolve("MpGetLogIteratorCurrentThreadStackElement");
            F_MpGetLogIteratorCurrentEntryCount = Resolve("MpGetLogIteratorCurrentEntryCount");
            F_MpGetLogIteratorCurrentEntryPart = Resolve("MpGetLogIteratorCurrentEntryPart");
            F_MpGetLogIteratorCurrentLabelOffset = Resolve("MpGetLogIteratorCurrentLabelOffset");
            F_MpGetLogIteratorScopeLabelCount = Resolve("MpGetLogIteratorScopeLabelCount");
            F_MpGetLogIteratorCurrentScopeLabelCount = Resolve("MpGetLogIteratorCurrentScopeLabelCount");
            F_MpGetLogIteratorScopeLabelOffset = Resolve("MpGetLogIteratorScopeLabelOffset");
            F_MpGetLogIteratorCurrentScopeLabelOffset = Resolve("MpGetLogIteratorCurrentScopeLabelOffset");
            F_MpCreateCounterIterator = Resolve("MpCreateCounterIterator");
            F_MpDeleteCounterIterator = Resolve("MpDeleteCounterIterator");
            F_MpStepCounterIterator = Resolve("MpStepCounterIterator");
            F_MpRewindCounterIterator = Resolve("MpRewindCounterIterator");
            F_MpConfigureCounterIterator = Resolve("MpConfigureCounterIterator");
            F_MpGetCounterIteratorConfigBuffer = Resolve("MpGetCounterIteratorConfigBuffer");
            F_MpGetCounterIteratorState = Resolve("MpGetCounterIteratorState");
            F_MpGetCounterIteratorStackElement = Resolve("MpGetCounterIteratorStackElement");
            F_MpUseControlChannel = Resolve("MpUseControlChannel");
            F_MpIsBackendVersionCompatible = Resolve("MpIsBackendVersionCompatible");
            F_MpIsBackendAccessible = Resolve("MpIsBackendAccessible");
            F_MpIsBackendReady = Resolve("MpIsBackendReady");
            F_MpControlEnableProfiler = Resolve("MpControlEnableProfiler");
            F_MpControlEnableCapture = Resolve("MpControlEnableCapture");
            F_MpControlShowUI = Resolve("MpControlShowUI");
            F_MpControlSetFrameLimit = Resolve("MpControlSetFrameLimit");
            F_MpControlSetMemTargetMB = Resolve("MpControlSetMemTargetMB");
            F_MpGetTimerIdsByGroupId = Resolve("MpGetTimerIdsByGroupId");
            F_MpFindGroupId = Resolve("MpFindGroupId");
            F_MpFindGroupIds = Resolve("MpFindGroupIds");
            F_MpFindTimerId = Resolve("MpFindTimerId");
            F_MpFindTimerIds = Resolve("MpFindTimerIds");
            F_MpGetTimerIdsByGroupNames = Resolve("MpGetTimerIdsByGroupNames");
            F_MpFindCounterId = Resolve("MpFindCounterId");
            F_MpFindCounterIds = Resolve("MpFindCounterIds");
            F_MpFindThreadId = Resolve("MpFindThreadId");
            F_MpFindThreadIds = Resolve("MpFindThreadIds");
        }

        // Invoke helpers (no string lookup, direct Function reference)
        internal int Invoke(Function f) { _callDepth++; object? r; try { r = f.Invoke(); } finally { _callDepth--; } return Finish(r); }
        internal int Invoke(Function f, int a0) { _callDepth++; object? r; try { r = f.Invoke((ValueBox)a0); } finally { _callDepth--; } return Finish(r); }
        internal int Invoke(Function f, int a0, int a1) { _callDepth++; object? r; try { r = f.Invoke((ValueBox)a0, (ValueBox)a1); } finally { _callDepth--; } return Finish(r); }
        internal int Invoke(Function f, int a0, int a1, int a2) { _callDepth++; object? r; try { r = f.Invoke((ValueBox)a0, (ValueBox)a1, (ValueBox)a2); } finally { _callDepth--; } return Finish(r); }
        internal int Invoke(Function f, int a0, int a1, int a2, int a3) { _callDepth++; object? r; try { r = f.Invoke((ValueBox)a0, (ValueBox)a1, (ValueBox)a2, (ValueBox)a3); } finally { _callDepth--; } return Finish(r); }
        internal int Invoke(Function f, int a0, int a1, int a2, int a3, int a4) { _callDepth++; object? r; try { r = f.Invoke((ValueBox)a0, (ValueBox)a1, (ValueBox)a2, (ValueBox)a3, (ValueBox)a4); } finally { _callDepth--; } return Finish(r); }
        internal int Invoke(Function f, int a0, int a1, int a2, int a3, int a4, int a5) { _callDepth++; object? r; try { r = f.Invoke((ValueBox)a0, (ValueBox)a1, (ValueBox)a2, (ValueBox)a3, (ValueBox)a4, (ValueBox)a5); } finally { _callDepth--; } return Finish(r); }

        private int Finish(object? result)
        {
            if (_callDepth == 0 && !_pendingReleases.IsEmpty)
                RunPendingReleases();
            return result is null ? 0 : Convert.ToInt32(result);
        }

        // QueueRelease is what finalizers do instead of calling into WASM, because .NET runs them
        // on its finalizer thread, concurrently with the owner's LibMP calls. The next top-level
        // call frees the object when it returns, after the native code is done with that call's
        // arguments; calls made from WASM callbacks leave the queue alone. Releasing at the end
        // means a method needs no GC.KeepAlive while it reads its pointer and module from fields,
        // rather than from locals kept across a call.
        internal void QueueRelease(Action release) => _pendingReleases.Enqueue(release);
        internal void QueueRelease(Function release, int ptr) => QueueRelease(() => Invoke(release, ptr));

        private void RunPendingReleases()
        {
            _callDepth++; // the releases' own calls must not drain the queue again
            try
            {
                while (_pendingReleases.TryDequeue(out var release))
                    release();
            }
            finally
            {
                _callDepth--;
            }
        }

        public void Dispose()
        {
            if (_instance != null && _callDepth != 0)
                throw new InvalidOperationException(
                    "a LibMP instance cannot be disposed from its own callback");

            if (_instance != null)
            {
                RunPendingReleases(); // lets finalized sessions close their data providers
                // Close the sessions still open so their data providers close too. Releases
                // queued from here on never run, since the store goes away.
                _callDepth++;
                foreach (int ptr in new List<int>(SessionsByPtr.Keys))
                {
                    if (GetSession(ptr) is Session session)
                        session.Dispose();
                    else
                        Invoke(F_MpCloseSession, ptr); // unreachable, but not finalized yet
                }
                _instance = null;
            }
            // FlatBuffer views share this allocator and may outlive their session. Stop them
            // from retaining a pointer into the store before that memory is released.
            _wasmAllocator.Invalidate();
            _memory = null;
            _linker?.Dispose();
            _store?.Dispose();
            _engine?.Dispose();
        }
    }

    // =========================================================================
    // HELPERS
    // =========================================================================

    public interface IDataProvider
    {
        bool Open();
        bool Close();
        ulong GetSize();
        int Read(Span<byte> destination, uint blockId, ulong offset, int size);
    }

    // =========================================================================
    // ARRAY CLASSES
    // =========================================================================

    public class ArrayBase : IDisposable
    {
        // Element getters take the session; after its Dispose they get 0 and return null
        private readonly Session _session;
        protected int ArrayPtr;
        protected WasmModule W;

        public ArrayBase(Session session, int arrayPtr)
        {
            W = session.Runtime;
            _session = session;
            ArrayPtr = arrayPtr;
        }

        protected int SessionPtr => _session.Pointer;

        public void Dispose()
        {
            if (ArrayPtr != 0)
            {
                W.Invoke(W.F_MpReleaseArray, ArrayPtr);
                ArrayPtr = 0;
            }
            GC.SuppressFinalize(this);
        }

        ~ArrayBase()
        {
            if (ArrayPtr != 0)
                W.QueueRelease(W.F_MpReleaseArray, ArrayPtr);
        }

        public int Size()
        {
            if (ArrayPtr == 0) return 0;
            return checked((int)(uint)W.Invoke(W.F_MpGetArraySize, ArrayPtr));
        }
    }

    public class ScalarArray : ArrayBase
    {
        private Function _getterFunc;

        public ScalarArray(Session session, int arrayPtr, Function getterFunc)
            : base(session, arrayPtr)
        {
            _getterFunc = getterFunc;
        }

        public uint Get(int index)
        {
            return (uint)W.Invoke(_getterFunc, SessionPtr, ArrayPtr, index);
        }

        public uint Fetch(int index) => Get(index);

        public uint[] FetchAll()
        {
            int count = Size();
            var result = new uint[count];
            for (int i = 0; i < count; i++)
                result[i] = Get(i);
            return result;
        }
    }

    public class TableArray<T> : ArrayBase where T : struct, IFlatbufferObject
    {
        private Function _getterFunc;

        public TableArray(Session session, int arrayPtr, Function getterFunc)
            : base(session, arrayPtr)
        {
            _getterFunc = getterFunc;
        }

        public T? Get(int index)
        {
            int ptr = W.Invoke(_getterFunc, SessionPtr, ArrayPtr, index);
            return LibMpHelpers.WrapTable<T>(W, ptr);
        }

        public T?[] FetchAll()
        {
            int count = Size();
            var result = new T?[count];
            for (int i = 0; i < count; i++)
                result[i] = Get(i);
            return result;
        }
    }

    public class StructArray<TView, TObject> : ArrayBase
        where TView : struct, IFlatbufferObject
        where TObject : class
    {
        private Function _getterFunc;
        private Func<TView, TObject> _unpackFunc;

        public StructArray(Session session, int arrayPtr,
            Function getterFunc, Func<TView, TObject> unpackFunc)
            : base(session, arrayPtr)
        {
            _getterFunc = getterFunc;
            _unpackFunc = unpackFunc;
        }

        public TView? Get(int index)
        {
            int ptr = W.Invoke(_getterFunc, SessionPtr, ArrayPtr, index);
            return LibMpHelpers.WrapStruct<TView>(W, ptr);
        }

        public TObject? Fetch(int index)
        {
            var view = Get(index);
            return view.HasValue ? _unpackFunc(view.Value) : null;
        }

        public TObject[] FetchAll()
        {
            int count = Size();
            var result = new TObject[count];
            for (int i = 0; i < count; i++)
            {
                var value = Fetch(i);
                if (value == null)
                    throw new InvalidOperationException("Array element unexpectedly returned null");
                result[i] = value;
            }
            return result;
        }
    }

    public class CounterSampleArray : ArrayBase
    {
        public CounterSampleArray(Session session, int arrayPtr)
            : base(session, arrayPtr) { }

        public Types.CounterSample? Get(int index)
        {
            int ptr = W.Invoke(W.F_MpGetCounterSampleFromArray, SessionPtr, ArrayPtr, index);
            return LibMpHelpers.WrapStruct<Types.CounterSample>(W, ptr);
        }

        public Types.CounterSampleT? Fetch(int index)
        {
            return Get(index)?.UnPack();
        }

        public Types.CounterSampleT[] FetchAll()
        {
            int count = Size();
            var result = new Types.CounterSampleT[count];
            for (int i = 0; i < count; i++)
                result[i] = Fetch(i)!;
            return result;
        }

        public Types.CounterSample? GetSampleByFrameId(int frameId)
        {
            int ptr = W.Invoke(W.F_MpGetCounterSampleFromArrayByFrameId, SessionPtr, ArrayPtr, frameId);
            return LibMpHelpers.WrapStruct<Types.CounterSample>(W, ptr);
        }

        public Types.CounterSampleT? FetchSampleByFrameId(int frameId)
        {
            return GetSampleByFrameId(frameId)?.UnPack();
        }

        public Types.CounterSample? GetLastSample()
        {
            int ptr = W.Invoke(W.F_MpGetLastCounterSampleFromArray, SessionPtr, ArrayPtr);
            return LibMpHelpers.WrapStruct<Types.CounterSample>(W, ptr);
        }

        public Types.CounterSampleT? FetchLastSample()
        {
            return GetLastSample()?.UnPack();
        }
    }

    // =========================================================================
    // HELPERS (wrapTable / wrapStruct)
    // =========================================================================

    internal static class LibMpHelpers
    {
        public static T? WrapTable<T>(WasmModule w, int ptr) where T : struct, IFlatbufferObject
        {
            if (ptr == 0) return null;
            var buf = w.GlobalBuffer;
            int offset = buf.GetInt(ptr);
            var obj = default(T);
            obj.__init(ptr + offset, buf);
            return obj;
        }

        public static T? WrapStruct<T>(WasmModule w, int ptr) where T : struct, IFlatbufferObject
        {
            if (ptr == 0) return null;
            var buf = w.GlobalBuffer;
            var obj = default(T);
            obj.__init(ptr, buf);
            return obj;
        }
    }

    // =========================================================================
    // LOG ITERATOR CLASS
    // =========================================================================

    public class LogIterator : IDisposable
    {
        private WasmModule _w;
        private Session _session;
        private int _iterPtr;

        internal LogIterator(WasmModule w, Session session)
        {
            _w = w;
            _session = session;
            _iterPtr = w.Invoke(w.F_MpCreateLogIterator, session.Pointer);
        }

        public void Dispose()
        {
            if (_iterPtr != 0)
            {
                _w.Invoke(_w.F_MpDeleteLogIterator, _iterPtr);
                _iterPtr = 0;
            }
            GC.SuppressFinalize(this);
        }

        ~LogIterator()
        {
            if (_iterPtr != 0)
                _w.QueueRelease(_w.F_MpDeleteLogIterator, _iterPtr);
        }

        public bool Configure(Types.LogIteratorConfigT config)
        {
            var builder = new FlatBufferBuilder(1024);
            var rootOffset = Types.LogIteratorConfig.Pack(builder, config);
            builder.Finish(rootOffset.Value);

            var buf = builder.SizedByteArray();
            int bufLen = buf.Length;

            int destPtr = _w.Invoke(_w.F_MpGetLogIteratorConfigBuffer, _iterPtr, bufLen);
            if (destPtr == 0) return false;
            _w.WriteToMemory(destPtr, buf);

            return _w.Invoke(_w.F_MpConfigureLogIterator, _iterPtr, 0) != 0;
        }

        public bool Step()
        {
            return _w.Invoke(_w.F_MpStepLogIterator, _iterPtr) != 0;
        }

        public void Rewind()
        {
            _w.Invoke(_w.F_MpRewindLogIterator, _iterPtr);
        }

        public void RewindTo(uint frameIdMin, uint frameIdMax)
        {
            _w.Invoke(_w.F_MpRewindLogIteratorTo, _iterPtr, (int)frameIdMin, (int)frameIdMax);
        }

        public Types.LogIteratorState? GetState()
        {
            int ptr = _w.Invoke(_w.F_MpGetLogIteratorState, _iterPtr);
            return LibMpHelpers.WrapStruct<Types.LogIteratorState>(_w, ptr);
        }

        public Types.LogIteratorThreadStackState? GetThreadStackState(uint threadId)
        {
            int ptr = _w.Invoke(_w.F_MpGetLogIteratorThreadStackState, _iterPtr, (int)threadId);
            return LibMpHelpers.WrapStruct<Types.LogIteratorThreadStackState>(_w, ptr);
        }

        public Types.LogIteratorThreadStackElement? GetThreadStackElement(uint threadId, int stackLevel)
        {
            int ptr = _w.Invoke(_w.F_MpGetLogIteratorThreadStackElement, _iterPtr, (int)threadId, stackLevel);
            return LibMpHelpers.WrapStruct<Types.LogIteratorThreadStackElement>(_w, ptr);
        }

        public Types.LogIteratorThreadStackElement? GetCurrentThreadStackElement(int stackLevel)
        {
            int ptr = _w.Invoke(_w.F_MpGetLogIteratorCurrentThreadStackElement, _iterPtr, stackLevel);
            return LibMpHelpers.WrapStruct<Types.LogIteratorThreadStackElement>(_w, ptr);
        }

        public uint GetCurrentEntryCount()
        {
            return (uint)_w.Invoke(_w.F_MpGetLogIteratorCurrentEntryCount, _iterPtr);
        }

        public uint GetCurrentEntryPart(uint entryIndex, uint partIndex)
        {
            return (uint)_w.Invoke(_w.F_MpGetLogIteratorCurrentEntryPart,
                _iterPtr, (int)entryIndex, (int)partIndex);
        }

        private ulong? ReadLabelOffset(int ptr)
        {
            if (ptr == 0) return null;
            ulong value = BinaryPrimitives.ReadUInt64LittleEndian(_w.GetMemorySpan(ptr, 8));
            return value == 0 ? null : value;
        }

        private ulong? GetCurrentLabelOffset()
        {
            return ReadLabelOffset(_w.Invoke(
                _w.F_MpGetLogIteratorCurrentLabelOffset, _iterPtr));
        }

        public string? FetchCurrentLabel()
        {
            ulong? offset = GetCurrentLabelOffset();
            return offset.HasValue ? _session.FetchLabelFromOffsetInternal(offset.Value) : null;
        }

        public uint GetScopeLabelCount(uint threadId, int stackLevel)
        {
            return (uint)_w.Invoke(_w.F_MpGetLogIteratorScopeLabelCount,
                _iterPtr, (int)threadId, stackLevel);
        }

        public uint GetCurrentScopeLabelCount()
        {
            return (uint)_w.Invoke(
                _w.F_MpGetLogIteratorCurrentScopeLabelCount, _iterPtr);
        }

        private ulong? GetScopeLabelOffset(uint threadId, int stackLevel, int labelIndex)
        {
            return ReadLabelOffset(_w.Invoke(
                _w.F_MpGetLogIteratorScopeLabelOffset,
                _iterPtr, (int)threadId, stackLevel, labelIndex));
        }

        private ulong? GetCurrentScopeLabelOffset(int labelIndex)
        {
            return ReadLabelOffset(_w.Invoke(
                _w.F_MpGetLogIteratorCurrentScopeLabelOffset, _iterPtr, labelIndex));
        }

        public string? FetchScopeLabel(uint threadId, int stackLevel, int labelIndex)
        {
            ulong? offset = GetScopeLabelOffset(threadId, stackLevel, labelIndex);
            return offset.HasValue ? _session.FetchLabelFromOffsetInternal(offset.Value) : null;
        }

        public string? FetchCurrentScopeLabel(int labelIndex)
        {
            ulong? offset = GetCurrentScopeLabelOffset(labelIndex);
            return offset.HasValue ? _session.FetchLabelFromOffsetInternal(offset.Value) : null;
        }

    }


    // =========================================================================
    // COUNTER ITERATOR CLASS
    // =========================================================================

    public class CounterIterator : IDisposable
    {
        private WasmModule _w;
        private Session _session; // keeps the session alive while the iterator is reachable
        private int _iterPtr;

        internal CounterIterator(WasmModule w, Session session)
        {
            _w = w;
            _session = session;
            _iterPtr = w.Invoke(w.F_MpCreateCounterIterator, session.Pointer);
        }

        public void Dispose()
        {
            if (_iterPtr != 0)
            {
                _w.Invoke(_w.F_MpDeleteCounterIterator, _iterPtr);
                _iterPtr = 0;
            }
            GC.SuppressFinalize(this);
        }

        ~CounterIterator()
        {
            if (_iterPtr != 0)
                _w.QueueRelease(_w.F_MpDeleteCounterIterator, _iterPtr);
        }

        public bool Configure(Types.CounterIteratorConfigT config)
        {
            var builder = new FlatBufferBuilder(1024);
            var rootOffset = Types.CounterIteratorConfig.Pack(builder, config);
            builder.Finish(rootOffset.Value);

            var buf = builder.SizedByteArray();
            int bufLen = buf.Length;

            int destPtr = _w.Invoke(_w.F_MpGetCounterIteratorConfigBuffer, _iterPtr, bufLen);
            if (destPtr == 0) return false;
            _w.WriteToMemory(destPtr, buf);

            return _w.Invoke(_w.F_MpConfigureCounterIterator, _iterPtr, 0) != 0;
        }

        public bool Step()
        {
            return _w.Invoke(_w.F_MpStepCounterIterator, _iterPtr) != 0;
        }

        public void Rewind()
        {
            _w.Invoke(_w.F_MpRewindCounterIterator, _iterPtr);
        }

        public Types.CounterIteratorState? GetState()
        {
            int ptr = _w.Invoke(_w.F_MpGetCounterIteratorState, _iterPtr);
            return LibMpHelpers.WrapStruct<Types.CounterIteratorState>(_w, ptr);
        }

        public uint GetStackElement(int stackLevel)
        {
            return (uint)_w.Invoke(_w.F_MpGetCounterIteratorStackElement, _iterPtr, stackLevel);
        }

    }


    // =========================================================================
    // CONTROL CLASS
    // =========================================================================

    public class Control
    {
        private WasmModule _w;
        private int _ptr;

        internal Control(WasmModule w, int ptr)
        {
            _w = w;
            _ptr = ptr;
        }

        // Called when the session closes: its commands must not flag the freed session for resync
        internal void Detach() => _ptr = 0;

        public bool EnableProfiler(bool enable)
        {
            return _w.Invoke(_w.F_MpControlEnableProfiler, _ptr, enable ? 1 : 0) != 0;
        }

        public bool EnableCapture(bool enable)
        {
            return _w.Invoke(_w.F_MpControlEnableCapture, _ptr, enable ? 1 : 0) != 0;
        }

        public bool ShowUI(bool show)
        {
            return _w.Invoke(_w.F_MpControlShowUI, _ptr, show ? 1 : 0) != 0;
        }

        public bool SetFrameLimit(int num)
        {
            return _w.Invoke(_w.F_MpControlSetFrameLimit, _ptr, num) != 0;
        }

        public bool SetMemTargetMB(int targetMB)
        {
            return _w.Invoke(_w.F_MpControlSetMemTargetMB, _ptr, targetMB) != 0;
        }

        public byte[]? CaptureToBufferSync(CaptureParams? captureParams = null) => null;
        public bool CaptureToSlotSync(uint targetSlotId, CaptureParams? captureParams = null) => false;
        public byte[]? SaveSlotToBuffer(uint slotId) => null;
        public bool ResetSlot(uint slotId) => false;

        public bool UseControlChannel(bool use)
        {
            return _w.Invoke(_w.F_MpUseControlChannel, use ? 1 : 0) != 0;
        }

        public bool IsBackendVersionCompatible()
        {
            return _w.Invoke(_w.F_MpIsBackendVersionCompatible) != 0;
        }

        public bool IsBackendAccessible()
        {
            return _w.Invoke(_w.F_MpIsBackendAccessible) != 0;
        }

        public bool IsBackendReady()
        {
            return _w.Invoke(_w.F_MpIsBackendReady) != 0;
        }
    }

    // =========================================================================
    // SESSION CLASS
    // =========================================================================

    public class Session : IDisposable
    {
        private WasmModule _w;
        private int _ptr;
        private int _sessionId;
        private bool _objectCacheEnabled;
        private int _dataLockDepth;
        private bool _needCheckSyncLocalCache;
        private bool _labelCacheEnabled;
        private bool _labelCacheDirty;
        private readonly ulong _labelCacheChunkSize;
        private readonly Dictionary<ulong, Dictionary<ulong, string>> _labelCache = new();
        private int? _ownedMemPtr;

        public Control Control { get; private set; }
        internal WasmModule Runtime => _w;
        internal int Pointer => _ptr;

        // === CACHE LAYER (buckets match C API MpSession::CacheBucket) ===
        // CacheBucket indices match C API MpSession::CacheBucket
        private const int BucketShortTerm = 0;
        private const int BucketLongTerm = 1;
        private const uint NotifyLabelRegionsChanged = 1u << 0;
        private const uint NotifyLabelCacheReset = 1u << 1;

        private class CacheEntry<TView, TObj>
        {
            public Dictionary<uint, TView> Wraps = new();
            public Dictionary<uint, TObj> Objs = new();
            public void Clear() { Wraps.Clear(); Objs.Clear(); }
        }

        // ShortTerm cache (cleared more often)
        private CacheEntry<Types.GlobalDesc, Types.GlobalDescT> _cacheGlobalDesc = new();
        private CacheEntry<Types.GeneralInfo, Types.GeneralInfoT> _cacheGeneralInfo = new();

        // LongTerm cache
        private CacheEntry<Types.GroupDesc, Types.GroupDescT> _cacheGroupDesc = new();
        private CacheEntry<Types.TimerDesc, Types.TimerDescT> _cacheTimerDesc = new();
        private CacheEntry<Types.CounterDesc, Types.CounterDescT> _cacheCounterDesc = new();
        private CacheEntry<Types.ThreadDesc, Types.ThreadDescT> _cacheThreadDesc = new();

        private Session(WasmModule w, int ptr, int? id = null)
        {
            _w = w;
            _ptr = ptr;

            if (id == null)
                id = w.AllocateSessionId();

            _sessionId = id.Value;
            _w.SessionsByPtr[_ptr] = new WeakReference<Session>(this);
            _labelCacheChunkSize = (uint)_w.Invoke(
                _w.F_MpInternalGetLabelCacheChunkSize);

            Control = new Control(w, ptr);
            EnableObjectCache(true);
        }

        // -- Static factory methods --

        public static Session? OpenFromMem(WasmModule w, byte[] data)
        {
            int len = data.Length;
            int ptr = w.Invoke(w.F_malloc, len);
            w.WriteToMemory(ptr, data);

            int sessionPtr = w.Invoke(w.F_MpOpenFromMemTrusted, ptr, len);
            if (sessionPtr != 0)
            {
                var session = new Session(w, sessionPtr);
                session._ownedMemPtr = ptr;
                return session;
            }

            w.Invoke(w.F_free, ptr);
            return null;
        }

        public static Session? OpenFromExternalProvider(
            WasmModule w, IDataProvider io, int cacheSize = 0, bool useBlockFetch = false)
        {
            if (cacheSize < 0) throw new ArgumentOutOfRangeException(nameof(cacheSize));
            int handle = w.AllocateSessionId();

            // Register IO provider so callbacks can find it during MpOpenFromExternalData
            w.IoRegistry[handle] = io;

            int sessionPtr = w.Invoke(
                w.F_MpOpenFromExternalDataTrusted, handle, useBlockFetch ? 1 : 0, cacheSize);
            if (sessionPtr == 0)
            {
                w.IoRegistry.Remove(handle);
                return null;
            }

            var session = new Session(w, sessionPtr, handle);
            return session;
        }

        public static Session? OpenFromBuffer(WasmModule w, byte[] data, int cacheSize = 0)
        {
            var io = new BufferDataProvider(data);
            return OpenFromExternalProvider(w, io, cacheSize);
        }

        public static Session? OpenFromFile(WasmModule w, string path, int cacheSize = 0)
        {
            var io = new FileDataProvider(path);
            return OpenFromExternalProvider(w, io, cacheSize);
        }

        // -- Instance methods --

        public void Dispose()
        {
            if (_ptr != 0)
            {
                Release(_w, _ptr, _sessionId, _ownedMemPtr, Control);
                _ownedMemPtr = null;
                _ptr = 0;
                _labelCache.Clear();
            }
            GC.SuppressFinalize(this);
        }

        ~Session()
        {
            if (_ptr != 0)
            {
                var (w, ptr, sessionId, ownedMemPtr, control) = (_w, _ptr, _sessionId, _ownedMemPtr, Control);
                w.QueueRelease(() => Release(w, ptr, sessionId, ownedMemPtr, control));
            }
        }

        // Static so that the finalizer's queued release does not keep the session alive
        private static void Release(WasmModule w, int ptr, int sessionId, int? ownedMemPtr, Control? control)
        {
            w.Invoke(w.F_MpCloseSession, ptr);
            w.SessionsByPtr.Remove(ptr);
            w.IoRegistry.Remove(sessionId);
            if (ownedMemPtr.HasValue)
                w.Invoke(w.F_free, ownedMemPtr.Value);
            control?.Detach();
        }

        public bool SyncWithDataSource()
        {
            return _w.Invoke(_w.F_MpSyncWithDataSource, _ptr) != 0;
        }

        public void SetSyncRequired()
        {
            _w.Invoke(_w.F_MpSetSyncRequired, _ptr);
            _needCheckSyncLocalCache = true;
        }

        public void SetClearCacheOnSyncRequired()
        {
            _w.Invoke(_w.F_MpSetClearCacheOnSyncRequired, _ptr);
        }

        public void SyncIfRequired()
        {
            if (_needCheckSyncLocalCache)
            {
                _w.Invoke(_w.F_MpSyncIfRequired, _ptr);
                _needCheckSyncLocalCache = false;
            }
        }

        public bool IsValid()
        {
            return _w.Invoke(_w.F_MpIsSessionValid, _ptr) != 0;
        }

        public uint GetDataFormatVersion()
        {
            return (uint)_w.Invoke(_w.F_MpGetSessionDataFormatVersion, _ptr);
        }

        public uint GetTrackCount()
        {
            return (uint)_w.Invoke(_w.F_MpGetTrackCount, _ptr);
        }

        public StructArray<Types.TrackDesc, Types.TrackDescT> GetTracks()
        {
            int ptr = _w.Invoke(_w.F_MpGetTracks, _ptr);
            return new StructArray<Types.TrackDesc, Types.TrackDescT>(
                this, ptr, _w.F_MpGetTrackDescFromArray, value => value.UnPack());
        }

        public Types.TrackDescT[] FetchTracks()
        {
            using var tracks = GetTracks();
            return tracks.FetchAll();
        }

        public uint GetSegmentCount(uint trackIndex)
        {
            return (uint)_w.Invoke(_w.F_MpGetSegmentCount, _ptr, (int)trackIndex);
        }

        public StructArray<Types.SegmentDesc, Types.SegmentDescT> GetSegments(uint trackIndex)
        {
            int ptr = _w.Invoke(_w.F_MpGetSegments, _ptr, (int)trackIndex);
            return new StructArray<Types.SegmentDesc, Types.SegmentDescT>(
                this, ptr, _w.F_MpGetSegmentDescFromArray, value => value.UnPack());
        }

        public Types.SegmentDescT[] FetchSegments(uint trackIndex)
        {
            using var segments = GetSegments(trackIndex);
            return segments.FetchAll();
        }

        public bool SelectTrack(uint trackIndex)
        {
            return _w.Invoke(_w.F_MpSelectTrack, _ptr, (int)trackIndex) != 0;
        }

        public bool SelectSegment(uint segmentIndex)
        {
            return _w.Invoke(_w.F_MpSelectSegment, _ptr, (int)segmentIndex) != 0;
        }

        public uint GetObjSize()
        {
            return (uint)_w.Invoke(_w.F_MpGetObjSize, _ptr);
        }

        public bool WithDataLock(Action callback)
        {
            bool isLocked = _w.Invoke(_w.F_MpPushDataLock, _ptr) != 0;
            if (!isLocked) return false;

            _dataLockDepth++;
            try
            {
                callback();
            }
            finally
            {
                _dataLockDepth--;
                _w.Invoke(_w.F_MpPopDataLock, _ptr);
            }
            return true;
        }

        public void EnableObjectCache(bool enabled)
        {
            _objectCacheEnabled = enabled;
            int e = enabled ? 1 : 0;
            _w.Invoke(_w.F_MpEnableObjectCache, _ptr, e, e, e, e);
            if (!enabled) ClearLocalCache();
        }

        public void ClearObjectCache()
        {
            _w.Invoke(_w.F_MpClearObjectCache, _ptr);
            if (_objectCacheEnabled) ClearLocalCache();
        }

        internal void HandleSessionNotification(uint flags)
        {
            if ((flags & NotifyLabelCacheReset) != 0)
                InvalidateLabelCache(true);
            else if ((flags & NotifyLabelRegionsChanged) != 0)
                InvalidateLabelCache(false);
        }

        private void InvalidateLabelCache(bool clearAll)
        {
            _labelCacheDirty = true;
            if (clearAll)
                _labelCache.Clear();
        }

        private void RefreshLabelCache()
        {
            if (!_labelCacheEnabled || !_labelCacheDirty)
                return;

            int ptr = _w.Invoke(_w.F_MpInternalGetLabelBufferRegions, _ptr);
            using var regions = new StructArray<Types.LabelBufferRegion, Types.LabelBufferRegionT>(
                this, ptr, _w.F_MpInternalGetLabelBufferRegionFromArray,
                value => value.UnPack());
            var removed = new List<ulong>();
            foreach (ulong chunkId in _labelCache.Keys)
            {
                ulong chunkStart = chunkId * _labelCacheChunkSize;
                ulong chunkEnd = chunkStart + _labelCacheChunkSize;
                bool covered = false;
                for (int i = 0; i < regions.Size(); ++i)
                {
                    var region = regions.Get(i);
                    if (region.HasValue && region.Value.StartOffset <= chunkStart &&
                        region.Value.EndOffset >= chunkEnd)
                    {
                        covered = true;
                        break;
                    }
                }
                if (!covered)
                    removed.Add(chunkId);
            }
            foreach (ulong chunkId in removed)
                _labelCache.Remove(chunkId);
            _labelCacheDirty = false;
        }

        private string? FetchLabel(ulong offset, Func<int, int> call)
        {
            SyncIfRequired();
            RefreshLabelCache();
            ulong chunkId = offset / _labelCacheChunkSize;
            if (_labelCacheEnabled && _labelCache.TryGetValue(chunkId, out var cachedChunk) &&
                cachedChunk.TryGetValue(offset, out var cached))
                return cached;

            int stackStart = _w.Invoke(_w.F_emscripten_stack_get_current);
            int lengthPtr = _w.Invoke(_w.F__emscripten_stack_alloc, 4);
            string? value;
            try
            {
                BinaryPrimitives.WriteUInt32LittleEndian(_w.GetMemorySpan(lengthPtr, 4), 0);
                int ptr = call(lengthPtr);
                if (ptr == 0)
                    return null;
                uint length = BinaryPrimitives.ReadUInt32LittleEndian(
                    _w.GetMemorySpan(lengthPtr, 4));
                value = Encoding.UTF8.GetString(_w.GetMemorySpan(ptr, checked((int)length)));
            }
            finally
            {
                _w.Invoke(_w.F__emscripten_stack_restore, stackStart);
            }

            if (_labelCacheEnabled)
            {
                if (!_labelCache.TryGetValue(chunkId, out var chunk))
                {
                    chunk = new Dictionary<ulong, string>();
                    _labelCache[chunkId] = chunk;
                }
                chunk[offset] = value;
            }
            return value;
        }

        public void EnableLabelCache(bool enabled)
        {
            _labelCacheEnabled = enabled;
            if (!enabled)
                _labelCache.Clear();
        }

        public void ClearLabelCache()
        {
            _labelCache.Clear();
            _labelCacheDirty = false;
        }

        internal string? FetchLabelFromOffsetInternal(ulong offset)
        {
            if (offset == 0)
                return null;
            return FetchLabel(offset, lengthPtr => _w.Invoke(
                _w.F_MpGetLabelFromOffsetParts, _ptr,
                unchecked((int)(uint)offset), unchecked((int)(uint)(offset >> 32)),
                lengthPtr));
        }

        internal void ClearCacheBucket(int bucketIdx)
        {
            if (bucketIdx == BucketShortTerm)
            {
                _cacheGlobalDesc.Clear();
                _cacheGeneralInfo.Clear();
            }
            else if (bucketIdx == BucketLongTerm)
            {
                _cacheGroupDesc.Clear();
                _cacheTimerDesc.Clear();
                _cacheCounterDesc.Clear();
                _cacheThreadDesc.Clear();
            }
        }

        private void ClearLocalCache()
        {
            ClearCacheBucket(BucketShortTerm);
            ClearCacheBucket(BucketLongTerm);
        }

        // =============================================================================
        // GENERIC CACHING HELPERS
        // =============================================================================

        private TView? GetCached<TView, TObj>(CacheEntry<TView, TObj> cache, uint id,
            Func<int> getPtr, Func<int, TView?> wrap) where TView : struct where TObj : class
        {
            SyncIfRequired();
            if (_objectCacheEnabled && cache.Wraps.TryGetValue(id, out var cached))
            {
                if (_dataLockDepth > 0)
                    _w.Invoke(_w.F_MpConsumeDataLock, _ptr);
                return cached;
            }
            int ptr = getPtr();
            var obj = wrap(ptr);
            if (obj.HasValue && _objectCacheEnabled)
                cache.Wraps[id] = obj.Value;
            return obj;
        }

        private TObj? FetchCached<TView, TObj>(CacheEntry<TView, TObj> cache, uint id,
            Func<TView?> getMethod, Func<TView, TObj> unpack)
            where TView : struct where TObj : class
        {
            SyncIfRequired();
            if (_objectCacheEnabled && cache.Objs.TryGetValue(id, out var cached))
            {
                if (_dataLockDepth > 0)
                    _w.Invoke(_w.F_MpConsumeDataLock, _ptr);
                return cached;
            }
            var view = getMethod();
            if (view == null) return null;
            var obj = unpack(view.Value);
            if (_objectCacheEnabled)
                cache.Objs[id] = obj;
            return obj;
        }

        // =============================================================================
        // CORE GETTERS
        // =============================================================================

        public Types.GlobalDesc? GetGlobalDesc()
            => GetCached(_cacheGlobalDesc, 0, () => _w.Invoke(_w.F_MpGetGlobalDesc, _ptr), p => LibMpHelpers.WrapStruct<Types.GlobalDesc>(_w, p));
        public Types.GlobalDescT? FetchGlobalDesc()
            => FetchCached(_cacheGlobalDesc, 0, () => GetGlobalDesc(), v => v.UnPack());

        public Types.GeneralInfo? GetGeneralInfo()
            => GetCached(_cacheGeneralInfo, 0, () => _w.Invoke(_w.F_MpGetGeneralInfo, _ptr), p => LibMpHelpers.WrapTable<Types.GeneralInfo>(_w, p));
        public Types.GeneralInfoT? FetchGeneralInfo()
            => FetchCached(_cacheGeneralInfo, 0, () => GetGeneralInfo(), v => v.UnPack());

        public uint GetGroupIdMax() => (uint)_w.Invoke(_w.F_MpGetGroupIdMax, _ptr);

        public Types.GroupDesc? GetGroupDesc(uint groupId)
            => GetCached(_cacheGroupDesc, groupId, () => _w.Invoke(_w.F_MpGetGroupDesc, _ptr, (int)groupId), p => LibMpHelpers.WrapTable<Types.GroupDesc>(_w, p));
        public Types.GroupDescT? FetchGroupDesc(uint groupId)
            => FetchCached(_cacheGroupDesc, groupId, () => GetGroupDesc(groupId), v => v.UnPack());

        public ScalarArray GetTimerIds() => new ScalarArray(this, _w.Invoke(_w.F_MpGetTimerIds, _ptr), _w.F_MpGetIdFromArray);
        public uint[] FetchTimerIds() { using var arr = GetTimerIds(); return arr.FetchAll(); }

        public Types.TimerDesc? GetTimerDesc(uint timerId)
            => GetCached(_cacheTimerDesc, timerId, () => _w.Invoke(_w.F_MpGetTimerDesc, _ptr, (int)timerId), p => LibMpHelpers.WrapTable<Types.TimerDesc>(_w, p));
        public Types.TimerDescT? FetchTimerDesc(uint timerId)
            => FetchCached(_cacheTimerDesc, timerId, () => GetTimerDesc(timerId), v => v.UnPack());

        public ScalarArray GetCounterIds() => new ScalarArray(this, _w.Invoke(_w.F_MpGetCounterIds, _ptr), _w.F_MpGetIdFromArray);
        public uint[] FetchCounterIds() { using var arr = GetCounterIds(); return arr.FetchAll(); }

        public Types.CounterDesc? GetCounterDesc(uint counterId)
            => GetCached(_cacheCounterDesc, counterId, () => _w.Invoke(_w.F_MpGetCounterDesc, _ptr, (int)counterId), p => LibMpHelpers.WrapTable<Types.CounterDesc>(_w, p));
        public Types.CounterDescT? FetchCounterDesc(uint counterId)
            => FetchCached(_cacheCounterDesc, counterId, () => GetCounterDesc(counterId), v => v.UnPack());

        public CounterSampleArray GetCounterSamples(uint counterId) => new CounterSampleArray(this, _w.Invoke(_w.F_MpGetCounterSamples, _ptr, (int)counterId));
        public Types.CounterSampleT[] FetchCounterSamples(uint counterId) { using var arr = GetCounterSamples(counterId); return arr.FetchAll(); }

        public Types.CounterSample? GetLastCounterSample(uint counterId)
        {
            int ptr = _w.Invoke(_w.F_MpGetLastCounterSample, _ptr, (int)counterId);
            return LibMpHelpers.WrapStruct<Types.CounterSample>(_w, ptr);
        }
        public Types.CounterSampleT? FetchLastCounterSample(uint counterId) => GetLastCounterSample(counterId)?.UnPack();

        public Types.ThreadDesc? GetThreadDesc(uint threadId)
            => GetCached(_cacheThreadDesc, threadId, () => _w.Invoke(_w.F_MpGetThreadDesc, _ptr, (int)threadId), p => LibMpHelpers.WrapTable<Types.ThreadDesc>(_w, p));
        public Types.ThreadDescT? FetchThreadDesc(uint threadId)
            => FetchCached(_cacheThreadDesc, threadId, () => GetThreadDesc(threadId), v => v.UnPack());

        public uint GetThreadIdGpu() => (uint)_w.Invoke(_w.F_MpGetThreadIdGpu, _ptr);

        public ScalarArray GetThreadIds() => new ScalarArray(this, _w.Invoke(_w.F_MpGetThreadIds, _ptr), _w.F_MpGetIdFromArray);
        public uint[] FetchThreadIds() { using var arr = GetThreadIds(); return arr.FetchAll(); }

        public uint GetFrameIdMin() => (uint)_w.Invoke(_w.F_MpGetFrameIdMin, _ptr);
        public uint GetFrameIdMax() => (uint)_w.Invoke(_w.F_MpGetFrameIdMax, _ptr);

        public Types.FrameDesc? GetFrameDesc(uint frameId)
        {
            SyncIfRequired();
            int ptr = _w.Invoke(_w.F_MpGetFrameDesc, _ptr, (int)frameId);
            return LibMpHelpers.WrapTable<Types.FrameDesc>(_w, ptr);
        }
        public Types.FrameDescT? FetchFrameDesc(uint frameId) => GetFrameDesc(frameId)?.UnPack();

        public StructArray<Types.U64Sample, Types.U64SampleT> GetPlaceIdSamples()
        {
            int arrPtr = _w.Invoke(_w.F_MpGetPlaceIdSamples, _ptr);
            return new StructArray<Types.U64Sample, Types.U64SampleT>(
                this, arrPtr, _w.F_MpGetU64SampleFromArray, v => v.UnPack());
        }

        public Types.U64SampleT[] FetchPlaceIdSamples()
        {
            using var arr = GetPlaceIdSamples();
            return arr.FetchAll();
        }

        public StructArray<Types.U64Sample, Types.U64SampleT> GetUtcTimestampSamples()
        {
            int arrPtr = _w.Invoke(_w.F_MpGetUtcTimestampSamples, _ptr);
            return new StructArray<Types.U64Sample, Types.U64SampleT>(
                this, arrPtr, _w.F_MpGetU64SampleFromArray, v => v.UnPack());
        }

        public Types.U64SampleT[] FetchUtcTimestampSamples()
        {
            using var arr = GetUtcTimestampSamples();
            return arr.FetchAll();
        }

        // -- Iterators --

        public LogIterator CreateLogIterator()
        {
            return new LogIterator(_w, this);
        }

        public CounterIterator CreateCounterIterator()
        {
            return new CounterIterator(_w, this);
        }

        // -- Find/Filter Methods --

        private int CallWithMask(Function func, string? nameMask, bool caseSensitive)
        {
            nameMask ??= "";
            var bytes = Encoding.UTF8.GetBytes(nameMask);
            int len = bytes.Length;

            int stackStart = _w.Invoke(_w.F_emscripten_stack_get_current);
            int strPtr = _w.Invoke(_w.F__emscripten_stack_alloc, len + 1);

            _w.WriteToMemory(strPtr, bytes);
            _w.GetMemorySpan(strPtr + len, 1)[0] = 0;

            int cs = caseSensitive ? 1 : 0;
            int result = _w.Invoke(func, _ptr, strPtr, cs);

            _w.Invoke(_w.F__emscripten_stack_restore, stackStart);
            return result;
        }

        public uint FindGroupId(string? nameMask = null, bool caseSensitive = false)
        {
            return (uint)CallWithMask(_w.F_MpFindGroupId, nameMask, caseSensitive);
        }

        public uint[] FindGroupIds(string? nameMask = null, bool caseSensitive = false)
        {
            int arrPtr = CallWithMask(_w.F_MpFindGroupIds, nameMask, caseSensitive);
            using var arr = new ScalarArray(this, arrPtr, _w.F_MpGetIdFromArray);
            return arr.FetchAll();
        }

        public uint FindTimerId(string? nameMask = null, bool caseSensitive = false)
        {
            return (uint)CallWithMask(_w.F_MpFindTimerId, nameMask, caseSensitive);
        }

        public uint[] FindTimerIds(string? nameMask = null, bool caseSensitive = false)
        {
            int arrPtr = CallWithMask(_w.F_MpFindTimerIds, nameMask, caseSensitive);
            using var arr = new ScalarArray(this, arrPtr, _w.F_MpGetIdFromArray);
            return arr.FetchAll();
        }

        public ScalarArray GetTimerIdsByGroupId(uint groupId)
        {
            int arrPtr = _w.Invoke(_w.F_MpGetTimerIdsByGroupId, _ptr, (int)groupId);
            return new ScalarArray(this, arrPtr, _w.F_MpGetIdFromArray);
        }

        public uint[] FetchTimerIdsByGroupId(uint groupId)
        {
            using var arr = GetTimerIdsByGroupId(groupId);
            return arr.FetchAll();
        }

        public ScalarArray GetTimerIdsByGroupNames(string? nameMask = null, bool caseSensitive = false)
        {
            int arrPtr = CallWithMask(_w.F_MpGetTimerIdsByGroupNames, nameMask, caseSensitive);
            return new ScalarArray(this, arrPtr, _w.F_MpGetIdFromArray);
        }

        public uint[] FetchTimerIdsByGroupNames(string? nameMask = null, bool caseSensitive = false)
        {
            using var arr = GetTimerIdsByGroupNames(nameMask, caseSensitive);
            return arr.FetchAll();
        }

        public uint FindCounterId(string? nameMask = null, bool caseSensitive = false)
        {
            return (uint)CallWithMask(_w.F_MpFindCounterId, nameMask, caseSensitive);
        }

        public uint[] FindCounterIds(string? nameMask = null, bool caseSensitive = false)
        {
            int arrPtr = CallWithMask(_w.F_MpFindCounterIds, nameMask, caseSensitive);
            using var arr = new ScalarArray(this, arrPtr, _w.F_MpGetIdFromArray);
            return arr.FetchAll();
        }

        public uint FindThreadId(string? nameMask = null, bool caseSensitive = false)
        {
            return (uint)CallWithMask(_w.F_MpFindThreadId, nameMask, caseSensitive);
        }

        public uint[] FindThreadIds(string? nameMask = null, bool caseSensitive = false)
        {
            int arrPtr = CallWithMask(_w.F_MpFindThreadIds, nameMask, caseSensitive);
            using var arr = new ScalarArray(this, arrPtr, _w.F_MpGetIdFromArray);
            return arr.FetchAll();
        }

    }

    // =========================================================================
    // DATA PROVIDERS
    // =========================================================================

    internal class BufferDataProvider : IDataProvider
    {
        private byte[]? _data;

        public BufferDataProvider(byte[] data) { _data = data; }

        public bool Open() => true;
        public bool Close() { _data = null; return true; }
        public ulong GetSize() => (ulong)(_data?.LongLength ?? 0);

        public int Read(Span<byte> dest, uint blockId, ulong offset, int size)
        {
            if (_data == null || blockId != 0 || offset > (ulong)_data.Length ||
                size < 0 || (ulong)size > (ulong)_data.Length - offset)
                return 0;
            _data.AsSpan((int)offset, size).CopyTo(dest);
            return size;
        }
    }

    internal class FileDataProvider : IDataProvider
    {
        private string _path;
        private FileStream? _stream;

        public FileDataProvider(string path) { _path = path; }

        public bool Open()
        {
            _stream = new FileStream(_path, FileMode.Open, FileAccess.Read, FileShare.Read);
            return true;
        }
        public bool Close() { _stream?.Dispose(); _stream = null; return true; }
        public ulong GetSize() => _stream != null ? (ulong)_stream.Length : 0;

        public int Read(Span<byte> dest, uint blockId, ulong offset, int size)
        {
            if (_stream == null || blockId != 0 || offset > long.MaxValue || size < 0)
                return 0;
            _stream.Seek((long)offset, SeekOrigin.Begin);
            return _stream.Read(dest.Slice(0, size));
        }
    }

    // =========================================================================
    // TOOLS
    // =========================================================================

    public sealed class Tools
    {
        private readonly WasmModule _w;

        internal Tools(WasmModule w) { _w = w; }

        public bool SetHtmlAssetsBaseUrl(string? baseUrl = null)
        {
            byte[] bytes = Encoding.UTF8.GetBytes(baseUrl ?? "");
            int stackStart = _w.Invoke(_w.F_emscripten_stack_get_current);
            int ptr = _w.Invoke(_w.F__emscripten_stack_alloc, bytes.Length + 1);
            try
            {
                _w.WriteToMemory(ptr, bytes);
                _w.GetMemorySpan(ptr + bytes.Length, 1)[0] = 0;
                return _w.Invoke(
                    _w.F_MpToolSetHtmlAssetsBaseUrl, ptr, bytes.Length) != 0;
            }
            finally
            {
                _w.Invoke(_w.F__emscripten_stack_restore, stackStart);
            }
        }

        private bool Run(
            Session session, Types.ContainerFormat format, ToolOutputHandler output)
        {
            if (session == null || session.Runtime != _w || session.Pointer == 0)
                throw new ArgumentException(
                    "Expected a live Session owned by this LibMP instance", nameof(session));
            if (_w.ExtToolOutput != null)
                throw new InvalidOperationException("A LibMP tool operation is already active");

            Exception? outputError = null;
            _w.ExtToolOutput = (dataPtr, size) =>
            {
                if (dataPtr < 0 || size < 0 || dataPtr > _w.GetMemoryLength() ||
                    size > _w.GetMemoryLength() - dataPtr)
                    return 0;
                try
                {
                    return output(_w.GetMemorySpan(dataPtr, size)) ? 1 : 0;
                }
                catch (Exception error)
                {
                    outputError = error;
                    return 0;
                }
            };

            bool success;
            try
            {
                success = _w.Invoke(
                    _w.F_MpToolConvertSession, session.Pointer, (int)format) != 0;
            }
            finally
            {
                _w.ExtToolOutput = null;
            }
            if (outputError != null)
                throw outputError;
            return success;
        }

        public byte[]? ConvertToBuffer(Session session, Types.ContainerFormat format)
        {
            using var output = new MemoryStream();
            bool success = Run(session, format, data =>
            {
                output.Write(data);
                return true;
            });
            return success ? output.ToArray() : null;
        }

        public bool ConvertToFile(
            Session session, Types.ContainerFormat format, string outputName)
        {
            using var output = new FileStream(
                outputName, FileMode.Create, FileAccess.Write, FileShare.None);
            return Run(session, format, data =>
            {
                output.Write(data);
                return true;
            });
        }
    }

    public sealed class ExperimentalV1
    {
        public bool StartFileStream(string filenamePostfix = "", uint targetSlotId = 0) => false;
        public bool StartMemoryStream(uint targetSlotId) => false;
        public bool StopStream() => false;
        public bool SetStreamMarker(uint marker, bool enabled) => false;
        public Types.ExperimentalStreamStatusT GetStreamStatus() => new();
        public bool SetContentMode(Types.ContentMode contentMode) => false;
    }

    public sealed class ExperimentalApi
    {
        public ExperimentalV1 V1 { get; } = new();
    }

    // =========================================================================
    // VERSIONS
    // =========================================================================

    public class Versions
    {
        public uint DataFormat { get; }
        public uint Library { get; }

        internal Versions(WasmModule w)
        {
            DataFormat = (uint)w.Invoke(w.F_MpGetDataFormatVersion);
            Library = (uint)w.Invoke(w.F_MpGetLibraryVersion);
        }
    }

    // =========================================================================
    // LIBMP (Top-level API)
    // =========================================================================

    public class LibMPInstance : IDisposable
    {
        private WasmModule _w;

        public Versions Versions { get; private set; } = null!;
        public Control Control { get; private set; } = null!;
        public Tools Tools { get; private set; } = null!;
        public ExperimentalApi Experimental { get; private set; } = new();

        public static LibMPInstance Create(ControlMessageHandler? controlMessage = null)
        {
            return new LibMPInstance(EmbeddedWasm.GetBytes(), controlMessage);
        }

        public LibMPInstance(byte[] wasmBytes, ControlMessageHandler? controlMessage = null)
        {
            _w = new WasmModule();

            // Wire up external IO callbacks
            _w.GprxExtDataOpen = (handle) =>
            {
                var io = _w.GetIo(handle);
                if (io == null) return 0;
                try { return io.Open() ? 1 : 0; }
                catch { return 0; }
            };

            _w.GprxExtDataClose = (handle) =>
            {
                var io = _w.GetIo(handle);
                if (io == null) return 0;
                try { return io.Close() ? 1 : 0; }
                catch { return 0; }
            };

            _w.GprxExtDataGetSize = (handle, sizeLowPtr, sizeHighPtr) =>
            {
                var io = _w.GetIo(handle);
                if (io == null) return 0;
                try
                {
                    ulong size = io.GetSize();
                    BinaryPrimitives.WriteUInt32LittleEndian(
                        _w.GetMemorySpan(sizeLowPtr, 4), (uint)size);
                    BinaryPrimitives.WriteUInt32LittleEndian(
                        _w.GetMemorySpan(sizeHighPtr, 4), (uint)(size >> 32));
                    return 1;
                }
                catch { return 0; }
            };

            _w.GprxExtDataRead = (handle, dst, blockId, offsetLow, offsetHigh, sz) =>
            {
                var io = _w.GetIo(handle);
                if (io == null || sz < 0) return 0;
                try
                {
                    ulong offset = (uint)offsetLow | ((ulong)(uint)offsetHigh << 32);
                    var destSpan = _w.GetMemorySpan(dst, sz);
                    int bytesRead = io.Read(destSpan, (uint)blockId, offset, sz);
                    return bytesRead >= 0 && bytesRead <= sz ? bytesRead : 0;
                }
                catch { return 0; }
            };

            _w.ExtClearObjCacheCallback = (handle, bucketIds, bucketCount) =>
            {
                var session = _w.GetSession(handle);
                if (session != null)
                {
                    for (int i = 0; i < bucketCount; i++)
                        session.ClearCacheBucket(bucketIds[i]);
                }
            };

            _w.ExtSessionNotificationCallback = (handle, notificationFlags) =>
            {
                _w.GetSession(handle)?.HandleSessionNotification(
                    unchecked((uint)notificationFlags));
            };

            if (controlMessage != null)
            {
                _w.ExtControlMsg = (requestPtr, requestSize, responsePtr, responseSize) =>
                {
                    if (requestSize < 0 || responseSize < 0)
                        return 0;
                    try
                    {
                        int result = controlMessage(
                            _w.GetMemorySpan(requestPtr, requestSize),
                            _w.GetMemorySpan(responsePtr, responseSize));
                        return result >= 0 && result <= responseSize ? result : 0;
                    }
                    catch
                    {
                        return 0;
                    }
                };
            }

            _w.Load(wasmBytes);
            _w.Invoke(_w.F_MpInit);

            Versions = new Versions(_w);
            Control = new Control(_w, 0);
            Control.UseControlChannel(controlMessage != null);
            Tools = new Tools(_w);
        }

        public uint GetMemUsed()
        {
            return (uint)_w.Invoke(_w.F_MpGetMemUsed);
        }

        public uint GetMemReserved()
        {
            return (uint)_w.Invoke(_w.F_MpGetMemReserved);
        }

        // Session factory methods
        public Session? OpenFromMem(byte[] data) => Session.OpenFromMem(_w, data);
        public Session? OpenFromBuffer(byte[] data, int cacheSize = 0) => Session.OpenFromBuffer(_w, data, cacheSize);
        public Session? OpenFromFile(string path, int cacheSize = 0) => Session.OpenFromFile(_w, path, cacheSize);
        public Session? OpenFromSlot(uint slotId, int cacheSize = 0) => null;
        public Session? OpenFromExternalProvider(
            IDataProvider io, int cacheSize = 0, bool useBlockFetch = false)
            => Session.OpenFromExternalProvider(_w, io, cacheSize, useBlockFetch);

        public void Dispose()
        {
            _w?.Dispose();
        }
    }
}
