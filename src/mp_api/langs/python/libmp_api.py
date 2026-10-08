# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

# LibMP for Python
# Access Roblox MicroProfiler data from Python applications.
# %LIBMP_SHARED_METADATA%
#
# Requirements:
# - Python 3.10+.
# - wasmtime package: python -m pip install "wasmtime>=20.0.0,<50.0.0"
#
# Minimal usage:
#   import LibMP
#   libmp = LibMP.create()
#   session = libmp.Session.OpenFromFile("capture.x.html")  # or "capture.gprx"
#   print(session.FetchGlobalDesc().tickToMsCpu)
#   session.Dispose()
#   libmp.Dispose()

"""Language wrapper appended to the generated single-file LibMP module."""

from __future__ import annotations

import base64
import collections
import ctypes
import importlib.metadata
import struct
import threading
import weakref
from enum import IntEnum
from pathlib import Path
from types import SimpleNamespace
from typing import Any, Callable

try:
    import wasmtime
except ModuleNotFoundError as error:
    if error.name != "wasmtime":
        raise
    raise ModuleNotFoundError(
        "LibMP requires the Wasmtime runtime. Install it with:\n"
        f"  python -m pip install 'wasmtime{WASMTIME_PYTHON_REQUIREMENT}'"
    ) from None


_U32_MASK = 0xFFFFFFFF
_FAST_CALL_UNAVAILABLE = object()


class ContainerFormat(IntEnum):
    Raw = Types.ContainerFormat.Raw
    Compressed = Types.ContainerFormat.Compressed
    CompressedAscii = Types.ContainerFormat.CompressedAscii
    CompressedHtml = Types.ContainerFormat.CompressedHtml


class ContentMode(IntEnum):
    Full = Types.ContentMode.Full
    CountersOnly = Types.ContentMode.CountersOnly


class AppType(IntEnum):
    Unknown = Types.AppType.Unknown
    Client = Types.AppType.Client
    Studio = Types.AppType.Studio
    Server = Types.AppType.Server


class DataSlot(IntEnum):
    Live = Types.DataSlot.Live
    FirstUserManaged = Types.DataSlot.FirstUserManaged
    LastUserManaged = Types.DataSlot.LastUserManaged
    Temporary = Types.DataSlot.Temporary


Enums = SimpleNamespace(
    AppType=AppType, ContainerFormat=ContainerFormat, ContentMode=ContentMode,
    DataSlot=DataSlot)

_SESSION_NOTIFY_LABEL_REGIONS_CHANGED = 1 << 0
_SESSION_NOTIFY_LABEL_CACHE_RESET = 1 << 1


def _u32(value: int) -> int:
    return int(value) & _U32_MASK


def _method(value: Any, *names: str) -> Callable[..., Any] | None:
    for name in names:
        candidate = getattr(value, name, None)
        if callable(candidate):
            return candidate
    return None


def _dispose_later(target: Any) -> None:
    # Finalizers can run inside a LibMP callback or on another thread, so they leave
    # the object to the next top-level call on its instance
    runtime = getattr(target, "_runtime", None)
    if getattr(target, "_ptr", 0) and runtime is not None and runtime._instance is not None:
        runtime._pending_disposals.append(target)


def _config(config_type: type, value: Any) -> Any:
    if value is None:
        return config_type()
    if isinstance(value, config_type):
        return value
    if not isinstance(value, dict):
        raise TypeError(f"expected {config_type.__name__} or dict")
    result = config_type()
    for name, field_value in value.items():
        candidates = (name, name[:1].lower() + name[1:])
        field = next((candidate for candidate in candidates if hasattr(result, candidate)), None)
        if field is None:
            raise TypeError(f"unknown {config_type.__name__} field: {name}")
        setattr(result, field, field_value)
    return result


class _FastWasmtimeFunction:
    """An integer-only Wasmtime call with its validated ABI cached."""

    __slots__ = (
        "_call", "_context", "_function", "_function_ptr", "_lock",
        "_param_count", "_result_count", "_trap", "_trap_ptr", "_values",
        "_wasmtime", "_raise_last_exception")

    def __init__(self, ffi: Any, native_call: Any, store: Any, function: Any,
                 lock: Any, raise_last_exception: Callable[[], None]):
        function_type = function.type(store)
        i32 = wasmtime.ValType.i32()
        if any(value_type != i32 for value_type in function_type.params) or \
                len(function_type.results) > 1 or \
                any(value_type != i32 for value_type in function_type.results):
            raise TypeError("fast calls support only LibMP's i32 function ABI")

        self._call = native_call
        self._context = store._context()
        self._function = function
        self._function_ptr = ctypes.byref(function._func)
        self._lock = lock
        self._param_count = len(function_type.params)
        self._result_count = len(function_type.results)
        self._values = (ffi.wasmtime_val_raw_t * max(
            self._param_count, self._result_count, 1))()
        self._wasmtime = ffi
        self._raise_last_exception = raise_last_exception
        self._reset_trap()

    def _reset_trap(self) -> None:
        self._trap = ctypes.POINTER(self._wasmtime.wasm_trap_t)()
        self._trap_ptr = ctypes.byref(self._trap)

    def __call__(self, args: tuple[Any, ...]) -> Any:
        if len(args) != self._param_count:
            return _FAST_CALL_UNAVAILABLE
        with self._lock:
            for index, value in enumerate(args):
                self._values[index].i32 = int(value)
            error = self._call(
                self._context, self._function_ptr, self._values,
                max(self._param_count, self._result_count), self._trap_ptr)
            if error:
                exception = wasmtime.WasmtimeError._from_ptr(error)
                self._raise_last_exception()
                raise exception
            if self._trap:
                exception = wasmtime.Trap._from_ptr(self._trap)
                self._reset_trap()
                self._raise_last_exception()
                raise exception
            if self._result_count == 0:
                return 0
            return int(self._values[0].i32)


class _FastWasmtimeCalls:
    """Capability-checked access to Wasmtime's typed native call API."""

    __slots__ = ("_functions",)

    def __init__(self, functions: dict[str, _FastWasmtimeFunction]):
        self._functions = functions

    @classmethod
    def create(cls, store: Any, exports: dict[str, Any]) -> "_FastWasmtimeCalls | None":
        try:
            version = importlib.metadata.version("wasmtime")
            major = int(version.split(".", 1)[0])
            if not WASMTIME_FAST_MIN_MAJOR <= major < \
                    WASMTIME_FAST_MAX_MAJOR_EXCLUSIVE:
                return None

            from wasmtime import _ffi as ffi
            from wasmtime._func import maybe_raise_last_exn

            native_call = ffi.dll.wasmtime_func_call_unchecked
            if ctypes.sizeof(ffi.wasmtime_val_raw_t) != 16 or \
                    len(native_call.argtypes or ()) != 5:
                return None

            lock = threading.RLock()
            functions = {}
            for name, function in exports.items():
                if isinstance(function, wasmtime.Func):
                    functions[name] = _FastWasmtimeFunction(
                        ffi, native_call, store, function, lock, maybe_raise_last_exn)
            return cls(functions)
        except (AttributeError, ImportError, TypeError, ValueError):
            return None

    def call(self, name: str, args: tuple[Any, ...]) -> Any:
        function = self._functions.get(name)
        if function is None:
            return _FAST_CALL_UNAVAILABLE
        return function(args)


class _View:
    """A zero-copy FlatBuffer view that follows WASM memory growth."""

    __slots__ = (
        "_runtime", "_type", "_object_type", "_raw_view", "_memory_generation")

    def __init__(self, runtime: "LibMPInstance", view_type: type, position: int,
                 is_table: bool, object_type: type | None = None):
        self._runtime = runtime
        self._type = view_type
        self._object_type = object_type
        memory = runtime._memory_bytes()
        if is_table:
            position += struct.unpack_from("<I", memory, position)[0]
        self._raw_view = view_type()
        self._raw_view.Init(memory, position)
        self._memory_generation = runtime._memory_generation

    @classmethod
    def _from_raw(cls, runtime: "LibMPInstance", raw_view: Any,
                  object_type: type | None) -> "_View":
        result = cls.__new__(cls)
        result._runtime = runtime
        result._type = type(raw_view)
        result._object_type = object_type
        result._raw_view = raw_view
        result._memory_generation = runtime._memory_generation
        return result

    def _raw(self) -> Any:
        if self._memory_generation != self._runtime._memory_generation:
            self._raw_view._tab.Bytes = self._runtime._memory_bytes()
            self._memory_generation = self._runtime._memory_generation
        return self._raw_view

    def UnPack(self) -> Any:
        if self._object_type is None:
            return self._raw()
        return self._object_type.InitFromObj(self._raw())

    def __getattr__(self, name: str) -> Any:
        type_attribute = getattr(self._type, name, None)
        if not callable(type_attribute):
            return getattr(self._raw(), name)

        def call(*args: Any, **kwargs: Any) -> Any:
            value = getattr(self._raw(), name)(*args, **kwargs)
            if value is not None and hasattr(value, "_tab"):
                return _View._from_raw(
                    self._runtime, value,
                    getattr(Types, type(value).__name__ + "T", None))
            return value

        return call


class Control:
    def __init__(self, runtime: "LibMPInstance", session_ptr: int = 0):
        self._runtime = runtime
        self._ptr = session_ptr
        self.Global = self

    def EnableProfiler(self, enable: bool) -> bool:
        return bool(self._runtime._call("MpControlEnableProfiler", self._ptr, int(enable)))

    def EnableCapture(self, enable: bool) -> bool:
        return bool(self._runtime._call("MpControlEnableCapture", self._ptr, int(enable)))

    def ShowUI(self, show: bool) -> bool:
        return bool(self._runtime._call("MpControlShowUI", self._ptr, int(show)))

    def SetFrameLimit(self, count: int) -> bool:
        return bool(self._runtime._call("MpControlSetFrameLimit", self._ptr, count))

    def SetMemTargetMB(self, target_mb: int) -> bool:
        return bool(self._runtime._call("MpControlSetMemTargetMB", self._ptr, target_mb))

    def CaptureToBufferSync(self, capture_params: Any = None) -> bytes | None:
        return None

    def CaptureToSlotSync(self, target_slot_id: int,
                          capture_params: Any = None) -> bool:
        return False

    def SaveSlotToBuffer(self, slot_id: int) -> bytes | None:
        return None

    def ResetSlot(self, slot_id: int) -> bool:
        return False

    def UseControlChannel(self, use: bool) -> bool:
        return bool(self._runtime._call("MpUseControlChannel", int(use)))

    def IsBackendVersionCompatible(self) -> bool:
        return bool(self._runtime._call("MpIsBackendVersionCompatible"))

    def IsBackendAccessible(self) -> bool:
        return bool(self._runtime._call("MpIsBackendAccessible"))

    def IsBackendReady(self) -> bool:
        return bool(self._runtime._call("MpIsBackendReady"))


class ArrayBase:
    def __init__(self, session: "Session", array_ptr: int):
        self._runtime = session._runtime
        # Element getters take the session; after its Dispose they get 0 and return None
        self._session = session
        self._ptr = array_ptr

    def Dispose(self) -> None:
        if self._ptr:
            self._runtime._call("MpReleaseArray", self._ptr)
            self._ptr = 0

    def __enter__(self) -> "ArrayBase":
        return self

    def __exit__(self, *_: Any) -> None:
        self.Dispose()

    def __del__(self) -> None:
        _dispose_later(self)

    def Size(self) -> int:
        return _u32(self._runtime._call("MpGetArraySize", self._ptr)) if self._ptr else 0

    def FetchAll(self) -> list[Any]:
        return [self.Fetch(index) for index in range(self.Size())]


class ScalarArray(ArrayBase):
    def __init__(self, session: "Session", array_ptr: int,
                 getter: str = "MpGetIdFromArray"):
        super().__init__(session, array_ptr)
        self._getter = getter

    def Get(self, index: int) -> int:
        return _u32(self._runtime._call(
            self._getter, self._session._ptr, self._ptr, index))

    def Fetch(self, index: int) -> int:
        return self.Get(index)


class ObjectArray(ArrayBase):
    def __init__(self, session: "Session", array_ptr: int,
                 getter: str, view_type: type, is_table: bool):
        super().__init__(session, array_ptr)
        self._getter = getter
        self._view_type = view_type
        self._is_table = is_table

    def Get(self, index: int) -> _View | None:
        return self._runtime._wrap(
            self._runtime._call(self._getter, self._session._ptr, self._ptr, index),
            self._view_type, self._is_table)

    def Fetch(self, index: int) -> Any:
        value = self.Get(index)
        return value.UnPack() if value else None


class CounterSampleArray(ObjectArray):
    def __init__(self, session: "Session", array_ptr: int):
        super().__init__(session, array_ptr,
                         "MpGetCounterSampleFromArray", Types.CounterSample, False)

    def GetSampleByFrameId(self, frame_id: int) -> _View | None:
        return self._runtime._wrap(self._runtime._call(
            "MpGetCounterSampleFromArrayByFrameId", self._session._ptr,
            self._ptr, frame_id), Types.CounterSample, False)

    def FetchSampleByFrameId(self, frame_id: int) -> Any:
        value = self.GetSampleByFrameId(frame_id)
        return value.UnPack() if value else None

    def GetLastSample(self) -> _View | None:
        return self._runtime._wrap(self._runtime._call(
            "MpGetLastCounterSampleFromArray", self._session._ptr, self._ptr),
            Types.CounterSample, False)

    def FetchLastSample(self) -> Any:
        value = self.GetLastSample()
        return value.UnPack() if value else None


class LogIterator:
    def __init__(self, runtime: "LibMPInstance", session: "Session"):
        self._runtime = runtime
        self._session = session
        self._ptr = runtime._call("MpCreateLogIterator", session._ptr)

    def Dispose(self) -> None:
        if self._ptr:
            self._runtime._call("MpDeleteLogIterator", self._ptr)
            self._ptr = 0

    def __enter__(self) -> "LogIterator":
        return self

    def __exit__(self, *_: Any) -> None:
        self.Dispose()

    def __del__(self) -> None:
        _dispose_later(self)

    def Configure(self, config: Any = None) -> bool:
        return self._runtime._configure(
            self._ptr, _config(Types.LogIteratorConfigT, config),
            "MpGetLogIteratorConfigBuffer", "MpConfigureLogIterator")

    def Step(self) -> bool:
        return bool(self._runtime._call("MpStepLogIterator", self._ptr))

    def Rewind(self) -> None:
        self._runtime._call("MpRewindLogIterator", self._ptr)

    def RewindTo(self, frame_id_min: int, frame_id_max: int) -> None:
        self._runtime._call("MpRewindLogIteratorTo", self._ptr,
                            frame_id_min, frame_id_max)

    def GetState(self) -> _View | None:
        return self._runtime._wrap(self._runtime._call(
            "MpGetLogIteratorState", self._ptr), Types.LogIteratorState, False)

    def GetThreadStackState(self, thread_id: int) -> _View | None:
        return self._runtime._wrap(self._runtime._call(
            "MpGetLogIteratorThreadStackState", self._ptr, thread_id),
            Types.LogIteratorThreadStackState, False)

    def GetThreadStackElement(self, thread_id: int, stack_level: int) -> _View | None:
        return self._runtime._wrap(self._runtime._call(
            "MpGetLogIteratorThreadStackElement", self._ptr, thread_id, stack_level),
            Types.LogIteratorThreadStackElement, False)

    def GetCurrentThreadStackElement(self, stack_level: int) -> _View | None:
        return self._runtime._wrap(self._runtime._call(
            "MpGetLogIteratorCurrentThreadStackElement", self._ptr, stack_level),
            Types.LogIteratorThreadStackElement, False)

    def GetCurrentEntryCount(self) -> int:
        return _u32(self._runtime._call(
            "MpGetLogIteratorCurrentEntryCount", self._ptr))

    def GetCurrentEntryPart(self, entry_index: int, part_index: int) -> int:
        return _u32(self._runtime._call(
            "MpGetLogIteratorCurrentEntryPart", self._ptr,
            entry_index, part_index))

    def _get_current_label_offset(self) -> int | None:
        return self._runtime._read_u64_pointer(self._runtime._call(
            "MpGetLogIteratorCurrentLabelOffset", self._ptr))

    def FetchCurrentLabel(self) -> str | None:
        offset = self._get_current_label_offset()
        return None if offset is None else self._session._fetch_label_from_offset(offset)

    def GetScopeLabelCount(self, thread_id: int, stack_level: int) -> int:
        return _u32(self._runtime._call(
            "MpGetLogIteratorScopeLabelCount", self._ptr, thread_id, stack_level))

    def GetCurrentScopeLabelCount(self) -> int:
        return _u32(self._runtime._call(
            "MpGetLogIteratorCurrentScopeLabelCount", self._ptr))

    def _get_scope_label_offset(
            self, thread_id: int, stack_level: int, label_index: int) -> int | None:
        return self._runtime._read_u64_pointer(self._runtime._call(
            "MpGetLogIteratorScopeLabelOffset", self._ptr,
            thread_id, stack_level, label_index))

    def _get_current_scope_label_offset(self, label_index: int) -> int | None:
        return self._runtime._read_u64_pointer(self._runtime._call(
            "MpGetLogIteratorCurrentScopeLabelOffset", self._ptr, label_index))

    def FetchScopeLabel(
            self, thread_id: int, stack_level: int, label_index: int) -> str | None:
        offset = self._get_scope_label_offset(thread_id, stack_level, label_index)
        return None if offset is None else self._session._fetch_label_from_offset(offset)

    def FetchCurrentScopeLabel(self, label_index: int) -> str | None:
        offset = self._get_current_scope_label_offset(label_index)
        return None if offset is None else self._session._fetch_label_from_offset(offset)


class CounterIterator:
    def __init__(self, runtime: "LibMPInstance", session: "Session"):
        self._runtime = runtime
        self._session = session  # keeps the session alive while the iterator is reachable
        self._ptr = runtime._call("MpCreateCounterIterator", session._ptr)

    def Dispose(self) -> None:
        if self._ptr:
            self._runtime._call("MpDeleteCounterIterator", self._ptr)
            self._ptr = 0

    def __enter__(self) -> "CounterIterator":
        return self

    def __exit__(self, *_: Any) -> None:
        self.Dispose()

    def __del__(self) -> None:
        _dispose_later(self)

    def Configure(self, config: Any = None) -> bool:
        return self._runtime._configure(
            self._ptr, _config(Types.CounterIteratorConfigT, config),
            "MpGetCounterIteratorConfigBuffer", "MpConfigureCounterIterator")

    def Step(self) -> bool:
        return bool(self._runtime._call("MpStepCounterIterator", self._ptr))

    def Rewind(self) -> None:
        self._runtime._call("MpRewindCounterIterator", self._ptr)

    def GetState(self) -> _View | None:
        return self._runtime._wrap(self._runtime._call(
            "MpGetCounterIteratorState", self._ptr), Types.CounterIteratorState, False)

    def GetStackElement(self, stack_level: int) -> int:
        return _u32(self._runtime._call(
            "MpGetCounterIteratorStackElement", self._ptr, stack_level))


def _new_cache_entry() -> dict[str, dict[Any, Any]]:
    return {"views": {}, "objects": {}}


class Session:
    def __init__(self, runtime: "LibMPInstance", ptr: int, provider_id: int = 0,
                 provider: Any = None, owned_mem_ptr: int = 0):
        self._runtime = runtime
        self._ptr = ptr
        self._provider_id = provider_id
        self._provider = provider
        self._owned_mem_ptr = owned_mem_ptr
        self._object_cache_enabled = False
        self._data_lock_depth = 0
        self._sync_required = False
        self._label_cache_enabled = False
        self._label_cache_dirty = False
        self._label_cache: dict[int, dict[int, str]] = {}
        self.Control = Control(runtime, ptr)
        self._cache = [
            {"GlobalDesc": _new_cache_entry(), "GeneralInfo": _new_cache_entry()},
            {name: _new_cache_entry() for name in
             ("GroupDesc", "TimerDesc", "CounterDesc", "ThreadDesc")},
        ]
        runtime._sessions_by_ptr[ptr] = weakref.ref(self)
        self.EnableObjectCache(True)

    def Dispose(self) -> None:
        if not self._ptr:
            return
        ptr = self._ptr
        self._runtime._call("MpCloseSession", ptr)
        self._runtime._sessions_by_ptr.pop(ptr, None)
        if self._provider_id:
            self._runtime._providers.pop(self._provider_id, None)
        if self._owned_mem_ptr:
            self._runtime._call("free", self._owned_mem_ptr)
        self._ptr = 0
        self.Control._ptr = 0  # its commands must not flag the freed session for resync
        self._owned_mem_ptr = 0
        self._provider = None
        self._clear_cache()
        self._label_cache.clear()

    def __enter__(self) -> "Session":
        return self

    def __exit__(self, *_: Any) -> None:
        self.Dispose()

    def __del__(self) -> None:
        _dispose_later(self)

    def SyncWithDataSource(self) -> bool:
        return bool(self._runtime._call("MpSyncWithDataSource", self._ptr))

    def SetSyncRequired(self) -> None:
        self._runtime._call("MpSetSyncRequired", self._ptr)
        self._sync_required = True

    def SetClearCacheOnSyncRequired(self) -> None:
        self._runtime._call("MpSetClearCacheOnSyncRequired", self._ptr)

    def SyncIfRequired(self) -> None:
        if self._sync_required:
            self._runtime._call("MpSyncIfRequired", self._ptr)
            self._sync_required = False

    def IsValid(self) -> bool:
        return bool(self._runtime._call("MpIsSessionValid", self._ptr))

    def GetDataFormatVersion(self) -> int:
        return _u32(self._runtime._call("MpGetSessionDataFormatVersion", self._ptr))

    def GetTrackCount(self) -> int:
        return _u32(self._runtime._call("MpGetTrackCount", self._ptr))

    def GetTracks(self) -> ObjectArray:
        return ObjectArray(self, self._runtime._call(
            "MpGetTracks", self._ptr), "MpGetTrackDescFromArray",
            Types.TrackDesc, False)

    def FetchTracks(self) -> list[Any]:
        return self._fetch_and_dispose(self.GetTracks())

    def GetSegmentCount(self, track_index: int) -> int:
        return _u32(self._runtime._call(
            "MpGetSegmentCount", self._ptr, track_index))

    def GetSegments(self, track_index: int) -> ObjectArray:
        return ObjectArray(self, self._runtime._call(
            "MpGetSegments", self._ptr, track_index),
            "MpGetSegmentDescFromArray", Types.SegmentDesc, False)

    def FetchSegments(self, track_index: int) -> list[Any]:
        return self._fetch_and_dispose(self.GetSegments(track_index))

    def SelectTrack(self, track_index: int) -> bool:
        return bool(self._runtime._call("MpSelectTrack", self._ptr, track_index))

    def SelectSegment(self, segment_index: int) -> bool:
        return bool(self._runtime._call("MpSelectSegment", self._ptr, segment_index))

    def WithDataLock(self, callback: Callable[[], Any]) -> Any:
        if not self._runtime._call("MpPushDataLock", self._ptr):
            return None
        self._data_lock_depth += 1
        try:
            return callback()
        finally:
            self._data_lock_depth -= 1
            self._runtime._call("MpPopDataLock", self._ptr)

    def _get_cached(self, cache: dict[str, dict[Any, Any]], key: Any, api: str,
                    view_type: type, is_table: bool) -> _View | None:
        self.SyncIfRequired()
        if self._object_cache_enabled and key in cache["views"]:
            if self._data_lock_depth:
                self._runtime._call("MpConsumeDataLock", self._ptr)
            return cache["views"][key]
        args = (self._ptr,) if key is None else (self._ptr, key)
        value = self._runtime._wrap(self._runtime._call(api, *args), view_type, is_table)
        if value is not None and self._object_cache_enabled:
            cache["views"][key] = value
        return value

    def _fetch_cached(self, cache: dict[str, dict[Any, Any]], key: Any,
                      getter: Callable[..., _View | None]) -> Any:
        self.SyncIfRequired()
        if self._object_cache_enabled and key in cache["objects"]:
            if self._data_lock_depth:
                self._runtime._call("MpConsumeDataLock", self._ptr)
            return cache["objects"][key]
        view = getter() if key is None else getter(key)
        value = view.UnPack() if view else None
        if value is not None and self._object_cache_enabled:
            cache["objects"][key] = value
        return value

    def _clear_cache_bucket(self, index: int) -> None:
        if 0 <= index < len(self._cache):
            for cache in self._cache[index].values():
                cache["views"].clear()
                cache["objects"].clear()

    def _clear_cache(self) -> None:
        for index in range(len(self._cache)):
            self._clear_cache_bucket(index)

    def EnableObjectCache(self, enabled: bool) -> None:
        self._object_cache_enabled = bool(enabled)
        value = int(enabled)
        self._runtime._call("MpEnableObjectCache", self._ptr, value, value, value, value)
        if not enabled:
            self._clear_cache()

    def ClearObjectCache(self) -> None:
        self._runtime._call("MpClearObjectCache", self._ptr)
        self._clear_cache()

    def _invalidate_label_cache(self, clear_all: bool) -> None:
        self._label_cache_dirty = True
        if clear_all:
            self._label_cache.clear()

    def _refresh_label_cache(self) -> None:
        if not self._label_cache_enabled or not self._label_cache_dirty:
            return
        regions = ObjectArray(self, self._runtime._call(
            "MpInternalGetLabelBufferRegions", self._ptr),
            "MpInternalGetLabelBufferRegionFromArray",
            Types.LabelBufferRegion, False)
        try:
            for chunk_id in list(self._label_cache):
                chunk_start = chunk_id * self._runtime._label_cache_chunk_size
                chunk_end = chunk_start + self._runtime._label_cache_chunk_size
                covered = any(
                    region is not None and
                    region.StartOffset() <= chunk_start and
                    region.EndOffset() >= chunk_end
                    for region in (regions.Get(i) for i in range(regions.Size())))
                if not covered:
                    del self._label_cache[chunk_id]
        finally:
            regions.Dispose()
        self._label_cache_dirty = False

    def _fetch_label(self, offset: int, call: Callable[[int], int]) -> str | None:
        self.SyncIfRequired()
        self._refresh_label_cache()
        chunk_id = offset // self._runtime._label_cache_chunk_size
        if self._label_cache_enabled:
            cached = self._label_cache.get(chunk_id, {}).get(offset)
            if cached is not None:
                return cached

        def read(length_ptr: int) -> str | None:
            struct.pack_into("<I", self._runtime._memory_bytes(), length_ptr, 0)
            ptr = _u32(call(length_ptr))
            if ptr == 0:
                return None
            length = struct.unpack_from(
                "<I", self._runtime._memory_bytes(), length_ptr)[0]
            return bytes(self._runtime._memory_bytes()[ptr:ptr + length]).decode("utf-8")

        value = self._runtime._with_stack(4, read)
        if value is not None and self._label_cache_enabled:
            self._label_cache.setdefault(chunk_id, {})[offset] = value
        return value

    def EnableLabelCache(self, enabled: bool) -> None:
        self._label_cache_enabled = bool(enabled)
        if not enabled:
            self._label_cache.clear()

    def ClearLabelCache(self) -> None:
        self._label_cache.clear()
        self._label_cache_dirty = False

    def _fetch_label_from_offset(self, offset: int) -> str | None:
        if not 0 <= offset <= 0xFFFFFFFFFFFFFFFF:
            raise ValueError("label offset must fit in an unsigned 64-bit integer")
        if offset == 0:
            return None
        return self._fetch_label(offset, lambda length_ptr: self._runtime._call(
            "MpGetLabelFromOffsetParts", self._ptr,
            offset & _U32_MASK, offset >> 32, length_ptr))

    def GetObjSize(self) -> int:
        return _u32(self._runtime._call("MpGetObjSize", self._ptr))

    def GetGlobalDesc(self) -> _View | None:
        return self._get_cached(self._cache[0]["GlobalDesc"], None,
                                "MpGetGlobalDesc", Types.GlobalDesc, False)

    def FetchGlobalDesc(self) -> Any:
        return self._fetch_cached(self._cache[0]["GlobalDesc"], None, self.GetGlobalDesc)

    def GetGeneralInfo(self) -> _View | None:
        return self._get_cached(self._cache[0]["GeneralInfo"], None,
                                "MpGetGeneralInfo", Types.GeneralInfo, True)

    def FetchGeneralInfo(self) -> Any:
        return self._fetch_cached(self._cache[0]["GeneralInfo"], None, self.GetGeneralInfo)

    def GetGroupIdMax(self) -> int:
        return _u32(self._runtime._call("MpGetGroupIdMax", self._ptr))

    def GetGroupDesc(self, group_id: int) -> _View | None:
        return self._get_cached(self._cache[1]["GroupDesc"], group_id,
                                "MpGetGroupDesc", Types.GroupDesc, True)

    def FetchGroupDesc(self, group_id: int) -> Any:
        return self._fetch_cached(self._cache[1]["GroupDesc"], group_id, self.GetGroupDesc)

    def _scalar_array(self, ptr: int) -> ScalarArray:
        return ScalarArray(self, ptr)

    def GetTimerIds(self) -> ScalarArray:
        return self._scalar_array(self._runtime._call("MpGetTimerIds", self._ptr))

    def FetchTimerIds(self) -> list[int]:
        return self._fetch_and_dispose(self.GetTimerIds())

    def GetTimerDesc(self, timer_id: int) -> _View | None:
        return self._get_cached(self._cache[1]["TimerDesc"], timer_id,
                                "MpGetTimerDesc", Types.TimerDesc, True)

    def FetchTimerDesc(self, timer_id: int) -> Any:
        return self._fetch_cached(self._cache[1]["TimerDesc"], timer_id, self.GetTimerDesc)

    def GetCounterIds(self) -> ScalarArray:
        return self._scalar_array(self._runtime._call("MpGetCounterIds", self._ptr))

    def FetchCounterIds(self) -> list[int]:
        return self._fetch_and_dispose(self.GetCounterIds())

    def GetCounterDesc(self, counter_id: int) -> _View | None:
        return self._get_cached(self._cache[1]["CounterDesc"], counter_id,
                                "MpGetCounterDesc", Types.CounterDesc, True)

    def FetchCounterDesc(self, counter_id: int) -> Any:
        return self._fetch_cached(self._cache[1]["CounterDesc"], counter_id,
                                  self.GetCounterDesc)

    def GetCounterSamples(self, counter_id: int) -> CounterSampleArray:
        return CounterSampleArray(self, self._runtime._call(
            "MpGetCounterSamples", self._ptr, counter_id))

    def FetchCounterSamples(self, counter_id: int) -> list[Any]:
        return self._fetch_and_dispose(self.GetCounterSamples(counter_id))

    def GetLastCounterSample(self, counter_id: int) -> _View | None:
        return self._runtime._wrap(self._runtime._call(
            "MpGetLastCounterSample", self._ptr, counter_id), Types.CounterSample, False)

    def FetchLastCounterSample(self, counter_id: int) -> Any:
        value = self.GetLastCounterSample(counter_id)
        return value.UnPack() if value else None

    def GetThreadIds(self) -> ScalarArray:
        return self._scalar_array(self._runtime._call("MpGetThreadIds", self._ptr))

    def FetchThreadIds(self) -> list[int]:
        return self._fetch_and_dispose(self.GetThreadIds())

    def GetThreadDesc(self, thread_id: int) -> _View | None:
        return self._get_cached(self._cache[1]["ThreadDesc"], thread_id,
                                "MpGetThreadDesc", Types.ThreadDesc, True)

    def FetchThreadDesc(self, thread_id: int) -> Any:
        return self._fetch_cached(self._cache[1]["ThreadDesc"], thread_id, self.GetThreadDesc)

    def GetThreadIdGpu(self) -> int:
        return _u32(self._runtime._call("MpGetThreadIdGpu", self._ptr))

    def GetFrameIdMin(self) -> int:
        return _u32(self._runtime._call("MpGetFrameIdMin", self._ptr))

    def GetFrameIdMax(self) -> int:
        return _u32(self._runtime._call("MpGetFrameIdMax", self._ptr))

    def GetFrameDesc(self, frame_id: int) -> _View | None:
        return self._runtime._wrap(self._runtime._call(
            "MpGetFrameDesc", self._ptr, frame_id), Types.FrameDesc, True)

    def FetchFrameDesc(self, frame_id: int) -> Any:
        value = self.GetFrameDesc(frame_id)
        return value.UnPack() if value else None

    def GetPlaceIdSamples(self) -> ObjectArray:
        return ObjectArray(self, self._runtime._call(
            "MpGetPlaceIdSamples", self._ptr), "MpGetU64SampleFromArray",
            Types.U64Sample, False)

    def FetchPlaceIdSamples(self) -> list[Any]:
        return self._fetch_and_dispose(self.GetPlaceIdSamples())

    def GetUtcTimestampSamples(self) -> ObjectArray:
        return ObjectArray(self, self._runtime._call(
            "MpGetUtcTimestampSamples", self._ptr), "MpGetU64SampleFromArray",
            Types.U64Sample, False)

    def FetchUtcTimestampSamples(self) -> list[Any]:
        return self._fetch_and_dispose(self.GetUtcTimestampSamples())

    def CreateLogIterator(self) -> LogIterator:
        return LogIterator(self._runtime, self)

    def CreateCounterIterator(self) -> CounterIterator:
        return CounterIterator(self._runtime, self)

    @staticmethod
    def _fetch_and_dispose(array: ArrayBase) -> list[Any]:
        try:
            return array.FetchAll()
        finally:
            array.Dispose()

    def _call_with_mask(self, api: str, name_mask: str, case_sensitive: bool) -> int:
        data = str(name_mask or "").encode("utf-8") + b"\0"
        return self._runtime._with_stack(len(data), lambda ptr: self._runtime._copy_and_call(
            ptr, data, api, self._ptr, ptr, int(case_sensitive)))

    def FindGroupId(self, mask: str, case_sensitive: bool = False) -> int:
        return _u32(self._call_with_mask("MpFindGroupId", mask, case_sensitive))

    def FindGroupIds(self, mask: str, case_sensitive: bool = False) -> list[int]:
        return self._fetch_and_dispose(self._scalar_array(
            self._call_with_mask("MpFindGroupIds", mask, case_sensitive)))

    def FindTimerId(self, mask: str, case_sensitive: bool = False) -> int:
        return _u32(self._call_with_mask("MpFindTimerId", mask, case_sensitive))

    def FindTimerIds(self, mask: str, case_sensitive: bool = False) -> list[int]:
        return self._fetch_and_dispose(self._scalar_array(
            self._call_with_mask("MpFindTimerIds", mask, case_sensitive)))

    def GetTimerIdsByGroupId(self, group_id: int) -> ScalarArray:
        return self._scalar_array(self._runtime._call(
            "MpGetTimerIdsByGroupId", self._ptr, group_id))

    def FetchTimerIdsByGroupId(self, group_id: int) -> list[int]:
        return self._fetch_and_dispose(self.GetTimerIdsByGroupId(group_id))

    def GetTimerIdsByGroupNames(self, mask: str,
                                case_sensitive: bool = False) -> ScalarArray:
        return self._scalar_array(self._call_with_mask(
            "MpGetTimerIdsByGroupNames", mask, case_sensitive))

    def FetchTimerIdsByGroupNames(self, mask: str,
                                  case_sensitive: bool = False) -> list[int]:
        return self._fetch_and_dispose(
            self.GetTimerIdsByGroupNames(mask, case_sensitive))

    def FindCounterId(self, mask: str, case_sensitive: bool = False) -> int:
        return _u32(self._call_with_mask("MpFindCounterId", mask, case_sensitive))

    def FindCounterIds(self, mask: str, case_sensitive: bool = False) -> list[int]:
        return self._fetch_and_dispose(self._scalar_array(
            self._call_with_mask("MpFindCounterIds", mask, case_sensitive)))

    def FindThreadId(self, mask: str, case_sensitive: bool = False) -> int:
        return _u32(self._call_with_mask("MpFindThreadId", mask, case_sensitive))

    def FindThreadIds(self, mask: str, case_sensitive: bool = False) -> list[int]:
        return self._fetch_and_dispose(self._scalar_array(
            self._call_with_mask("MpFindThreadIds", mask, case_sensitive)))


class Tools:
    def __init__(self, runtime: "LibMPInstance"):
        self._runtime = runtime

    def SetHtmlAssetsBaseUrl(self, base_url: str = "") -> bool:
        data = str(base_url or "").encode("utf-8")

        def call(ptr: int) -> bool:
            memory = self._runtime._memory_bytes()
            memory[ptr:ptr + len(data)] = data
            memory[ptr + len(data)] = 0
            return bool(self._runtime._call(
                "MpToolSetHtmlAssetsBaseUrl", ptr, len(data)))

        return bool(self._runtime._with_stack(len(data) + 1, call))

    def ConvertToBuffer(self, session: Session, format: ContainerFormat) -> bytes | None:
        chunks = bytearray()

        def output(data: memoryview) -> bool:
            chunks.extend(data)
            return True

        return bytes(chunks) if self._runtime._run_tool(session, format, output) else None

    def ConvertToFile(self, session: Session, format: ContainerFormat,
                      output_name: str | Path) -> bool:
        with Path(output_name).open("wb") as output_file:
            return self._runtime._run_tool(
                session, format,
                lambda data: output_file.write(data) == len(data))


class _BoundSession:
    def __init__(self, runtime: "LibMPInstance"):
        self._runtime = runtime

    def OpenFromMem(self, data: Any) -> Session | None:
        return self._runtime.OpenFromMem(data)

    def OpenFromBuffer(self, data: Any, cache_size: int = 0) -> Session | None:
        return self._runtime.OpenFromBuffer(data, cache_size)

    def OpenFromExternalProvider(self, provider: Any, cache_size: int = 0,
                                 use_block_fetch: bool = False) -> Session | None:
        return self._runtime.OpenFromExternalProvider(
            provider, cache_size, use_block_fetch)

    def OpenFromFile(self, path: str | Path, cache_size: int = 0) -> Session | None:
        return self._runtime.OpenFromFile(path, cache_size)

    def OpenFromSlot(self, slot_id: int, cache_size: int = 0) -> Session | None:
        return None


class _ExperimentalV1:
    def StartFileStream(self, filename_postfix: str = "", target_slot_id: int = 0) -> bool:
        return False

    def StartMemoryStream(self, target_slot_id: int) -> bool:
        return False

    def StopStream(self) -> bool:
        return False

    def SetStreamMarker(self, marker: int, enabled: bool) -> bool:
        return False

    def GetStreamStatus(self) -> Any:
        return Types.ExperimentalStreamStatusT()

    def SetContentMode(self, content_mode: ContentMode) -> bool:
        return False


Experimental = SimpleNamespace(V1=_ExperimentalV1())


class LibMPInstance:
    def __init__(self, *, control_message: Callable[..., Any] | None = None,
                 log: Callable[[int, str], None] | None = None,
                 on_error: Callable[[Exception, str], None] | None = None):
        self._control_message = control_message
        self._log = log
        self._on_error = on_error
        self._providers: dict[int, Any] = {}
        self._sessions_by_ptr: dict[int, weakref.ReferenceType[Session]] = {}
        # Objects whose finalizers ran, disposed at the start of the next top-level call
        self._pending_disposals: collections.deque[Any] = collections.deque()
        self._callback_depth = 0
        self._disposing_pending = False
        self._next_provider_id = 1
        self._engine = wasmtime.Engine()
        self._store = wasmtime.Store(self._engine)
        self._linker = wasmtime.Linker(self._engine)
        self._instance = None
        self._memory = None
        self._memory_view: memoryview | None = None
        self._memory_generation = 0
        self._exports: dict[str, Any] = {}
        self._fast_calls: _FastWasmtimeCalls | None = None
        self._tool_output: Callable[[memoryview], bool] | None = None
        self._tool_output_error: Exception | None = None
        self._label_cache_chunk_size = 0
        self._define_imports()
        module = wasmtime.Module(self._engine, base64.b64decode(LIBMP_WASM_BASE64))
        self._instance = self._linker.instantiate(self._store, module)
        exports = self._instance.exports(self._store)
        self._exports = {name: exports[name] for name in exports}
        self._memory = self._exports.get("memory")
        if not isinstance(self._memory, wasmtime.Memory):
            raise RuntimeError("LibMP WASM does not export linear memory")
        self._fast_calls = _FastWasmtimeCalls.create(self._store, self._exports)
        self._refresh_memory()
        self._call("MpInit")
        self._label_cache_chunk_size = _u32(
            self._call("MpInternalGetLabelCacheChunkSize"))
        self.Session = _BoundSession(self)
        self.Control = Control(self)
        self.Control.UseControlChannel(control_message is not None)
        self.Tools = Tools(self)
        self.Enums = Enums
        self.Experimental = Experimental
        self.Versions = SimpleNamespace(
            DataFormat=_u32(self._call("MpGetDataFormatVersion")),
            Library=_u32(self._call("MpGetLibraryVersion")),
        )

    @classmethod
    def Create(cls, **options: Any) -> "LibMPInstance":
        return cls(**options)

    def Dispose(self) -> None:
        if self._callback_depth:
            # The store it closes is still running the call that made the callback
            raise RuntimeError("a LibMP instance cannot be disposed from its own callback")
        self._dispose_pending()  # finalized sessions close their data providers too
        for ref in list(self._sessions_by_ptr.values()):
            session = ref()
            if session is not None:
                session.Dispose()
        self._pending_disposals.clear()
        self._providers.clear()
        self._tool_output = None
        self._tool_output_error = None
        self._fast_calls = None
        self._exports.clear()
        self._memory = None
        self._memory_view = None
        self._memory_generation += 1  # views read nothing from the freed memory
        self._instance = None
        # Wasmtime keeps the import callbacks, and through them this instance, until the
        # linker and store are closed. Closing an already closed object does nothing.
        self._linker.close()
        self._store.close()
        self._engine.close()

    def __enter__(self) -> "LibMPInstance":
        return self

    def __exit__(self, *_: Any) -> None:
        self.Dispose()

    def _call(self, name: str, *args: Any) -> int:
        if self._pending_disposals:
            self._dispose_pending()
        if self._fast_calls is not None:
            result = self._fast_calls.call(name, args)
            if result is not _FAST_CALL_UNAVAILABLE:
                return result
        function = self._exports[name]
        result = function(self._store, *[int(arg) for arg in args])
        return 0 if result is None else int(result)

    def _dispose_pending(self) -> None:
        # A callback runs in the middle of a LibMP call, so it leaves the objects to the
        # top-level call. The objects' own Dispose calls do not start another pass.
        if self._callback_depth or self._disposing_pending:
            return
        self._disposing_pending = True
        try:
            while self._pending_disposals:
                try:
                    self._pending_disposals.popleft().Dispose()
                except Exception:
                    pass
        finally:
            self._disposing_pending = False

    def GetMemUsed(self) -> int:
        return _u32(self._call("MpGetMemUsed"))

    def GetMemReserved(self) -> int:
        return _u32(self._call("MpGetMemReserved"))

    def _refresh_memory(self) -> None:
        # Emscripten calls this after memory.grow. Existing FlatBuffer views keep
        # their offsets and lazily replace only the stale backing memoryview.
        if self._memory is None:
            return
        size = self._memory.data_len(self._store)
        if size == 0:
            self._memory_view = memoryview(bytearray())
        else:
            pointer = self._memory.data_ptr(self._store)
            array = (ctypes.c_ubyte * size).from_address(ctypes.addressof(pointer.contents))
            self._memory_view = memoryview(array).cast("B")
        self._memory_generation += 1

    def _memory_bytes(self) -> memoryview:
        if self._memory is None:
            raise RuntimeError("LibMP instance is disposed")
        if self._memory_view is None:
            self._refresh_memory()
        assert self._memory_view is not None
        return self._memory_view

    def _read_u64_pointer(self, ptr: int) -> int | None:
        if not ptr:
            return None
        value = struct.unpack_from("<Q", self._memory_bytes(), _u32(ptr))[0]
        return value or None

    def _wrap(self, ptr: int, view_type: type, is_table: bool) -> _View | None:
        if not ptr:
            return None
        return _View(self, view_type, _u32(ptr), is_table,
                     getattr(Types, view_type.__name__ + "T", None))

    def _with_stack(self, size: int, callback: Callable[[int], Any]) -> Any:
        stack = self._call("emscripten_stack_get_current")
        ptr = self._call("_emscripten_stack_alloc", size)
        try:
            return callback(ptr)
        finally:
            self._call("_emscripten_stack_restore", stack)

    def _copy_and_call(self, ptr: int, data: bytes, api: str, *args: int) -> int:
        self._memory_bytes()[ptr:ptr + len(data)] = data
        return self._call(api, *args)

    def _configure(self, iterator_ptr: int, config: Any,
                   buffer_api: str, configure_api: str) -> bool:
        builder = flatbuffers.Builder(1024)
        builder.Finish(config.Pack(builder))
        data = bytes(builder.Output())
        destination = self._call(buffer_api, iterator_ptr, len(data))
        if not destination:
            return False
        self._memory_bytes()[destination:destination + len(data)] = data
        return bool(self._call(configure_api, iterator_ptr, 0))

    def OpenFromMem(self, data: Any) -> Session | None:
        payload = bytes(data)
        if len(payload) > 0x7FFFFFFF:
            raise ValueError("capture size exceeds the WASM signed 32-bit address range")
        ptr = self._call("malloc", len(payload))
        if not ptr and payload:
            raise MemoryError("unable to allocate WASM memory for the capture")
        self._memory_bytes()[ptr:ptr + len(payload)] = payload
        session_ptr = self._call("MpOpenFromMemTrusted", ptr, len(payload))
        if not session_ptr:
            self._call("free", ptr)
            return None
        return Session(self, session_ptr, owned_mem_ptr=ptr)

    def OpenFromBuffer(self, data: Any, cache_size: int = 0) -> Session | None:
        payload = memoryview(data).cast("B")

        class BufferProvider:
            def get_size(self) -> int:
                return len(payload)

            def read(self, destination: memoryview, block_id: int,
                     offset: int, size: int) -> int:
                if block_id != 0 or offset > len(payload) or size > len(payload) - offset:
                    return 0
                destination[:size] = payload[offset:offset + size]
                return size

        return self.OpenFromExternalProvider(BufferProvider(), cache_size)

    def OpenFromFile(self, path: str | Path, cache_size: int = 0) -> Session | None:
        class FileProvider:
            def __init__(self, file_path: str | Path):
                self.file = Path(file_path).open("rb", buffering=0)
                self.size = self.file.seek(0, 2)

            def close(self) -> bool:
                if self.file is not None:
                    self.file.close()
                    self.file = None
                self.size = 0
                return True

            def get_size(self) -> int:
                return self.size

            def read(self, destination: memoryview, block_id: int,
                     offset: int, size: int) -> int:
                if self.file is None or block_id != 0 or offset > self.size or \
                        size > self.size - offset:
                    return 0
                self.file.seek(offset)
                bytes_read = 0
                while bytes_read < size:
                    count = self.file.readinto(destination[bytes_read:size])
                    if not count:
                        break
                    bytes_read += count
                return bytes_read

        provider = FileProvider(path)
        session = self.OpenFromExternalProvider(provider, cache_size)
        if session is None:
            provider.close()
        return session

    def OpenFromExternalProvider(self, provider: Any, cache_size: int = 0,
                                 use_block_fetch: bool = False) -> Session | None:
        if _method(provider, "get_size", "GetSize") is None or \
                _method(provider, "read", "Read") is None:
            raise TypeError("external provider must define get_size() and read()")
        if not 0 <= cache_size <= _U32_MASK:
            raise ValueError("cache_size must fit in an unsigned 32-bit integer")
        if self._next_provider_id > 0x7FFFFFFF:
            raise RuntimeError("external data handle space is exhausted")
        provider_id = self._next_provider_id
        self._next_provider_id += 1
        self._providers[provider_id] = provider
        session_ptr = self._call("MpOpenFromExternalDataTrusted", provider_id,
                                 int(use_block_fetch), cache_size)
        if not session_ptr:
            self._providers.pop(provider_id, None)
            return None
        return Session(self, session_ptr, provider_id, provider)

    def _provider_call(self, handle: int, callback: Callable[[Any], int]) -> int:
        provider = self._providers.get(_u32(handle))
        if provider is None:
            return 0
        try:
            return callback(provider)
        except Exception as error:
            self._report_error(error, "external data provider failed")
            return 0

    def _run_tool(self, session: Session, format: ContainerFormat,
                  output: Callable[[memoryview], bool]) -> bool:
        if not isinstance(session, Session) or session._runtime is not self or not session._ptr:
            raise TypeError("expected a live Session owned by this LibMP instance")
        if self._tool_output is not None:
            raise RuntimeError("a LibMP tool operation is already active")
        self._tool_output = output
        self._tool_output_error = None
        try:
            success = bool(self._call("MpToolConvertSession", session._ptr, int(format)))
        finally:
            error = self._tool_output_error
            self._tool_output = None
            self._tool_output_error = None
        if error is not None:
            raise error
        return success

    def _report_error(self, error: Exception, message: str) -> None:
        if self._on_error is not None:
            self._on_error(error, message)
        else:
            print(f"{message}: {error}")

    def _define(self, module: str, name: str, params: list[Any], results: list[Any],
                callback: Callable[..., Any]) -> None:
        def call(*args: Any) -> Any:
            self._callback_depth += 1
            try:
                return callback(*args)
            finally:
                self._callback_depth -= 1

        self._linker.define_func(module, name, wasmtime.FuncType(params, results), call)

    def _define_imports(self) -> None:
        i32 = wasmtime.ValType.i32()
        i64 = wasmtime.ValType.i64()
        self._define(
            "env", "emscripten_notify_memory_growth", [i32], [],
            lambda _index: self._refresh_memory())

        def console_log(level: int, ptr: int, size: int) -> None:
            message = bytes(self._memory_bytes()[ptr:ptr + _u32(size)]).decode(
                "utf-8", errors="replace")
            if self._log is not None:
                self._log(_u32(level), message)
            else:
                print(message)

        def control_message(request_ptr: int, request_size: int,
                            response_ptr: int, response_capacity: int) -> int:
            if self._control_message is None:
                return 0
            try:
                memory = self._memory_bytes()
                request = memory[request_ptr:request_ptr + _u32(request_size)]
                response = memory[response_ptr:response_ptr + _u32(response_capacity)]
                result = self._control_message(request, response)
                if isinstance(result, (bytes, bytearray, memoryview)):
                    data = bytes(result)
                    if len(data) > len(response):
                        return 0
                    response[:len(data)] = data
                    return len(data)
                result = int(result)
                return result if 0 <= result <= len(response) else 0
            except Exception as error:
                self._report_error(error, "control-message callback failed")
                return 0

        def clear_cache(handle: int, bucket_ptr: int, count: int) -> None:
            session_ref = self._sessions_by_ptr.get(_u32(handle))
            session = session_ref() if session_ref else None
            if session is None:
                return
            memory = self._memory_bytes()
            for index in range(_u32(count)):
                session._clear_cache_bucket(struct.unpack_from(
                    "<I", memory, bucket_ptr + index * 4)[0])

        def session_notification(handle: int, notification_flags: int) -> None:
            session_ref = self._sessions_by_ptr.get(_u32(handle))
            session = session_ref() if session_ref else None
            if session is None:
                return
            flags = _u32(notification_flags)
            if flags & _SESSION_NOTIFY_LABEL_CACHE_RESET:
                session._invalidate_label_cache(True)
            elif flags & _SESSION_NOTIFY_LABEL_REGIONS_CHANGED:
                session._invalidate_label_cache(False)

        def tool_output(data_ptr: int, size: int) -> int:
            if self._tool_output is None:
                return 0
            try:
                start = _u32(data_ptr)
                byte_count = _u32(size)
                memory = self._memory_bytes()
                if start > len(memory) or byte_count > len(memory) - start:
                    return 0
                return int(self._tool_output(memory[start:start + byte_count]) is not False)
            except Exception as error:
                self._tool_output_error = error
                return 0

        def data_open(handle: int) -> int:
            return self._provider_call(handle, lambda provider: int(
                (method := _method(provider, "open", "Open")) is None or method() is not False))

        def data_close(handle: int) -> int:
            return self._provider_call(handle, lambda provider: int(
                (method := _method(provider, "close", "Close")) is None or method() is not False))

        def data_size(handle: int, low_ptr: int, high_ptr: int) -> int:
            def call(provider: Any) -> int:
                size = int(_method(provider, "get_size", "GetSize")())
                if not 0 <= size <= 0xFFFFFFFFFFFFFFFF:
                    return 0
                memory = self._memory_bytes()
                struct.pack_into("<I", memory, low_ptr, size & _U32_MASK)
                struct.pack_into("<I", memory, high_ptr, size >> 32)
                return 1
            return self._provider_call(handle, call)

        def data_read(handle: int, destination_ptr: int, block_id: int,
                      offset_low: int, offset_high: int, size: int) -> int:
            def call(provider: Any) -> int:
                byte_count = _u32(size)
                offset = _u32(offset_low) | (_u32(offset_high) << 32)
                destination = self._memory_bytes()[
                    destination_ptr:destination_ptr + byte_count]
                result = _method(provider, "read", "Read")(
                    destination, _u32(block_id), offset, byte_count)
                if isinstance(result, (bytes, bytearray, memoryview)):
                    data = bytes(result)
                    if len(data) > byte_count:
                        return 0
                    destination[:len(data)] = data
                    return len(data)
                result = int(result)
                return result if 0 <= result <= byte_count else 0
            return self._provider_call(handle, call)

        self._define("env", "LibMpExtConsoleLog", [i32, i32, i32], [], console_log)
        self._define("env", "LibMpExtControlMsg", [i32, i32, i32, i32], [i32],
                     control_message)
        self._define("env", "LibMpExtClearObjCacheCallback", [i32, i32, i32], [],
                     clear_cache)
        self._define("env", "LibMpExtSessionNotificationCallback", [i32, i32], [],
                     session_notification)
        self._define("env", "LibMpExtToolOutput", [i32, i32], [i32], tool_output)
        self._define("env", "GprxExtDataOpen", [i32], [i32], data_open)
        self._define("env", "GprxExtDataClose", [i32], [i32], data_close)
        self._define("env", "GprxExtDataGetSize", [i32, i32, i32], [i32], data_size)
        self._define("env", "GprxExtDataRead", [i32] * 6, [i32], data_read)
        self._define("wasi_snapshot_preview1", "fd_write", [i32] * 4, [i32],
                     lambda *_: 0)
        self._define("wasi_snapshot_preview1", "fd_close", [i32], [i32], lambda *_: 0)
        self._define("wasi_snapshot_preview1", "fd_seek", [i32, i64, i32, i32], [i32],
                     lambda *_: 0)


def create(**options: Any) -> LibMPInstance:
    return LibMPInstance(**options)


__all__ = [
    "AppType", "ArrayBase", "ContainerFormat", "ContentMode", "Control", "CounterIterator", "CounterSampleArray",
    "DataSlot", "Enums", "Experimental",
    "FlatBuffers", "LibMPInstance", "LogIterator", "ObjectArray",
    "ScalarArray", "Session", "Tools", "Types", "create",
]
