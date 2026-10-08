// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

/*
 * LibMP for JavaScript
 * Access Roblox MicroProfiler data in browsers and Node.js.
 * %LIBMP_SHARED_METADATA%
 *
 * No external packages are required.
 *
 * Minimal usage:
 *   const libmp = await LibMP.create();
 *   const session = libmp.Session.OpenFromBuffer(captureArrayBuffer);
 *   session?.Dispose();
 */

import * as flatbuffers from "flatbuffers";
import * as MpTypes from "../../../build/js/mp-types.js";
import { LIBMP_WASM_BASE64 } from "../../../build/js/embedded_wasm.js";

const textDecoder = new TextDecoder();
const textEncoder = new TextEncoder();
const SHIFT_32 = 0x1_0000_0000n;
const MAX_U64 = 0xffff_ffff_ffff_ffffn;
const MAX_BUFFER_SIZE = 0x7fff_ffff;
const SINGLE_CACHE_KEY = Symbol("single");
const SESSION_NOTIFY_LABEL_REGIONS_CHANGED = 1 << 0;
const SESSION_NOTIFY_LABEL_CACHE_RESET = 1 << 1;
const Enums = Object.freeze({
    AppType: Object.freeze(MpTypes.AppType),
    ContainerFormat: Object.freeze({
        Raw: MpTypes.ContainerFormat.Raw,
        Compressed: MpTypes.ContainerFormat.Compressed,
        CompressedAscii: MpTypes.ContainerFormat.CompressedAscii,
        CompressedHtml: MpTypes.ContainerFormat.CompressedHtml,
    }),
    ContentMode: Object.freeze({
        Full: MpTypes.ContentMode.Full,
        CountersOnly: MpTypes.ContentMode.CountersOnly,
    }),
    DataSlot: Object.freeze({
        Live: MpTypes.DataSlot.Live,
        FirstUserManaged: MpTypes.DataSlot.FirstUserManaged,
        LastUserManaged: MpTypes.DataSlot.LastUserManaged,
        Temporary: MpTypes.DataSlot.Temporary,
    }),
});

function asBytes(value) {
    if (value instanceof Uint8Array)
        return value;
    if (value instanceof ArrayBuffer)
        return new Uint8Array(value);
    if (ArrayBuffer.isView(value))
        return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
    throw new TypeError("Expected an ArrayBuffer or typed-array view");
}

function decodeBase64(value) {
    if (typeof Buffer !== "undefined")
        return new Uint8Array(Buffer.from(value, "base64"));

    const binary = globalThis.atob(value);
    const result = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; ++i)
        result[i] = binary.charCodeAt(i);
    return result;
}

function asU64(value, description) {
    let result;
    try {
        if (typeof value === "number" && !Number.isSafeInteger(value))
            throw new TypeError();
        result = typeof value === "bigint" ? value : BigInt(value);
    } catch {
        throw new TypeError(`${description} must be an integer number or bigint`);
    }
    if (result < 0n || result > MAX_U64)
        throw new RangeError(`${description} must fit in an unsigned 64-bit integer`);
    return result;
}

function lowerFirst(value) {
    return value.length ? value[0].toLowerCase() + value.slice(1) : value;
}

function makeConfig(Type, values) {
    if (values instanceof Type)
        return values;
    const result = new Type();
    for (const [name, value] of Object.entries(values || {})) {
        const key = name in result ? name : lowerFirst(name);
        if (!(key in result))
            throw new TypeError(`Unknown ${Type.name} field: ${name}`);
        result[key] = value;
    }
    return result;
}

class Control {
    constructor(runtime, sessionPtr = 0) {
        this._runtime = runtime;
        this._ptr = sessionPtr;
    }

    EnableProfiler(enable) {
        return this._runtime._exports.MpControlEnableProfiler(this._ptr, enable ? 1 : 0) !== 0;
    }

    EnableCapture(enable) {
        return this._runtime._exports.MpControlEnableCapture(this._ptr, enable ? 1 : 0) !== 0;
    }

    ShowUI(show) {
        return this._runtime._exports.MpControlShowUI(this._ptr, show ? 1 : 0) !== 0;
    }

    SetFrameLimit(count) {
        return this._runtime._exports.MpControlSetFrameLimit(this._ptr, count | 0) !== 0;
    }

    SetMemTargetMB(targetMB) {
        return this._runtime._exports.MpControlSetMemTargetMB(this._ptr, targetMB | 0) !== 0;
    }

    CaptureToBufferSync(_captureParams = undefined) { return null; }
    CaptureToSlotSync(_targetSlotId, _captureParams = undefined) { return false; }
    SaveSlotToBuffer(_slotId) { return null; }
    ResetSlot(_slotId) { return false; }

    UseControlChannel(use) {
        return this._runtime._exports.MpUseControlChannel(use ? 1 : 0) !== 0;
    }

    IsBackendVersionCompatible() {
        return this._runtime._exports.MpIsBackendVersionCompatible() !== 0;
    }

    IsBackendAccessible() {
        return this._runtime._exports.MpIsBackendAccessible() !== 0;
    }

    IsBackendReady() {
        return this._runtime._exports.MpIsBackendReady() !== 0;
    }
}

class Tools {
    constructor(runtime) {
        this._runtime = runtime;
    }

    SetHtmlAssetsBaseUrl(baseUrl = "") {
        const bytes = textEncoder.encode(String(baseUrl ?? ""));
        return this._runtime._withStack(bytes.byteLength + 1, (ptr) => {
            const memory = this._runtime._memoryBytes();
            memory.set(bytes, ptr);
            memory[ptr + bytes.byteLength] = 0;
            return this._runtime._exports.MpToolSetHtmlAssetsBaseUrl(
                ptr, bytes.byteLength) !== 0;
        });
    }

    ConvertToBuffer(session, format) {
        const chunks = [];
        let totalSize = 0;
        const success = this._runtime._runTool(session, format, (chunk) => {
            if (chunk.byteLength > MAX_BUFFER_SIZE - totalSize)
                throw new RangeError("Converted container exceeds the supported buffer size");
            chunks.push(chunk.slice());
            totalSize += chunk.byteLength;
            return true;
        });
        if (!success)
            return null;
        const result = new Uint8Array(totalSize);
        let offset = 0;
        for (const chunk of chunks) {
            result.set(chunk, offset);
            offset += chunk.byteLength;
        }
        return result;
    }

    async ConvertToFile(session, format, outputName) {
        if (typeof process !== "undefined" && process.versions?.node) {
            const fs = await import("node:fs");
            const descriptor = fs.openSync(outputName, "w");
            try {
                return this._runtime._runTool(session, format, (chunk) => {
                    let offset = 0;
                    while (offset < chunk.byteLength) {
                        const written = fs.writeSync(
                            descriptor, chunk, offset, chunk.byteLength - offset);
                        if (written <= 0)
                            return false;
                        offset += written;
                    }
                    return true;
                });
            } finally {
                fs.closeSync(descriptor);
            }
        }

        if (typeof document === "undefined" || typeof URL === "undefined" ||
            typeof URL.createObjectURL !== "function" || typeof Blob === "undefined")
            throw new Error("ConvertToFile requires Node.js or a browser document");
        const data = this.ConvertToBuffer(session, format);
        if (data === null)
            return false;
        const url = URL.createObjectURL(new Blob([data], { type: "application/octet-stream" }));
        try {
            const link = document.createElement("a");
            link.href = url;
            link.download = String(outputName);
            link.style.display = "none";
            document.body.appendChild(link);
            link.click();
            link.remove();
        } finally {
            URL.revokeObjectURL(url);
        }
        return true;
    }
}

const ExperimentalV1 = Object.freeze({
    StartFileStream(_filenamePostfix = "", _targetSlotId = 0) { return false; },
    StartMemoryStream(_targetSlotId) { return false; },
    StopStream() { return false; },
    SetStreamMarker(_marker, _enabled) { return false; },
    GetStreamStatus() { return new MpTypes.ExperimentalStreamStatusT(); },
    SetContentMode(_contentMode) { return false; },
});
const Experimental = Object.freeze({ V1: ExperimentalV1 });

class ArrayBase {
    constructor(session, arrayPtr) {
        this._runtime = session._runtime;
        // Element getters take the session; after its Dispose they get 0 and return null
        this._session = session;
        this._arrayPtr = arrayPtr;
        this._runtime._arrayFinalizer?.register(this, arrayPtr, this);
    }

    Dispose() {
        if (this._arrayPtr !== 0) {
            this._runtime._arrayFinalizer?.unregister(this);
            this._runtime._exports.MpReleaseArray(this._arrayPtr);
            this._arrayPtr = 0;
        }
    }

    Size() {
        return this._arrayPtr === 0 ? 0 : this._runtime._exports.MpGetArraySize(this._arrayPtr) >>> 0;
    }

    FetchAll() {
        const result = new Array(this.Size());
        for (let i = 0; i < result.length; ++i)
            result[i] = this.Fetch(i);
        return result;
    }
}

class ScalarArray extends ArrayBase {
    constructor(session, arrayPtr, getter) {
        super(session, arrayPtr);
        this._getter = getter;
    }

    Get(index) {
        return this._getter(this._session._ptr, this._arrayPtr, index) >>> 0;
    }

    Fetch(index) {
        return this.Get(index);
    }
}

class ObjectArray extends ArrayBase {
    constructor(session, arrayPtr, getter, Type, isStruct) {
        super(session, arrayPtr);
        this._getter = getter;
        this._Type = Type;
        this._isStruct = isStruct;
    }

    Get(index) {
        const ptr = this._getter(this._session._ptr, this._arrayPtr, index);
        return this._isStruct
            ? this._runtime._wrapStruct(ptr, this._Type)
            : this._runtime._wrapTable(ptr, this._Type);
    }

    Fetch(index) {
        return this.Get(index)?.unpack() ?? null;
    }
}

class CounterSampleArray extends ObjectArray {
    constructor(session, arrayPtr) {
        super(session, arrayPtr,
            session._runtime._exports.MpGetCounterSampleFromArray, MpTypes.CounterSample, true);
    }

    GetSampleByFrameId(frameId) {
        return this._runtime._wrapStruct(
            this._runtime._exports.MpGetCounterSampleFromArrayByFrameId(
                this._session._ptr, this._arrayPtr, frameId),
            MpTypes.CounterSample);
    }

    FetchSampleByFrameId(frameId) {
        return this.GetSampleByFrameId(frameId)?.unpack() ?? null;
    }

    GetLastSample() {
        return this._runtime._wrapStruct(
            this._runtime._exports.MpGetLastCounterSampleFromArray(
                this._session._ptr, this._arrayPtr),
            MpTypes.CounterSample);
    }

    FetchLastSample() {
        return this.GetLastSample()?.unpack() ?? null;
    }
}

function fetchLabelFromOffset(session, value) {
    const offset = asU64(value, "Label offset");
    if (offset === 0n)
        return null;
    return session._fetchLabel(offset, (lengthPtr) =>
        session._runtime._exports.MpGetLabelFromOffsetParts(
            session._ptr,
            Number(offset & 0xffff_ffffn), Number(offset >> 32n), lengthPtr));
}

class LogIterator {
    constructor(runtime, session) {
        this._runtime = runtime;
        this._session = session;
        this._ptr = runtime._exports.MpCreateLogIterator(session._ptr);
        runtime._logIteratorFinalizer?.register(this, this._ptr, this);
    }

    Dispose() {
        if (this._ptr !== 0) {
            this._runtime._logIteratorFinalizer?.unregister(this);
            this._runtime._exports.MpDeleteLogIterator(this._ptr);
            this._ptr = 0;
        }
    }

    Configure(values = {}) {
        const builder = new flatbuffers.Builder(1024);
        const config = makeConfig(MpTypes.LogIteratorConfigT, values);
        builder.finish(config.pack(builder));
        const data = builder.asUint8Array();
        const destination = this._runtime._exports.MpGetLogIteratorConfigBuffer(
            this._ptr, data.byteLength);
        if (destination === 0)
            return false;
        this._runtime._memoryBytes().set(data, destination);
        return this._runtime._exports.MpConfigureLogIterator(this._ptr, 0) !== 0;
    }

    Step() {
        return this._runtime._exports.MpStepLogIterator(this._ptr) !== 0;
    }

    Rewind() {
        this._runtime._exports.MpRewindLogIterator(this._ptr);
    }

    RewindTo(frameIdMin, frameIdMax) {
        this._runtime._exports.MpRewindLogIteratorTo(this._ptr, frameIdMin, frameIdMax);
    }

    GetState() {
        return this._runtime._wrapStruct(
            this._runtime._exports.MpGetLogIteratorState(this._ptr), MpTypes.LogIteratorState);
    }

    GetThreadStackState(threadId) {
        return this._runtime._wrapStruct(
            this._runtime._exports.MpGetLogIteratorThreadStackState(this._ptr, threadId),
            MpTypes.LogIteratorThreadStackState);
    }

    GetThreadStackElement(threadId, stackLevel) {
        return this._runtime._wrapStruct(
            this._runtime._exports.MpGetLogIteratorThreadStackElement(
                this._ptr, threadId, stackLevel),
            MpTypes.LogIteratorThreadStackElement);
    }

    GetCurrentThreadStackElement(stackLevel) {
        return this._runtime._wrapStruct(
            this._runtime._exports.MpGetLogIteratorCurrentThreadStackElement(
                this._ptr, stackLevel),
            MpTypes.LogIteratorThreadStackElement);
    }

    GetCurrentEntryCount() {
        return this._runtime._exports.MpGetLogIteratorCurrentEntryCount(this._ptr) >>> 0;
    }

    GetCurrentEntryPart(entryIndex, partIndex) {
        return this._runtime._exports.MpGetLogIteratorCurrentEntryPart(
            this._ptr, entryIndex, partIndex) >>> 0;
    }

    FetchCurrentLabel() {
        const offset = this._runtime._readU64Pointer(
            this._runtime._exports.MpGetLogIteratorCurrentLabelOffset(this._ptr));
        return offset === null ? null : fetchLabelFromOffset(this._session, offset);
    }

    GetScopeLabelCount(threadId, stackLevel) {
        return this._runtime._exports.MpGetLogIteratorScopeLabelCount(
            this._ptr, threadId, stackLevel) >>> 0;
    }

    GetCurrentScopeLabelCount() {
        return this._runtime._exports.MpGetLogIteratorCurrentScopeLabelCount(this._ptr) >>> 0;
    }

    FetchScopeLabel(threadId, stackLevel, labelIndex) {
        const offset = this._runtime._readU64Pointer(
            this._runtime._exports.MpGetLogIteratorScopeLabelOffset(
                this._ptr, threadId, stackLevel, labelIndex));
        return offset === null ? null : fetchLabelFromOffset(this._session, offset);
    }

    FetchCurrentScopeLabel(labelIndex) {
        const offset = this._runtime._readU64Pointer(
            this._runtime._exports.MpGetLogIteratorCurrentScopeLabelOffset(
                this._ptr, labelIndex));
        return offset === null ? null : fetchLabelFromOffset(this._session, offset);
    }

}

class CounterIterator {
    constructor(runtime, session) {
        this._runtime = runtime;
        this._session = session; // keeps the session alive while the iterator is reachable
        this._ptr = runtime._exports.MpCreateCounterIterator(session._ptr);
        runtime._counterIteratorFinalizer?.register(this, this._ptr, this);
    }

    Dispose() {
        if (this._ptr !== 0) {
            this._runtime._counterIteratorFinalizer?.unregister(this);
            this._runtime._exports.MpDeleteCounterIterator(this._ptr);
            this._ptr = 0;
        }
    }

    Configure(values = {}) {
        const builder = new flatbuffers.Builder(256);
        const config = makeConfig(MpTypes.CounterIteratorConfigT, values);
        builder.finish(config.pack(builder));
        const data = builder.asUint8Array();
        const destination = this._runtime._exports.MpGetCounterIteratorConfigBuffer(
            this._ptr, data.byteLength);
        if (destination === 0)
            return false;
        this._runtime._memoryBytes().set(data, destination);
        return this._runtime._exports.MpConfigureCounterIterator(this._ptr, 0) !== 0;
    }

    Step() {
        return this._runtime._exports.MpStepCounterIterator(this._ptr) !== 0;
    }

    Rewind() {
        this._runtime._exports.MpRewindCounterIterator(this._ptr);
    }

    GetState() {
        return this._runtime._wrapStruct(
            this._runtime._exports.MpGetCounterIteratorState(this._ptr),
            MpTypes.CounterIteratorState);
    }

    GetStackElement(stackLevel) {
        return this._runtime._exports.MpGetCounterIteratorStackElement(
            this._ptr, stackLevel) >>> 0;
    }

}

function newCacheEntry() {
    return { views: new Map(), objects: new Map() };
}

class Session {
    constructor(runtime, ptr, id, provider = null, ownedMemPtr = 0) {
        this._runtime = runtime;
        this._ptr = ptr;
        this._id = id;
        this._provider = provider;
        this._ownedMemPtr = ownedMemPtr;
        this._objectCacheEnabled = false;
        this._dataLockDepth = 0;
        this._syncRequired = false;
        this._labelCacheEnabled = false;
        this._labelCacheDirty = false;
        this._labelCache = new Map();
        this.Control = new Control(runtime, ptr);
        this._cache = [
            {
                GlobalDesc: newCacheEntry(),
                GeneralInfo: newCacheEntry(),
            },
            {
                GroupDesc: newCacheEntry(),
                TimerDesc: newCacheEntry(),
                CounterDesc: newCacheEntry(),
                ThreadDesc: newCacheEntry(),
            },
        ];

        runtime._sessionsByPtr.set(ptr,
            typeof WeakRef === "undefined" ? this : new WeakRef(this));
        runtime._sessionFinalizer?.register(
            this, { ptr, id, ownedMemPtr, control: this.Control }, this);
        this.EnableObjectCache(true);
    }

    Dispose() {
        if (this._ptr === 0)
            return;
        const ptr = this._ptr;
        this._runtime._sessionFinalizer?.unregister(this);
        this._runtime._exports.MpCloseSession(ptr);
        this._runtime._sessionsByPtr.delete(ptr);
        if (this._id !== 0)
            this._runtime._providers.delete(this._id);
        if (this._ownedMemPtr !== 0)
            this._runtime._exports.free(this._ownedMemPtr);
        this._ptr = 0;
        this.Control._ptr = 0; // its commands must not flag the freed session for resync
        this._ownedMemPtr = 0;
        this._provider = null;
        this._clearCache();
        this._labelCache.clear();
    }

    SyncWithDataSource() {
        return this._runtime._exports.MpSyncWithDataSource(this._ptr) !== 0;
    }

    SetSyncRequired() {
        this._runtime._exports.MpSetSyncRequired(this._ptr);
        this._syncRequired = true;
    }

    SetClearCacheOnSyncRequired() {
        this._runtime._exports.MpSetClearCacheOnSyncRequired(this._ptr);
    }

    SyncIfRequired() {
        if (this._syncRequired) {
            this._runtime._exports.MpSyncIfRequired(this._ptr);
            this._syncRequired = false;
        }
    }

    IsValid() {
        return this._runtime._exports.MpIsSessionValid(this._ptr) !== 0;
    }

    GetDataFormatVersion() {
        return this._runtime._exports.MpGetSessionDataFormatVersion(this._ptr) >>> 0;
    }

    GetTrackCount() {
        return this._runtime._exports.MpGetTrackCount(this._ptr) >>> 0;
    }

    GetTracks() {
        return new ObjectArray(this,
            this._runtime._exports.MpGetTracks(this._ptr),
            this._runtime._exports.MpGetTrackDescFromArray, MpTypes.TrackDesc, true);
    }

    FetchTracks() { return this._fetchAndDispose(this.GetTracks()); }

    GetSegmentCount(trackIndex) {
        return this._runtime._exports.MpGetSegmentCount(this._ptr, trackIndex) >>> 0;
    }

    GetSegments(trackIndex) {
        return new ObjectArray(this,
            this._runtime._exports.MpGetSegments(this._ptr, trackIndex),
            this._runtime._exports.MpGetSegmentDescFromArray, MpTypes.SegmentDesc, true);
    }

    FetchSegments(trackIndex) { return this._fetchAndDispose(this.GetSegments(trackIndex)); }

    SelectTrack(trackIndex) {
        return this._runtime._exports.MpSelectTrack(this._ptr, trackIndex) !== 0;
    }

    SelectSegment(segmentIndex) {
        return this._runtime._exports.MpSelectSegment(this._ptr, segmentIndex) !== 0;
    }

    WithDataLock(callback) {
        if (this._runtime._exports.MpPushDataLock(this._ptr) === 0)
            return { locked: false, value: undefined };
        this._dataLockDepth++;
        try {
            return { locked: true, value: callback() };
        } finally {
            this._dataLockDepth--;
            this._runtime._exports.MpPopDataLock(this._ptr);
        }
    }

    _getCached(cache, id, api, Type, isStruct) {
        this.SyncIfRequired();
        const key = id === undefined ? SINGLE_CACHE_KEY : id;
        if (this._objectCacheEnabled && cache.views.has(key)) {
            if (this._dataLockDepth > 0)
                this._runtime._exports.MpConsumeDataLock(this._ptr);
            return cache.views.get(key);
        }
        const ptr = id === undefined ? api(this._ptr) : api(this._ptr, id);
        const value = isStruct
            ? this._runtime._wrapStruct(ptr, Type)
            : this._runtime._wrapTable(ptr, Type);
        if (value && this._objectCacheEnabled)
            cache.views.set(key, value);
        return value;
    }

    _fetchCached(cache, id, getter) {
        this.SyncIfRequired();
        const key = id === undefined ? SINGLE_CACHE_KEY : id;
        if (this._objectCacheEnabled && cache.objects.has(key)) {
            if (this._dataLockDepth > 0)
                this._runtime._exports.MpConsumeDataLock(this._ptr);
            return cache.objects.get(key);
        }
        const value = getter.call(this, id)?.unpack() ?? null;
        if (value && this._objectCacheEnabled)
            cache.objects.set(key, value);
        return value;
    }

    _clearCacheBucket(index) {
        const bucket = this._cache[index];
        if (!bucket)
            return;
        for (const cache of Object.values(bucket)) {
            cache.views.clear();
            cache.objects.clear();
        }
    }

    _clearCache() {
        for (let i = 0; i < this._cache.length; ++i)
            this._clearCacheBucket(i);
    }

    EnableObjectCache(enabled) {
        this._objectCacheEnabled = Boolean(enabled);
        const value = enabled ? 1 : 0;
        this._runtime._exports.MpEnableObjectCache(this._ptr, value, value, value, value);
        if (!enabled)
            this._clearCache();
    }

    ClearObjectCache() {
        this._runtime._exports.MpClearObjectCache(this._ptr);
        this._clearCache();
    }

    _invalidateLabelCache(clearAll) {
        this._labelCacheDirty = true;
        if (clearAll)
            this._labelCache.clear();
    }

    _refreshLabelCache() {
        if (!this._labelCacheEnabled || !this._labelCacheDirty)
            return;
        const ptr = this._runtime._exports.MpInternalGetLabelBufferRegions(this._ptr);
        const regions = new ObjectArray(this, ptr,
            this._runtime._exports.MpInternalGetLabelBufferRegionFromArray,
            MpTypes.LabelBufferRegion, true);
        try {
            for (const chunkId of this._labelCache.keys()) {
                const chunkStart = chunkId * this._runtime._labelCacheChunkSize;
                const chunkEnd = chunkStart + this._runtime._labelCacheChunkSize;
                let covered = false;
                for (let i = 0; i < regions.Size(); ++i) {
                    const region = regions.Get(i);
                    if (region && region.startOffset() <= chunkStart &&
                        region.endOffset() >= chunkEnd) {
                        covered = true;
                        break;
                    }
                }
                if (!covered)
                    this._labelCache.delete(chunkId);
            }
        } finally {
            regions.Dispose();
        }
        this._labelCacheDirty = false;
    }

    _fetchLabel(offset, call) {
        this.SyncIfRequired();
        this._refreshLabelCache();
        const chunkId = offset / this._runtime._labelCacheChunkSize;
        if (this._labelCacheEnabled) {
            const cached = this._labelCache.get(chunkId)?.get(offset);
            if (cached !== undefined)
                return cached;
        }
        const value = this._runtime._withStack(4, (lengthPtr) => {
            this._runtime._globalBuffer.writeUint32(lengthPtr, 0);
            const ptr = call(lengthPtr);
            if (ptr === 0)
                return null;
            const length = this._runtime._globalBuffer.readUint32(lengthPtr);
            return textDecoder.decode(this._runtime._memoryBytes().subarray(ptr, ptr + length));
        });
        if (value !== null && this._labelCacheEnabled) {
            let chunk = this._labelCache.get(chunkId);
            if (!chunk) {
                chunk = new Map();
                this._labelCache.set(chunkId, chunk);
            }
            chunk.set(offset, value);
        }
        return value;
    }

    EnableLabelCache(enabled) {
        this._labelCacheEnabled = Boolean(enabled);
        if (!enabled)
            this._labelCache.clear();
    }

    ClearLabelCache() {
        this._labelCache.clear();
        this._labelCacheDirty = false;
    }

    GetObjSize() { return this._runtime._exports.MpGetObjSize(this._ptr) >>> 0; }

    GetGlobalDesc() {
        return this._getCached(this._cache[0].GlobalDesc, undefined,
            this._runtime._exports.MpGetGlobalDesc, MpTypes.GlobalDesc, true);
    }
    FetchGlobalDesc() {
        return this._fetchCached(this._cache[0].GlobalDesc, undefined, this.GetGlobalDesc);
    }

    GetGeneralInfo() {
        return this._getCached(this._cache[0].GeneralInfo, undefined,
            this._runtime._exports.MpGetGeneralInfo, MpTypes.GeneralInfo, false);
    }
    FetchGeneralInfo() {
        return this._fetchCached(this._cache[0].GeneralInfo, undefined, this.GetGeneralInfo);
    }

    GetGroupIdMax() { return this._runtime._exports.MpGetGroupIdMax(this._ptr) >>> 0; }
    GetGroupDesc(groupId) {
        return this._getCached(this._cache[1].GroupDesc, groupId,
            this._runtime._exports.MpGetGroupDesc, MpTypes.GroupDesc, false);
    }
    FetchGroupDesc(groupId) {
        return this._fetchCached(this._cache[1].GroupDesc, groupId, this.GetGroupDesc);
    }

    _scalarArray(ptr) {
        return new ScalarArray(this, ptr,
            this._runtime._exports.MpGetIdFromArray);
    }

    GetTimerIds() { return this._scalarArray(this._runtime._exports.MpGetTimerIds(this._ptr)); }
    FetchTimerIds() { return this._fetchAndDispose(this.GetTimerIds()); }
    GetTimerDesc(timerId) {
        return this._getCached(this._cache[1].TimerDesc, timerId,
            this._runtime._exports.MpGetTimerDesc, MpTypes.TimerDesc, false);
    }
    FetchTimerDesc(timerId) {
        return this._fetchCached(this._cache[1].TimerDesc, timerId, this.GetTimerDesc);
    }

    GetCounterIds() { return this._scalarArray(this._runtime._exports.MpGetCounterIds(this._ptr)); }
    FetchCounterIds() { return this._fetchAndDispose(this.GetCounterIds()); }
    GetCounterDesc(counterId) {
        return this._getCached(this._cache[1].CounterDesc, counterId,
            this._runtime._exports.MpGetCounterDesc, MpTypes.CounterDesc, false);
    }
    FetchCounterDesc(counterId) {
        return this._fetchCached(this._cache[1].CounterDesc, counterId, this.GetCounterDesc);
    }
    GetCounterSamples(counterId) {
        return new CounterSampleArray(this,
            this._runtime._exports.MpGetCounterSamples(this._ptr, counterId));
    }
    FetchCounterSamples(counterId) { return this._fetchAndDispose(this.GetCounterSamples(counterId)); }
    GetLastCounterSample(counterId) {
        return this._runtime._wrapStruct(
            this._runtime._exports.MpGetLastCounterSample(this._ptr, counterId),
            MpTypes.CounterSample);
    }
    FetchLastCounterSample(counterId) {
        return this.GetLastCounterSample(counterId)?.unpack() ?? null;
    }

    GetThreadIds() { return this._scalarArray(this._runtime._exports.MpGetThreadIds(this._ptr)); }
    FetchThreadIds() { return this._fetchAndDispose(this.GetThreadIds()); }
    GetThreadDesc(threadId) {
        return this._getCached(this._cache[1].ThreadDesc, threadId,
            this._runtime._exports.MpGetThreadDesc, MpTypes.ThreadDesc, false);
    }
    FetchThreadDesc(threadId) {
        return this._fetchCached(this._cache[1].ThreadDesc, threadId, this.GetThreadDesc);
    }
    GetThreadIdGpu() { return this._runtime._exports.MpGetThreadIdGpu(this._ptr) >>> 0; }

    GetFrameIdMin() { return this._runtime._exports.MpGetFrameIdMin(this._ptr) >>> 0; }
    GetFrameIdMax() { return this._runtime._exports.MpGetFrameIdMax(this._ptr) >>> 0; }
    GetFrameDesc(frameId) {
        return this._runtime._wrapTable(
            this._runtime._exports.MpGetFrameDesc(this._ptr, frameId), MpTypes.FrameDesc);
    }
    FetchFrameDesc(frameId) { return this.GetFrameDesc(frameId)?.unpack() ?? null; }

    GetPlaceIdSamples() {
        return new ObjectArray(this,
            this._runtime._exports.MpGetPlaceIdSamples(this._ptr),
            this._runtime._exports.MpGetU64SampleFromArray, MpTypes.U64Sample, true);
    }
    FetchPlaceIdSamples() { return this._fetchAndDispose(this.GetPlaceIdSamples()); }
    GetUtcTimestampSamples() {
        return new ObjectArray(this,
            this._runtime._exports.MpGetUtcTimestampSamples(this._ptr),
            this._runtime._exports.MpGetU64SampleFromArray, MpTypes.U64Sample, true);
    }
    FetchUtcTimestampSamples() { return this._fetchAndDispose(this.GetUtcTimestampSamples()); }

    CreateLogIterator() { return new LogIterator(this._runtime, this); }
    CreateCounterIterator() { return new CounterIterator(this._runtime, this); }

    _fetchAndDispose(array) {
        try {
            return array.FetchAll();
        } finally {
            array.Dispose();
        }
    }

    _callWithMask(api, nameMask, caseSensitive) {
        const bytes = textEncoder.encode(String(nameMask ?? ""));
        return this._runtime._withStack(bytes.byteLength + 1, (ptr) => {
            const memory = this._runtime._memoryBytes();
            memory.set(bytes, ptr);
            memory[ptr + bytes.byteLength] = 0;
            return api(this._ptr, ptr, caseSensitive ? 1 : 0);
        });
    }

    FindGroupId(mask, caseSensitive = false) {
        return this._callWithMask(this._runtime._exports.MpFindGroupId, mask, caseSensitive) >>> 0;
    }
    FindGroupIds(mask, caseSensitive = false) {
        return this._fetchAndDispose(this._scalarArray(
            this._callWithMask(this._runtime._exports.MpFindGroupIds, mask, caseSensitive)));
    }
    FindTimerId(mask, caseSensitive = false) {
        return this._callWithMask(this._runtime._exports.MpFindTimerId, mask, caseSensitive) >>> 0;
    }
    FindTimerIds(mask, caseSensitive = false) {
        return this._fetchAndDispose(this._scalarArray(
            this._callWithMask(this._runtime._exports.MpFindTimerIds, mask, caseSensitive)));
    }
    GetTimerIdsByGroupId(groupId) {
        return this._scalarArray(this._runtime._exports.MpGetTimerIdsByGroupId(this._ptr, groupId));
    }
    FetchTimerIdsByGroupId(groupId) { return this._fetchAndDispose(this.GetTimerIdsByGroupId(groupId)); }
    GetTimerIdsByGroupNames(mask, caseSensitive = false) {
        return this._scalarArray(this._callWithMask(
            this._runtime._exports.MpGetTimerIdsByGroupNames, mask, caseSensitive));
    }
    FetchTimerIdsByGroupNames(mask, caseSensitive = false) {
        return this._fetchAndDispose(this.GetTimerIdsByGroupNames(mask, caseSensitive));
    }
    FindCounterId(mask, caseSensitive = false) {
        return this._callWithMask(this._runtime._exports.MpFindCounterId, mask, caseSensitive) >>> 0;
    }
    FindCounterIds(mask, caseSensitive = false) {
        return this._fetchAndDispose(this._scalarArray(
            this._callWithMask(this._runtime._exports.MpFindCounterIds, mask, caseSensitive)));
    }
    FindThreadId(mask, caseSensitive = false) {
        return this._callWithMask(this._runtime._exports.MpFindThreadId, mask, caseSensitive) >>> 0;
    }
    FindThreadIds(mask, caseSensitive = false) {
        return this._fetchAndDispose(this._scalarArray(
            this._callWithMask(this._runtime._exports.MpFindThreadIds, mask, caseSensitive)));
    }
}

class LibMPInstance {
    constructor(options = {}) {
        this._options = options;
        this._instance = null;
        this._exports = null;
        this._memory = null;
        this._memoryBuffer = null;
        this._globalBuffer = new flatbuffers.ByteBuffer(new Uint8Array());
        this._nextProviderId = 1;
        this._providers = new Map();
        this._sessionsByPtr = new Map();
        this._toolOutput = null;
        this._toolOutputError = null;
        this._labelCacheChunkSize = 0n;

        if (typeof FinalizationRegistry !== "undefined") {
            this._arrayFinalizer = new FinalizationRegistry(
                (ptr) => ptr && this._exports.MpReleaseArray(ptr));
            this._logIteratorFinalizer = new FinalizationRegistry(
                (ptr) => ptr && this._exports.MpDeleteLogIterator(ptr));
            this._counterIteratorFinalizer = new FinalizationRegistry(
                (ptr) => ptr && this._exports.MpDeleteCounterIterator(ptr));
            this._sessionFinalizer = new FinalizationRegistry((state) => {
                this._exports.MpCloseSession(state.ptr);
                this._sessionsByPtr.delete(state.ptr);
                if (state.id)
                    this._providers.delete(state.id);
                if (state.ownedMemPtr)
                    this._exports.free(state.ownedMemPtr);
                state.control._ptr = 0;
            });
        }
    }

    async _initialize() {
        const imports = this._createImports();
        const result = await WebAssembly.instantiate(decodeBase64(LIBMP_WASM_BASE64), imports);
        this._instance = result.instance;
        this._exports = result.instance.exports;
        this._memory = this._exports.memory;
        if (!(this._memory instanceof WebAssembly.Memory))
            throw new Error("LibMP WASM does not export linear memory");
        this._refreshMemory();
        this._exports.MpInit();
        this._labelCacheChunkSize = BigInt(
            this._exports.MpInternalGetLabelCacheChunkSize() >>> 0);

        const runtime = this;
        this.Session = class BoundSession extends Session {
            static OpenFromMem(data) { return runtime._openFromMem(data); }
            static OpenFromBuffer(data, cacheSize = 0) {
                return runtime._openFromBuffer(data, cacheSize);
            }
            static OpenFromExternalProvider(provider, options = {}) {
                return runtime._openFromExternalProvider(provider, options);
            }
            static async OpenFromFile(path, cacheSize = 0) {
                const fs = await import("node:fs");
                let descriptor = null;
                let fileSize = 0n;
                let openError = null;
                const provider = {
                    open() {
                        try {
                            descriptor = fs.openSync(path, "r");
                            fileSize = fs.fstatSync(descriptor, { bigint: true }).size;
                            return true;
                        } catch (error) {
                            if (descriptor !== null) {
                                try {
                                    fs.closeSync(descriptor);
                                } catch {
                                }
                            }
                            descriptor = null;
                            fileSize = 0n;
                            openError = error;
                            return false;
                        }
                    },
                    close() {
                        if (descriptor !== null) {
                            fs.closeSync(descriptor);
                            descriptor = null;
                        }
                        fileSize = 0n;
                        return true;
                    },
                    getSize: () => fileSize,
                    read(destination, blockId, offset, size) {
                        if (descriptor === null || blockId !== 0 ||
                            offset > fileSize || BigInt(size) > fileSize - offset) {
                            return 0;
                        }
                        let bytesRead = 0;
                        while (bytesRead < size) {
                            const count = fs.readSync(
                                descriptor, destination, bytesRead, size - bytesRead,
                                offset + BigInt(bytesRead));
                            if (count === 0)
                                break;
                            bytesRead += count;
                        }
                        return bytesRead;
                    },
                };
                const session = runtime._openFromExternalProvider(provider, { cacheSize });
                if (openError)
                    throw openError;
                return session;
            }
            static OpenFromSlot(_slotId, _cacheSize = 0) { return null; }
        };
        this.Control = new Control(this, 0);
        this.Control.Global = this.Control;
        this.Control.UseControlChannel(typeof this._options.controlMessage === "function");
        this.Tools = new Tools(this);
        this.Enums = Enums;
        this.Experimental = Experimental;
        this.Versions = Object.freeze({
            DataFormat: this._exports.MpGetDataFormatVersion() >>> 0,
            Library: this._exports.MpGetLibraryVersion() >>> 0,
        });
        return this;
    }

    GetMemUsed() { return this._exports.MpGetMemUsed() >>> 0; }
    GetMemReserved() { return this._exports.MpGetMemReserved() >>> 0; }

    // Called at initialization and by emscripten_notify_memory_growth. Every
    // generated FlatBuffers object shares _globalBuffer.
    _refreshMemory() {
        if (!this._memory)
            return;
        if (this._memoryBuffer !== this._memory.buffer) {
            this._memoryBuffer = this._memory.buffer;
            this._globalBuffer.bytes_ = new Uint8Array(this._memoryBuffer);
        }
    }

    _memoryBytes() {
        return this._globalBuffer.bytes();
    }

    _readU64Pointer(ptr) {
        if (!ptr)
            return null;
        const bb = this._globalBuffer;
        const value = BigInt(bb.readUint32(ptr)) +
            (BigInt(bb.readUint32(ptr + 4)) << 32n);
        return value === 0n ? null : value;
    }

    _wrapTable(ptr, Type) {
        if (!ptr)
            return null;
        return new Type().__init(ptr + this._globalBuffer.readInt32(ptr), this._globalBuffer);
    }

    _wrapStruct(ptr, Type) {
        if (!ptr)
            return null;
        return new Type().__init(ptr, this._globalBuffer);
    }

    _withStack(size, callback) {
        const stack = this._exports.emscripten_stack_get_current();
        const ptr = this._exports._emscripten_stack_alloc(size);
        try {
            return callback(ptr);
        } finally {
            this._exports._emscripten_stack_restore(stack);
        }
    }

    _openFromMem(data) {
        const bytes = asBytes(data);
        const ptr = this._exports.malloc(bytes.byteLength);
        if (ptr === 0 && bytes.byteLength !== 0)
            throw new Error("Unable to allocate WASM memory for the capture");
        this._memoryBytes().set(bytes, ptr);
        const sessionPtr = this._exports.MpOpenFromMemTrusted(ptr, bytes.byteLength);
        if (sessionPtr === 0) {
            this._exports.free(ptr);
            return null;
        }
        return new this.Session(this, sessionPtr, 0, null, ptr);
    }

    _openFromBuffer(data, cacheSize) {
        const bytes = asBytes(data);
        const provider = {
            getSize: () => bytes.byteLength,
            read: (destination, blockId, offset, size) => {
                if (blockId !== 0 || offset > BigInt(Number.MAX_SAFE_INTEGER))
                    return 0;
                const start = Number(offset);
                if (start > bytes.byteLength || size > bytes.byteLength - start)
                    return 0;
                destination.set(bytes.subarray(start, start + size));
                return size;
            },
        };
        return this._openFromExternalProvider(provider, { cacheSize });
    }

    _openFromExternalProvider(provider, options = {}) {
        if (!provider || typeof provider.getSize !== "function" || typeof provider.read !== "function")
            throw new TypeError("External provider must define getSize() and read()");
        const cacheSize = options.cacheSize ?? 0;
        if (!Number.isInteger(cacheSize) || cacheSize < 0 || cacheSize > 0xffff_ffff)
            throw new RangeError("cacheSize must fit in an unsigned 32-bit integer");
        if (this._nextProviderId > 0x7fff_ffff)
            throw new Error("External data handle space is exhausted");
        const id = this._nextProviderId++;
        this._providers.set(id, provider);
        const sessionPtr = this._exports.MpOpenFromExternalDataTrusted(
            id, options.useBlockFetch ? 1 : 0, cacheSize);
        if (sessionPtr === 0) {
            this._providers.delete(id);
            return null;
        }
        return new this.Session(this, sessionPtr, id, provider, 0);
    }

    _providerCall(handle, callback, fallback = 0) {
        const provider = this._providers.get(handle >>> 0);
        if (!provider)
            return fallback;
        try {
            return callback(provider);
        } catch (error) {
            this._reportError("External data provider failed", error);
            return fallback;
        }
    }

    _runTool(session, format, output) {
        if (!(session instanceof Session) || session._runtime !== this || session._ptr === 0)
            throw new TypeError("Expected a live Session owned by this LibMP instance");
        if (!Number.isInteger(format))
            throw new TypeError("Container format must be an enum value");
        if (this._toolOutput !== null)
            throw new Error("A LibMP tool operation is already active");
        this._toolOutput = output;
        this._toolOutputError = null;
        let success;
        try {
            success = this._exports.MpToolConvertSession(session._ptr, format) !== 0;
        } finally {
            const error = this._toolOutputError;
            this._toolOutput = null;
            this._toolOutputError = null;
            if (error)
                throw error;
        }
        return success;
    }

    _reportError(message, error) {
        if (typeof this._options.onError === "function")
            this._options.onError(error, message);
        else
            console.error(message, error);
    }

    _createImports() {
        const runtime = this;
        return {
            env: {
                emscripten_notify_memory_growth() {
                    runtime._refreshMemory();
                },
                LibMpExtConsoleLog(level, ptr, size) {
                    const message = textDecoder.decode(
                        runtime._memoryBytes().subarray(ptr, ptr + (size >>> 0)));
                    if (typeof runtime._options.log === "function")
                        runtime._options.log(level >>> 0, message);
                    else if ((level >>> 0) >= 2)
                        console.warn(message);
                    else
                        console.log(message);
                },
                LibMpExtControlMsg(requestPtr, requestSize, responsePtr, responseCapacity) {
                    const handler = runtime._options.controlMessage;
                    if (typeof handler !== "function")
                        return 0;
                    try {
                        const memory = runtime._memoryBytes();
                        const request = memory.subarray(requestPtr, requestPtr + (requestSize >>> 0));
                        const response = memory.subarray(
                            responsePtr, responsePtr + (responseCapacity >>> 0));
                        const result = handler(request, response);
                        if (result instanceof Uint8Array) {
                            if (result.byteLength > response.byteLength)
                                return 0;
                            response.set(result);
                            return result.byteLength;
                        }
                        const size = Number(result);
                        return Number.isInteger(size) && size >= 0 && size <= response.byteLength
                            ? size : 0;
                    } catch (error) {
                        runtime._reportError("Control-message callback failed", error);
                        return 0;
                    }
                },
                LibMpExtToolOutput(dataPtr, size) {
                    const output = runtime._toolOutput;
                    if (typeof output !== "function")
                        return 0;
                    try {
                        const start = dataPtr >>> 0;
                        const byteCount = size >>> 0;
                        const memory = runtime._memoryBytes();
                        if (start > memory.byteLength || byteCount > memory.byteLength - start)
                            return 0;
                        return output(memory.subarray(start, start + byteCount)) === false ? 0 : 1;
                    } catch (error) {
                        runtime._toolOutputError = error;
                        return 0;
                    }
                },
                LibMpExtClearObjCacheCallback(handle, bucketIdsPtr, bucketCount) {
                    const entry = runtime._sessionsByPtr.get(handle);
                    const session = typeof WeakRef !== "undefined" && entry instanceof WeakRef
                        ? entry.deref() : entry;
                    if (!session)
                        return;
                    const bb = runtime._globalBuffer;
                    for (let i = 0; i < (bucketCount >>> 0); ++i)
                        session._clearCacheBucket(bb.readUint32(bucketIdsPtr + i * 4));
                },
                LibMpExtSessionNotificationCallback(handle, notificationFlags) {
                    const entry = runtime._sessionsByPtr.get(handle);
                    const session = typeof WeakRef !== "undefined" && entry instanceof WeakRef
                        ? entry.deref() : entry;
                    if (!session)
                        return;
                    const flags = notificationFlags >>> 0;
                    if ((flags & SESSION_NOTIFY_LABEL_CACHE_RESET) !== 0)
                        session._invalidateLabelCache(true);
                    else if ((flags & SESSION_NOTIFY_LABEL_REGIONS_CHANGED) !== 0)
                        session._invalidateLabelCache(false);
                },
                GprxExtDataOpen(handle) {
                    return runtime._providerCall(handle, (provider) =>
                        provider.open === undefined || provider.open() !== false ? 1 : 0);
                },
                GprxExtDataClose(handle) {
                    return runtime._providerCall(handle, (provider) =>
                        provider.close === undefined || provider.close() !== false ? 1 : 0);
                },
                GprxExtDataGetSize(handle, lowPtr, highPtr) {
                    return runtime._providerCall(handle, (provider) => {
                        const size = asU64(provider.getSize(), "External data size");
                        const bb = runtime._globalBuffer;
                        bb.writeUint32(lowPtr, Number(size & 0xffff_ffffn));
                        bb.writeUint32(highPtr, Number(size >> 32n));
                        return 1;
                    });
                },
                GprxExtDataRead(handle, destinationPtr, blockId, offsetLow, offsetHigh, size) {
                    return runtime._providerCall(handle, (provider) => {
                        if (size < 0)
                            return 0;
                        const byteCount = size >>> 0;
                        const offset = BigInt(offsetLow >>> 0) +
                            (BigInt(offsetHigh >>> 0) << 32n);
                        const destination = runtime._memoryBytes().subarray(
                            destinationPtr, destinationPtr + byteCount);
                        const result = provider.read(destination, blockId >>> 0, offset, byteCount);
                        if (result instanceof Uint8Array) {
                            if (result.byteLength > byteCount)
                                return 0;
                            destination.set(result);
                            return result.byteLength;
                        }
                        const bytesRead = Number(result);
                        return Number.isInteger(bytesRead) && bytesRead >= 0 && bytesRead <= byteCount
                            ? bytesRead : 0;
                    });
                },
            },
            wasi_snapshot_preview1: {
                fd_close() { return 0; },
                fd_write() { return 0; },
                fd_seek() { return 0; },
            },
        };
    }
}

export async function create(options = {}) {
    return new LibMPInstance(options)._initialize();
}

export { Enums, Experimental, flatbuffers as FlatBuffers, MpTypes as Types, LibMPInstance, Tools };
