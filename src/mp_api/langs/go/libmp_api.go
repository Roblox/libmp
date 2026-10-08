// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

// LibMP for Go
// Access Roblox MicroProfiler data from Go applications.
// %LIBMP_SHARED_METADATA%
//
// Requirements:
// - Go 1.18+ and a C compiler for cgo.
// - github.com/bytecodealliance/wasmtime-go/v39 (run go mod tidy).
//
// Minimal usage:
//   library, err := libmp.New()
//   if err != nil { panic(err) }
//   defer library.Dispose()
//   session, err := library.OpenFromFile("capture.x.html") // or "capture.gprx"
//   if err != nil { panic(err) }
//   if session != nil { defer session.Dispose() }

package libmp

import (
	"bytes"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"os"
	"runtime"
	"sync"

	wasmtime "github.com/bytecodealliance/wasmtime-go/v39"
)

type ControlMessageHandler func(request []byte, response []byte) int
type LogHandler func(level uint32, message string)
type ErrorHandler func(message string, err error)

type ContainerFormatValues struct {
	Raw             ContainerFormat
	Compressed      ContainerFormat
	CompressedAscii ContainerFormat
	CompressedHtml  ContainerFormat
}

type ContentModeValues struct {
	Full         ContentMode
	CountersOnly ContentMode
}

type AppTypeValues struct {
	Unknown AppType
	Client  AppType
	Studio  AppType
	Server  AppType
}

type DataSlotValues struct {
	Live             uint32
	FirstUserManaged uint32
	LastUserManaged  uint32
	Temporary        uint32
}

var Enums = struct {
	ContainerFormat ContainerFormatValues
	ContentMode     ContentModeValues
	AppType         AppTypeValues
	DataSlot        DataSlotValues
}{
	ContainerFormat: ContainerFormatValues{
		Raw: ContainerFormatRaw, Compressed: ContainerFormatCompressed,
		CompressedAscii: ContainerFormatCompressedAscii,
		CompressedHtml:  ContainerFormatCompressedHtml,
	},
	ContentMode: ContentModeValues{
		Full: ContentModeFull, CountersOnly: ContentModeCountersOnly,
	},
	AppType: AppTypeValues{
		Unknown: AppTypeUnknown, Client: AppTypeClient,
		Studio: AppTypeStudio, Server: AppTypeServer,
	},
	DataSlot: DataSlotValues{
		Live: uint32(DataSlotLive), FirstUserManaged: uint32(DataSlotFirstUserManaged),
		LastUserManaged: uint32(DataSlotLastUserManaged), Temporary: uint32(DataSlotTemporary),
	},
}

type CaptureParams struct {
	MaxFrameCount   uint32
	ContainerFormat ContainerFormat
}

const (
	sessionNotifyLabelRegionsChanged uint32 = 1 << 0
	sessionNotifyLabelCacheReset     uint32 = 1 << 1
)

type Options struct {
	ControlMessage ControlMessageHandler
	Log            LogHandler
	OnError        ErrorHandler
}

type Versions struct {
	DataFormat uint32
	Library    uint32
}

type DataProvider interface {
	GetSize() uint64
	Read(destination []byte, blockID uint32, offset uint64, size uint32) int
}

type OpenableDataProvider interface{ Open() bool }
type CloseableDataProvider interface{ Close() bool }

// All generated FlatBuffers views share this holder. memory.grow refreshes its
// slice once; individual views then adopt that slice on their next field access.
type memoryRef struct {
	owner      *LibMPInstance
	bytes      []byte
	generation uint64
}

func (m *memoryRef) Bytes() []byte {
	if m == nil {
		return nil
	}
	return m.bytes
}

func (m *memoryRef) refresh() {
	if m == nil || m.owner == nil || m.owner.memory == nil {
		return
	}
	m.bytes = m.owner.memory.UnsafeData(m.owner.store)
	m.generation++
}

var wasmExports = []string{
	"malloc", "free", "emscripten_stack_get_current", "_emscripten_stack_alloc",
	"_emscripten_stack_restore", "MpInit", "MpGetMemUsed", "MpGetMemReserved",
	"MpGetDataFormatVersion", "MpGetLibraryVersion", "MpOpenFromMemTrusted",
	"MpOpenFromExternalDataTrusted", "MpCloseSession", "MpSyncWithDataSource",
	"MpSyncIfRequired", "MpSetSyncRequired", "MpSetClearCacheOnSyncRequired",
	"MpIsSessionValid", "MpGetSessionDataFormatVersion", "MpGetObjSize",
	"MpPushDataLock", "MpConsumeDataLock", "MpPopDataLock", "MpEnableObjectCache", "MpClearObjectCache",
	"MpGetGlobalDesc", "MpGetGeneralInfo", "MpGetGroupIdMax", "MpGetGroupDesc",
	"MpGetTimerIds", "MpGetTimerDesc", "MpGetCounterIds", "MpGetCounterDesc",
	"MpGetCounterSamples", "MpGetLastCounterSample", "MpGetThreadIds",
	"MpGetThreadDesc", "MpGetThreadIdGpu", "MpGetFrameDesc", "MpGetFrameIdMin",
	"MpGetFrameIdMax", "MpGetPlaceIdSamples", "MpGetUtcTimestampSamples",
	"MpGetTrackCount", "MpGetTracks", "MpGetTrackDescFromArray",
	"MpGetSegmentCount", "MpGetSegments", "MpGetSegmentDescFromArray",
	"MpSelectTrack", "MpSelectSegment",
	"MpGetArraySize", "MpReleaseArray", "MpGetIdFromArray", "MpGetU64SampleFromArray",
	"MpGetCounterSampleFromArray", "MpGetCounterSampleFromArrayByFrameId",
	"MpGetLastCounterSampleFromArray", "MpCreateLogIterator", "MpDeleteLogIterator",
	"MpStepLogIterator", "MpRewindLogIterator", "MpRewindLogIteratorTo",
	"MpConfigureLogIterator", "MpGetLogIteratorConfigBuffer", "MpGetLogIteratorState",
	"MpGetLogIteratorThreadStackState", "MpGetLogIteratorThreadStackElement",
	"MpGetLogIteratorCurrentThreadStackElement", "MpGetLogIteratorCurrentEntryCount",
	"MpGetLogIteratorCurrentEntryPart", "MpGetLogIteratorCurrentLabelOffset",
	"MpGetLogIteratorScopeLabelCount", "MpGetLogIteratorCurrentScopeLabelCount",
	"MpGetLogIteratorScopeLabelOffset", "MpGetLogIteratorCurrentScopeLabelOffset",
	"MpGetLabelFromOffsetParts", "MpInternalGetLabelBufferRegions",
	"MpInternalGetLabelBufferRegionFromArray", "MpInternalGetLabelCacheChunkSize",
	"MpCreateCounterIterator",
	"MpDeleteCounterIterator", "MpStepCounterIterator", "MpRewindCounterIterator",
	"MpConfigureCounterIterator", "MpGetCounterIteratorConfigBuffer",
	"MpGetCounterIteratorState", "MpGetCounterIteratorStackElement",
	"MpUseControlChannel", "MpIsBackendVersionCompatible", "MpIsBackendAccessible",
	"MpIsBackendReady", "MpControlEnableProfiler", "MpControlEnableCapture",
	"MpControlShowUI", "MpControlSetFrameLimit", "MpControlSetMemTargetMB",
	"MpGetTimerIdsByGroupId",
	"MpFindGroupId", "MpFindGroupIds", "MpFindTimerId", "MpFindTimerIds",
	"MpGetTimerIdsByGroupNames", "MpFindCounterId", "MpFindCounterIds",
	"MpFindThreadId", "MpFindThreadIds", "MpToolConvertSession",
	"MpToolSetHtmlAssetsBaseUrl",
}

type LibMPInstance struct {
	engine    *wasmtime.Engine
	store     *wasmtime.Store
	linker    *wasmtime.Linker
	instance  *wasmtime.Instance
	memory    *wasmtime.Memory
	functions map[string]*wasmtime.Func
	buffer    memoryRef
	options   Options

	mu             sync.Mutex
	providers      map[int32]DataProvider
	sessionsByPtr  map[int32]*sessionState
	nextProviderID int32
	closed         bool
	toolMu         sync.Mutex
	toolOutput     func([]byte) error
	toolOutputErr  error

	releaseMu       sync.Mutex
	pendingReleases []func()
	callDepth       int

	Control      *Control
	Tools        *Tools
	Versions     Versions
	Experimental ExperimentalAPI
}

type ExperimentalV1 struct{}

func (ExperimentalV1) StartFileStream(filenamePostfix string, targetSlotID ...uint32) bool {
	return false
}
func (ExperimentalV1) StartMemoryStream(targetSlotID uint32) bool { return false }
func (ExperimentalV1) StopStream() bool                           { return false }
func (ExperimentalV1) SetStreamMarker(marker uint32, enabled bool) bool {
	return false
}
func (ExperimentalV1) GetStreamStatus() *ExperimentalStreamStatusT {
	return &ExperimentalStreamStatusT{}
}
func (ExperimentalV1) SetContentMode(contentMode ContentMode) bool { return false }

type ExperimentalAPI struct {
	V1 ExperimentalV1
}

func New(options ...Options) (*LibMPInstance, error) {
	var opts Options
	if len(options) > 1 {
		return nil, errors.New("New accepts at most one Options value")
	}
	if len(options) == 1 {
		opts = options[0]
	}
	w := &LibMPInstance{
		engine:         wasmtime.NewEngine(),
		functions:      make(map[string]*wasmtime.Func),
		providers:      make(map[int32]DataProvider),
		sessionsByPtr:  make(map[int32]*sessionState),
		nextProviderID: 1,
		options:        opts,
	}
	w.buffer.owner = w
	w.store = wasmtime.NewStore(w.engine)
	w.linker = wasmtime.NewLinker(w.engine)
	if err := w.defineImports(); err != nil {
		w.Dispose()
		return nil, err
	}
	wasm, err := base64.StdEncoding.DecodeString(libMPWasmBase64)
	if err != nil {
		w.Dispose()
		return nil, fmt.Errorf("decode embedded LibMP WASM: %w", err)
	}
	module, err := wasmtime.NewModule(w.engine, wasm)
	if err != nil {
		w.Dispose()
		return nil, fmt.Errorf("compile LibMP WASM: %w", err)
	}
	defer module.Close()
	w.instance, err = w.linker.Instantiate(w.store, module)
	if err != nil {
		w.Dispose()
		return nil, fmt.Errorf("instantiate LibMP WASM: %w", err)
	}
	memoryExport := w.instance.GetExport(w.store, "memory")
	if memoryExport == nil || memoryExport.Memory() == nil {
		w.Dispose()
		return nil, errors.New("LibMP WASM does not export linear memory")
	}
	w.memory = memoryExport.Memory()
	w.buffer.refresh()
	for _, name := range wasmExports {
		function := w.instance.GetFunc(w.store, name)
		if function == nil {
			w.Dispose()
			return nil, fmt.Errorf("LibMP WASM export not found: %s", name)
		}
		w.functions[name] = function
	}
	w.call("MpInit")
	w.Control = &Control{runtime: w}
	w.Control.Global = w.Control
	w.Control.UseControlChannel(opts.ControlMessage != nil)
	w.Tools = &Tools{runtime: w}
	w.Versions = Versions{
		DataFormat: uint32(w.call("MpGetDataFormatVersion")),
		Library:    uint32(w.call("MpGetLibraryVersion")),
	}
	// No finalizer: the WASM import callbacks capture w, and wasmtime keeps them reachable
	// until Dispose closes the linker, so an instance that is never disposed is never collected.
	return w, nil
}

func (w *LibMPInstance) call(name string, args ...interface{}) int32 {
	if w.callDepth == 0 {
		w.runPendingReleases()
	}
	w.callDepth++
	defer func() { w.callDepth-- }()
	result, err := w.functions[name].Call(w.store, args...)
	if err != nil {
		panic(fmt.Errorf("LibMP WASM call %s failed: %w", name, err))
	}
	if result == nil {
		return 0
	}
	switch value := result.(type) {
	case int32:
		return value
	case int:
		return int32(value)
	default:
		panic(fmt.Errorf("LibMP WASM call %s returned %T, expected i32", name, result))
	}
}

// queueRelease is what finalizers do instead of calling into WASM, because Go runs them on
// their own goroutine, concurrently with the caller's LibMP calls. The next top-level call
// frees the object on the caller's goroutine; calls made from WASM callbacks leave the
// queue alone.
func (w *LibMPInstance) queueRelease(release func()) {
	w.releaseMu.Lock()
	w.pendingReleases = append(w.pendingReleases, release)
	w.releaseMu.Unlock()
}

func (w *LibMPInstance) queueCall(function string, ptr int32) {
	if ptr != 0 {
		w.queueRelease(func() { w.call(function, ptr) })
	}
}

func (w *LibMPInstance) runPendingReleases() {
	w.releaseMu.Lock()
	releases := w.pendingReleases
	w.pendingReleases = nil
	w.releaseMu.Unlock()
	for _, release := range releases {
		release()
	}
}

func (w *LibMPInstance) GetMemUsed() uint32     { return uint32(w.call("MpGetMemUsed")) }
func (w *LibMPInstance) GetMemReserved() uint32 { return uint32(w.call("MpGetMemReserved")) }

type Tools struct {
	runtime *LibMPInstance
}

func (t *Tools) SetHtmlAssetsBaseUrl(baseURL string) bool {
	data := []byte(baseURL)
	stack := t.runtime.call("emscripten_stack_get_current")
	ptr := t.runtime.call("_emscripten_stack_alloc", int32(len(data)+1))
	defer t.runtime.call("_emscripten_stack_restore", stack)
	copy(t.runtime.buffer.Bytes()[uint32(ptr):], data)
	t.runtime.buffer.Bytes()[uint32(ptr)+uint32(len(data))] = 0
	return t.runtime.call("MpToolSetHtmlAssetsBaseUrl", ptr, int32(len(data))) != 0
}

func (t *Tools) run(session *Session, format ContainerFormat,
	output func([]byte) error) (bool, error) {
	if session == nil || session.runtime != t.runtime || session.ptr == 0 {
		return false, errors.New("expected a live Session owned by this LibMP instance")
	}
	t.runtime.toolMu.Lock()
	defer t.runtime.toolMu.Unlock()
	t.runtime.toolOutput = output
	t.runtime.toolOutputErr = nil
	defer func() {
		t.runtime.toolOutput = nil
		t.runtime.toolOutputErr = nil
	}()
	success := session.call("MpToolConvertSession", session.ptr, int32(format)) != 0
	err := t.runtime.toolOutputErr
	if err != nil {
		return false, err
	}
	return success, nil
}

func (t *Tools) ConvertToBuffer(session *Session, format ContainerFormat) ([]byte, error) {
	var output bytes.Buffer
	success, err := t.run(session, format, func(chunk []byte) error {
		_, writeErr := output.Write(chunk)
		return writeErr
	})
	if err != nil {
		return nil, err
	}
	if !success {
		return nil, errors.New("LibMP container conversion failed")
	}
	return output.Bytes(), nil
}

func (t *Tools) ConvertToFile(
	session *Session, format ContainerFormat, outputName string) error {
	output, err := os.Create(outputName)
	if err != nil {
		return err
	}
	success, convertErr := t.run(session, format, func(chunk []byte) error {
		written, writeErr := output.Write(chunk)
		if writeErr == nil && written != len(chunk) {
			writeErr = io.ErrShortWrite
		}
		return writeErr
	})
	closeErr := output.Close()
	if convertErr != nil {
		return convertErr
	}
	if closeErr != nil {
		return closeErr
	}
	if !success {
		return errors.New("LibMP container conversion failed")
	}
	return nil
}

func (w *LibMPInstance) Dispose() {
	if w == nil || w.closed {
		return
	}
	if w.callDepth != 0 {
		panic("a LibMP instance cannot be disposed from its own callback")
	}
	w.closed = true
	for _, session := range w.sessionsByPtr {
		session.close()
	}
	w.providers = nil
	w.sessionsByPtr = nil
	w.buffer.bytes = nil
	w.buffer.generation++
	if w.linker != nil {
		w.linker.Close()
	}
	if w.store != nil {
		w.store.Close()
	}
	if w.engine != nil {
		w.engine.Close()
	}
}

func wrapStruct[T any](w *LibMPInstance, ptr int32,
	init func(*T, *memoryRef, UOffsetT)) *T {
	if ptr == 0 {
		return nil
	}
	result := new(T)
	init(result, &w.buffer, UOffsetT(uint32(ptr)))
	return result
}

func wrapTable[T any](w *LibMPInstance, ptr int32,
	init func(*T, *memoryRef, UOffsetT)) *T {
	if ptr == 0 {
		return nil
	}
	position := UOffsetT(uint32(ptr))
	position += GetUOffsetT(w.buffer.Bytes()[position:])
	result := new(T)
	init(result, &w.buffer, position)
	return result
}

type Control struct {
	runtime *LibMPInstance
	ptr     int32
	Global  *Control
}

// call passes the session pointer only after earlier releases have run, and holds later ones
// until it returns: a Control can outlive its Session, whose release clears c.ptr.
func (c *Control) call(name string, args ...interface{}) int32 {
	w := c.runtime
	if w.callDepth == 0 {
		w.runPendingReleases()
	}
	w.callDepth++
	defer func() { w.callDepth-- }()
	return w.call(name, append([]interface{}{c.ptr}, args...)...)
}

func (c *Control) EnableProfiler(enable bool) bool {
	return c.call("MpControlEnableProfiler", bool32(enable)) != 0
}
func (c *Control) EnableCapture(enable bool) bool {
	return c.call("MpControlEnableCapture", bool32(enable)) != 0
}
func (c *Control) ShowUI(show bool) bool {
	return c.call("MpControlShowUI", bool32(show)) != 0
}
func (c *Control) SetFrameLimit(count int32) bool {
	return c.call("MpControlSetFrameLimit", count) != 0
}
func (c *Control) SetMemTargetMB(targetMB int32) bool {
	return c.call("MpControlSetMemTargetMB", targetMB) != 0
}
func (c *Control) CaptureToBufferSync(captureParams ...CaptureParams) []byte { return nil }
func (c *Control) CaptureToSlotSync(targetSlotID uint32, captureParams ...CaptureParams) bool {
	return false
}
func (c *Control) SaveSlotToBuffer(slotID uint32) []byte { return nil }
func (c *Control) ResetSlot(slotID uint32) bool          { return false }
func (c *Control) UseControlChannel(use bool) bool {
	return c.runtime.call("MpUseControlChannel", bool32(use)) != 0
}
func (c *Control) IsBackendVersionCompatible() bool {
	return c.runtime.call("MpIsBackendVersionCompatible") != 0
}
func (c *Control) IsBackendAccessible() bool {
	return c.runtime.call("MpIsBackendAccessible") != 0
}
func (c *Control) IsBackendReady() bool { return c.runtime.call("MpIsBackendReady") != 0 }

func bool32(value bool) int32 {
	if value {
		return 1
	}
	return 0
}

type ArrayBase struct {
	runtime *LibMPInstance
	// Element getters take the session; after its Dispose they get 0 and return nil
	session *Session
	ptr     int32
}

func (a *ArrayBase) Dispose() {
	if a != nil && a.ptr != 0 {
		a.runtime.call("MpReleaseArray", a.ptr)
		a.ptr = 0
	}
}
func (a *ArrayBase) Size() int {
	if a == nil || a.ptr == 0 {
		return 0
	}
	return int(uint32(a.call("MpGetArraySize", a.ptr)))
}

// call keeps the array reachable until the WASM call returns. Otherwise Go may finalize the
// array once a method has read its pointer, and the release would run before the call.
func (a *ArrayBase) call(name string, args ...interface{}) int32 {
	defer runtime.KeepAlive(a)
	return a.runtime.call(name, args...)
}
func (a *ArrayBase) finalize() { a.runtime.queueCall("MpReleaseArray", a.ptr) }

type ScalarArray struct{ ArrayBase }

func newScalarArray(s *Session, arrayPtr int32) *ScalarArray {
	result := &ScalarArray{ArrayBase{runtime: s.runtime, session: s, ptr: arrayPtr}}
	runtime.SetFinalizer(result, (*ScalarArray).finalize)
	return result
}
func (a *ScalarArray) Dispose() { runtime.SetFinalizer(a, nil); a.ArrayBase.Dispose() }
func (a *ScalarArray) Get(index int) uint32 {
	return uint32(a.call("MpGetIdFromArray", a.session.ptr, a.ptr, int32(index)))
}
func (a *ScalarArray) Fetch(index int) uint32 { return a.Get(index) }
func (a *ScalarArray) FetchAll() []uint32 {
	result := make([]uint32, a.Size())
	for index := range result {
		result[index] = a.Get(index)
	}
	return result
}

type ObjectArray[V any, T any] struct {
	ArrayBase
	getter string
	init   func(*V, *memoryRef, UOffsetT)
	unpack func(*V) *T
}

func newObjectArray[V any, T any](s *Session, arrayPtr int32, getter string,
	init func(*V, *memoryRef, UOffsetT), unpack func(*V) *T) *ObjectArray[V, T] {
	result := &ObjectArray[V, T]{
		ArrayBase: ArrayBase{runtime: s.runtime, session: s, ptr: arrayPtr},
		getter:    getter, init: init, unpack: unpack,
	}
	runtime.SetFinalizer(result, (*ObjectArray[V, T]).finalize)
	return result
}
func (a *ObjectArray[V, T]) Dispose() { runtime.SetFinalizer(a, nil); a.ArrayBase.Dispose() }
func (a *ObjectArray[V, T]) Get(index int) *V {
	return wrapStruct(a.runtime, a.call(a.getter, a.session.ptr, a.ptr, int32(index)), a.init)
}
func (a *ObjectArray[V, T]) Fetch(index int) *T {
	value := a.Get(index)
	if value == nil {
		return nil
	}
	return a.unpack(value)
}
func (a *ObjectArray[V, T]) FetchAll() []*T {
	result := make([]*T, a.Size())
	for index := range result {
		result[index] = a.Fetch(index)
	}
	return result
}

type CounterSampleArray struct {
	*ObjectArray[CounterSample, CounterSampleT]
}

func newCounterSampleArray(s *Session, arrayPtr int32) *CounterSampleArray {
	return &CounterSampleArray{newObjectArray(s, arrayPtr, "MpGetCounterSampleFromArray",
		(*CounterSample).initShared, (*CounterSample).UnPack)}
}
func (a *CounterSampleArray) GetSampleByFrameId(frameID uint32) *CounterSample {
	return wrapStruct(a.runtime, a.call("MpGetCounterSampleFromArrayByFrameId",
		a.session.ptr, a.ptr, int32(frameID)), (*CounterSample).initShared)
}
func (a *CounterSampleArray) FetchSampleByFrameId(frameID uint32) *CounterSampleT {
	value := a.GetSampleByFrameId(frameID)
	if value == nil {
		return nil
	}
	return value.UnPack()
}
func (a *CounterSampleArray) GetLastSample() *CounterSample {
	return wrapStruct(a.runtime, a.call("MpGetLastCounterSampleFromArray",
		a.session.ptr, a.ptr), (*CounterSample).initShared)
}
func (a *CounterSampleArray) FetchLastSample() *CounterSampleT {
	value := a.GetLastSample()
	if value == nil {
		return nil
	}
	return value.UnPack()
}

type LogIterator struct {
	runtime *LibMPInstance
	ptr     int32
	session *Session
}

func newLogIterator(session *Session) *LogIterator {
	result := &LogIterator{runtime: session.runtime, session: session,
		ptr: session.call("MpCreateLogIterator", session.ptr)}
	runtime.SetFinalizer(result, func(it *LogIterator) {
		it.runtime.queueCall("MpDeleteLogIterator", it.ptr)
	})
	return result
}

// call keeps the iterator reachable until the WASM call returns; see ArrayBase.call.
func (it *LogIterator) call(name string, args ...interface{}) int32 {
	defer runtime.KeepAlive(it)
	return it.runtime.call(name, args...)
}
func (it *LogIterator) Dispose() {
	if it != nil && it.ptr != 0 {
		runtime.SetFinalizer(it, nil)
		it.call("MpDeleteLogIterator", it.ptr)
		it.ptr = 0
	}
}
func (it *LogIterator) Configure(config *LogIteratorConfigT) bool {
	if config == nil {
		config = &LogIteratorConfigT{}
	}
	defer runtime.KeepAlive(it) // configure calls into WASM with the iterator pointer
	return it.runtime.configure(it.ptr, config.Pack,
		"MpGetLogIteratorConfigBuffer", "MpConfigureLogIterator")
}
func (it *LogIterator) Step() bool { return it.call("MpStepLogIterator", it.ptr) != 0 }
func (it *LogIterator) Rewind()    { it.call("MpRewindLogIterator", it.ptr) }
func (it *LogIterator) RewindTo(min, max uint32) {
	it.call("MpRewindLogIteratorTo", it.ptr, int32(min), int32(max))
}
func (it *LogIterator) GetState() *LogIteratorState {
	return wrapStruct(it.runtime, it.call("MpGetLogIteratorState", it.ptr),
		(*LogIteratorState).initShared)
}
func (it *LogIterator) GetThreadStackState(threadID uint32) *LogIteratorThreadStackState {
	return wrapStruct(it.runtime, it.call("MpGetLogIteratorThreadStackState",
		it.ptr, int32(threadID)), (*LogIteratorThreadStackState).initShared)
}
func (it *LogIterator) GetThreadStackElement(threadID uint32, level int32) *LogIteratorThreadStackElement {
	return wrapStruct(it.runtime, it.call("MpGetLogIteratorThreadStackElement",
		it.ptr, int32(threadID), level), (*LogIteratorThreadStackElement).initShared)
}
func (it *LogIterator) GetCurrentThreadStackElement(level int32) *LogIteratorThreadStackElement {
	return wrapStruct(it.runtime, it.call("MpGetLogIteratorCurrentThreadStackElement",
		it.ptr, level), (*LogIteratorThreadStackElement).initShared)
}
func (it *LogIterator) GetCurrentEntryCount() uint32 {
	return uint32(it.call("MpGetLogIteratorCurrentEntryCount", it.ptr))
}
func (it *LogIterator) GetCurrentEntryPart(entryIndex, partIndex uint32) uint32 {
	return uint32(it.call("MpGetLogIteratorCurrentEntryPart",
		it.ptr, int32(entryIndex), int32(partIndex)))
}
func (it *LogIterator) getCurrentLabelOffset() (uint64, bool) {
	return it.runtime.readU64Pointer(
		it.call("MpGetLogIteratorCurrentLabelOffset", it.ptr))
}
func (it *LogIterator) FetchCurrentLabel() (string, bool) {
	offset, ok := it.getCurrentLabelOffset()
	if !ok || it.session == nil {
		return "", false
	}
	return it.session.fetchLabelFromOffset(offset)
}
func (it *LogIterator) GetScopeLabelCount(threadID uint32, stackLevel int32) uint32 {
	return uint32(it.call("MpGetLogIteratorScopeLabelCount",
		it.ptr, int32(threadID), stackLevel))
}
func (it *LogIterator) GetCurrentScopeLabelCount() uint32 {
	return uint32(it.call("MpGetLogIteratorCurrentScopeLabelCount", it.ptr))
}
func (it *LogIterator) getScopeLabelOffset(
	threadID uint32, stackLevel int32, labelIndex uint32) (uint64, bool) {
	return it.runtime.readU64Pointer(it.call("MpGetLogIteratorScopeLabelOffset",
		it.ptr, int32(threadID), stackLevel, int32(labelIndex)))
}
func (it *LogIterator) getCurrentScopeLabelOffset(labelIndex uint32) (uint64, bool) {
	return it.runtime.readU64Pointer(it.call(
		"MpGetLogIteratorCurrentScopeLabelOffset", it.ptr, int32(labelIndex)))
}
func (it *LogIterator) FetchScopeLabel(
	threadID uint32, stackLevel int32, labelIndex uint32) (string, bool) {
	offset, ok := it.getScopeLabelOffset(threadID, stackLevel, labelIndex)
	if !ok || it.session == nil {
		return "", false
	}
	return it.session.fetchLabelFromOffset(offset)
}
func (it *LogIterator) FetchCurrentScopeLabel(labelIndex uint32) (string, bool) {
	offset, ok := it.getCurrentScopeLabelOffset(labelIndex)
	if !ok || it.session == nil {
		return "", false
	}
	return it.session.fetchLabelFromOffset(offset)
}

type CounterIterator struct {
	runtime *LibMPInstance
	ptr     int32
	session *Session // keeps the session alive while the iterator is reachable
}

func newCounterIterator(session *Session) *CounterIterator {
	result := &CounterIterator{runtime: session.runtime, session: session,
		ptr: session.call("MpCreateCounterIterator", session.ptr)}
	runtime.SetFinalizer(result, func(it *CounterIterator) {
		it.runtime.queueCall("MpDeleteCounterIterator", it.ptr)
	})
	return result
}

// call keeps the iterator reachable until the WASM call returns; see ArrayBase.call.
func (it *CounterIterator) call(name string, args ...interface{}) int32 {
	defer runtime.KeepAlive(it)
	return it.runtime.call(name, args...)
}
func (it *CounterIterator) Dispose() {
	if it != nil && it.ptr != 0 {
		runtime.SetFinalizer(it, nil)
		it.call("MpDeleteCounterIterator", it.ptr)
		it.ptr = 0
	}
}
func (it *CounterIterator) Configure(config *CounterIteratorConfigT) bool {
	if config == nil {
		config = &CounterIteratorConfigT{}
	}
	defer runtime.KeepAlive(it) // configure calls into WASM with the iterator pointer
	return it.runtime.configure(it.ptr, config.Pack,
		"MpGetCounterIteratorConfigBuffer", "MpConfigureCounterIterator")
}
func (it *CounterIterator) Step() bool { return it.call("MpStepCounterIterator", it.ptr) != 0 }
func (it *CounterIterator) Rewind()    { it.call("MpRewindCounterIterator", it.ptr) }
func (it *CounterIterator) GetState() *CounterIteratorState {
	return wrapStruct(it.runtime, it.call("MpGetCounterIteratorState", it.ptr),
		(*CounterIteratorState).initShared)
}
func (it *CounterIterator) GetStackElement(level int32) uint32 {
	return uint32(it.call("MpGetCounterIteratorStackElement", it.ptr, level))
}

func (w *LibMPInstance) configure(iterator int32,
	pack func(*Builder) UOffsetT, bufferAPI, configureAPI string) bool {
	builder := NewBuilder(1024)
	builder.Finish(pack(builder))
	data := builder.FinishedBytes()
	destination := w.call(bufferAPI, iterator, int32(len(data)))
	if destination == 0 {
		return false
	}
	copy(w.buffer.Bytes()[uint32(destination):], data)
	return w.call(configureAPI, iterator, 0) != 0
}

type cacheClearer interface{ clear() }
type objectCache[V any, T any] struct {
	views   map[uint32]*V
	objects map[uint32]*T
}

func newObjectCache[V any, T any]() *objectCache[V, T] {
	return &objectCache[V, T]{views: make(map[uint32]*V), objects: make(map[uint32]*T)}
}
func (c *objectCache[V, T]) clear() {
	for key := range c.views {
		delete(c.views, key)
	}
	for key := range c.objects {
		delete(c.objects, key)
	}
}
func getCached[V any, T any](s *Session, cache *objectCache[V, T], key uint32,
	getter func() *V) *V {
	s.SyncIfRequired()
	if s.objectCacheEnabled {
		if value, ok := cache.views[key]; ok {
			if s.dataLockDepth > 0 {
				s.call("MpConsumeDataLock", s.ptr)
			}
			return value
		}
	}
	value := getter()
	if value != nil && s.objectCacheEnabled {
		cache.views[key] = value
	}
	return value
}
func fetchCached[V any, T any](s *Session, cache *objectCache[V, T], key uint32,
	getter func() *V, unpack func(*V) *T) *T {
	s.SyncIfRequired()
	if s.objectCacheEnabled {
		if value, ok := cache.objects[key]; ok {
			if s.dataLockDepth > 0 {
				s.call("MpConsumeDataLock", s.ptr)
			}
			return value
		}
	}
	view := getter()
	if view == nil {
		return nil
	}
	value := unpack(view)
	if s.objectCacheEnabled {
		cache.objects[key] = value
	}
	return value
}

// Session is the handle callers hold. sessionsByPtr holds its state rather than the handle,
// so a session that is never disposed can still be finalized.
type Session struct {
	*sessionState
}

type sessionState struct {
	runtime             *LibMPInstance
	ptr                 int32
	providerID          int32
	provider            DataProvider
	ownedMemPtr         int32
	objectCacheEnabled  bool
	dataLockDepth       int
	syncRequired        bool
	Control             *Control
	cacheBuckets        [][]cacheClearer
	globalDesc          *objectCache[GlobalDesc, GlobalDescT]
	generalInfo         *objectCache[GeneralInfo, GeneralInfoT]
	groupDesc           *objectCache[GroupDesc, GroupDescT]
	timerDesc           *objectCache[TimerDesc, TimerDescT]
	counterDesc         *objectCache[CounterDesc, CounterDescT]
	threadDesc          *objectCache[ThreadDesc, ThreadDescT]
	labelCacheEnabled   bool
	labelCacheDirty     bool
	labelCacheChunkSize uint64
	labelCache          map[uint64]map[uint64]string
}

func newSession(w *LibMPInstance, ptr, providerID int32,
	provider DataProvider, ownedMemPtr int32) *Session {
	s := &Session{&sessionState{runtime: w, ptr: ptr, providerID: providerID,
		provider: provider, ownedMemPtr: ownedMemPtr}}
	s.Control = &Control{runtime: w, ptr: ptr}
	s.globalDesc = newObjectCache[GlobalDesc, GlobalDescT]()
	s.generalInfo = newObjectCache[GeneralInfo, GeneralInfoT]()
	s.groupDesc = newObjectCache[GroupDesc, GroupDescT]()
	s.timerDesc = newObjectCache[TimerDesc, TimerDescT]()
	s.counterDesc = newObjectCache[CounterDesc, CounterDescT]()
	s.threadDesc = newObjectCache[ThreadDesc, ThreadDescT]()
	s.labelCacheChunkSize = uint64(uint32(w.call("MpInternalGetLabelCacheChunkSize")))
	s.labelCache = make(map[uint64]map[uint64]string)
	s.cacheBuckets = [][]cacheClearer{
		{s.globalDesc, s.generalInfo},
		{s.groupDesc, s.timerDesc, s.counterDesc, s.threadDesc},
	}
	w.mu.Lock()
	w.sessionsByPtr[ptr] = s.sessionState
	w.mu.Unlock()
	s.EnableObjectCache(true)
	runtime.SetFinalizer(s, func(s *Session) {
		s.runtime.queueRelease(s.sessionState.close)
	})
	return s
}

// call keeps the session reachable until the WASM call returns; see ArrayBase.call.
func (s *Session) call(name string, args ...interface{}) int32 {
	defer runtime.KeepAlive(s)
	return s.runtime.call(name, args...)
}
func (s *Session) Dispose() {
	if s == nil {
		return
	}
	runtime.SetFinalizer(s, nil)
	s.close()
}

// close does nothing once the session is closed, so the instance's Dispose and a release
// queued by the finalizer can both reach it.
func (s *sessionState) close() {
	ptr := s.ptr
	if ptr == 0 {
		return
	}
	s.ptr = 0
	s.runtime.call("MpCloseSession", ptr)
	s.runtime.mu.Lock()
	delete(s.runtime.sessionsByPtr, ptr)
	if s.providerID != 0 {
		delete(s.runtime.providers, s.providerID)
	}
	s.runtime.mu.Unlock()
	if s.ownedMemPtr != 0 {
		s.runtime.call("free", s.ownedMemPtr)
	}
	s.ownedMemPtr, s.providerID, s.provider = 0, 0, nil
	s.Control.ptr = 0 // its commands must not flag the freed session for resync
	s.clearCache()
	s.clearLabelCache()
}
func (s *Session) SyncWithDataSource() bool {
	return s.call("MpSyncWithDataSource", s.ptr) != 0
}
func (s *Session) SetSyncRequired() {
	s.call("MpSetSyncRequired", s.ptr)
	s.syncRequired = true
}
func (s *Session) SetClearCacheOnSyncRequired() {
	s.call("MpSetClearCacheOnSyncRequired", s.ptr)
}
func (s *Session) SyncIfRequired() {
	if s.syncRequired {
		s.call("MpSyncIfRequired", s.ptr)
		s.syncRequired = false
	}
}
func (s *Session) IsValid() bool { return s.call("MpIsSessionValid", s.ptr) != 0 }
func (s *Session) GetDataFormatVersion() uint32 {
	return uint32(s.call("MpGetSessionDataFormatVersion", s.ptr))
}
func (s *Session) GetTrackCount() uint32 {
	return uint32(s.call("MpGetTrackCount", s.ptr))
}
func (s *Session) GetTracks() *ObjectArray[TrackDesc, TrackDescT] {
	ptr := s.call("MpGetTracks", s.ptr)
	return newObjectArray(s, ptr, "MpGetTrackDescFromArray", (*TrackDesc).initShared, (*TrackDesc).UnPack)
}
func (s *Session) FetchTracks() []*TrackDescT {
	array := s.GetTracks()
	defer array.Dispose()
	return array.FetchAll()
}
func (s *Session) GetSegmentCount(trackIndex uint32) uint32 {
	return uint32(s.call("MpGetSegmentCount", s.ptr, int32(trackIndex)))
}
func (s *Session) GetSegments(trackIndex uint32) *ObjectArray[SegmentDesc, SegmentDescT] {
	ptr := s.call("MpGetSegments", s.ptr, int32(trackIndex))
	return newObjectArray(s, ptr, "MpGetSegmentDescFromArray", (*SegmentDesc).initShared, (*SegmentDesc).UnPack)
}
func (s *Session) FetchSegments(trackIndex uint32) []*SegmentDescT {
	array := s.GetSegments(trackIndex)
	defer array.Dispose()
	return array.FetchAll()
}
func (s *Session) SelectTrack(trackIndex uint32) bool {
	return s.call("MpSelectTrack", s.ptr, int32(trackIndex)) != 0
}
func (s *Session) SelectSegment(segmentIndex uint32) bool {
	return s.call("MpSelectSegment", s.ptr, int32(segmentIndex)) != 0
}
func (s *Session) WithDataLock(callback func()) bool {
	if s.call("MpPushDataLock", s.ptr) == 0 {
		return false
	}
	s.dataLockDepth++
	defer func() {
		s.dataLockDepth--
		s.call("MpPopDataLock", s.ptr)
	}()
	callback()
	return true
}
func (s *sessionState) clearCacheBucket(index uint32) {
	if int(index) >= len(s.cacheBuckets) {
		return
	}
	for _, cache := range s.cacheBuckets[index] {
		cache.clear()
	}
}
func (s *sessionState) clearCache() {
	for index := range s.cacheBuckets {
		s.clearCacheBucket(uint32(index))
	}
}
func (s *Session) EnableObjectCache(enabled bool) {
	s.objectCacheEnabled = enabled
	value := bool32(enabled)
	s.call("MpEnableObjectCache", s.ptr, value, value, value, value)
	if !enabled {
		s.clearCache()
	}
}
func (s *Session) ClearObjectCache() {
	s.call("MpClearObjectCache", s.ptr)
	s.clearCache()
}
func (s *Session) GetObjSize() uint32 { return uint32(s.call("MpGetObjSize", s.ptr)) }
func (s *Session) GetGlobalDesc() *GlobalDesc {
	return getCached(s, s.globalDesc, 0, func() *GlobalDesc {
		return wrapStruct(s.runtime, s.call("MpGetGlobalDesc", s.ptr), (*GlobalDesc).initShared)
	})
}
func (s *Session) FetchGlobalDesc() *GlobalDescT {
	return fetchCached(s, s.globalDesc, 0, s.GetGlobalDesc, (*GlobalDesc).UnPack)
}
func (s *Session) GetGeneralInfo() *GeneralInfo {
	return getCached(s, s.generalInfo, 0, func() *GeneralInfo {
		return wrapTable(s.runtime, s.call("MpGetGeneralInfo", s.ptr), (*GeneralInfo).initShared)
	})
}
func (s *Session) FetchGeneralInfo() *GeneralInfoT {
	return fetchCached(s, s.generalInfo, 0, s.GetGeneralInfo, (*GeneralInfo).UnPack)
}
func (s *Session) GetGroupIdMax() uint32 { return uint32(s.call("MpGetGroupIdMax", s.ptr)) }
func (s *Session) GetGroupDesc(id uint32) *GroupDesc {
	return getCached(s, s.groupDesc, id, func() *GroupDesc {
		return wrapTable(s.runtime, s.call("MpGetGroupDesc", s.ptr, int32(id)), (*GroupDesc).initShared)
	})
}
func (s *Session) FetchGroupDesc(id uint32) *GroupDescT {
	return fetchCached(s, s.groupDesc, id, func() *GroupDesc { return s.GetGroupDesc(id) }, (*GroupDesc).UnPack)
}
func (s *Session) scalarArray(ptr int32) *ScalarArray { return newScalarArray(s, ptr) }
func (s *Session) fetchScalars(array *ScalarArray) []uint32 {
	defer array.Dispose()
	return array.FetchAll()
}
func (s *Session) GetTimerIds() *ScalarArray {
	return s.scalarArray(s.call("MpGetTimerIds", s.ptr))
}
func (s *Session) FetchTimerIds() []uint32 { return s.fetchScalars(s.GetTimerIds()) }
func (s *Session) GetTimerDesc(id uint32) *TimerDesc {
	return getCached(s, s.timerDesc, id, func() *TimerDesc {
		return wrapTable(s.runtime, s.call("MpGetTimerDesc", s.ptr, int32(id)), (*TimerDesc).initShared)
	})
}
func (s *Session) FetchTimerDesc(id uint32) *TimerDescT {
	return fetchCached(s, s.timerDesc, id, func() *TimerDesc { return s.GetTimerDesc(id) }, (*TimerDesc).UnPack)
}
func (s *Session) GetCounterIds() *ScalarArray {
	return s.scalarArray(s.call("MpGetCounterIds", s.ptr))
}
func (s *Session) FetchCounterIds() []uint32 { return s.fetchScalars(s.GetCounterIds()) }
func (s *Session) GetCounterDesc(id uint32) *CounterDesc {
	return getCached(s, s.counterDesc, id, func() *CounterDesc {
		return wrapTable(s.runtime, s.call("MpGetCounterDesc", s.ptr, int32(id)), (*CounterDesc).initShared)
	})
}
func (s *Session) FetchCounterDesc(id uint32) *CounterDescT {
	return fetchCached(s, s.counterDesc, id, func() *CounterDesc { return s.GetCounterDesc(id) }, (*CounterDesc).UnPack)
}
func (s *Session) GetCounterSamples(id uint32) *CounterSampleArray {
	return newCounterSampleArray(s, s.call("MpGetCounterSamples", s.ptr, int32(id)))
}
func (s *Session) FetchCounterSamples(id uint32) []*CounterSampleT {
	array := s.GetCounterSamples(id)
	defer array.Dispose()
	return array.FetchAll()
}
func (s *Session) GetLastCounterSample(id uint32) *CounterSample {
	return wrapStruct(s.runtime, s.call("MpGetLastCounterSample", s.ptr, int32(id)), (*CounterSample).initShared)
}
func (s *Session) FetchLastCounterSample(id uint32) *CounterSampleT {
	value := s.GetLastCounterSample(id)
	if value == nil {
		return nil
	}
	return value.UnPack()
}
func (s *Session) GetThreadIds() *ScalarArray {
	return s.scalarArray(s.call("MpGetThreadIds", s.ptr))
}
func (s *Session) FetchThreadIds() []uint32 { return s.fetchScalars(s.GetThreadIds()) }
func (s *Session) GetThreadDesc(id uint32) *ThreadDesc {
	return getCached(s, s.threadDesc, id, func() *ThreadDesc {
		return wrapTable(s.runtime, s.call("MpGetThreadDesc", s.ptr, int32(id)), (*ThreadDesc).initShared)
	})
}
func (s *Session) FetchThreadDesc(id uint32) *ThreadDescT {
	return fetchCached(s, s.threadDesc, id, func() *ThreadDesc { return s.GetThreadDesc(id) }, (*ThreadDesc).UnPack)
}
func (s *Session) GetThreadIdGpu() uint32 { return uint32(s.call("MpGetThreadIdGpu", s.ptr)) }
func (s *Session) GetFrameIdMin() uint32  { return uint32(s.call("MpGetFrameIdMin", s.ptr)) }
func (s *Session) GetFrameIdMax() uint32  { return uint32(s.call("MpGetFrameIdMax", s.ptr)) }
func (s *Session) GetFrameDesc(id uint32) *FrameDesc {
	return wrapTable(s.runtime, s.call("MpGetFrameDesc", s.ptr, int32(id)), (*FrameDesc).initShared)
}
func (s *Session) FetchFrameDesc(id uint32) *FrameDescT {
	value := s.GetFrameDesc(id)
	if value == nil {
		return nil
	}
	return value.UnPack()
}
func (s *Session) GetPlaceIdSamples() *ObjectArray[U64Sample, U64SampleT] {
	ptr := s.call("MpGetPlaceIdSamples", s.ptr)
	return newObjectArray(s, ptr, "MpGetU64SampleFromArray", (*U64Sample).initShared, (*U64Sample).UnPack)
}
func (s *Session) FetchPlaceIdSamples() []*U64SampleT {
	array := s.GetPlaceIdSamples()
	defer array.Dispose()
	return array.FetchAll()
}
func (s *Session) GetUtcTimestampSamples() *ObjectArray[U64Sample, U64SampleT] {
	ptr := s.call("MpGetUtcTimestampSamples", s.ptr)
	return newObjectArray(s, ptr, "MpGetU64SampleFromArray", (*U64Sample).initShared, (*U64Sample).UnPack)
}
func (s *Session) FetchUtcTimestampSamples() []*U64SampleT {
	array := s.GetUtcTimestampSamples()
	defer array.Dispose()
	return array.FetchAll()
}
func (s *Session) CreateLogIterator() *LogIterator         { return newLogIterator(s) }
func (s *Session) CreateCounterIterator() *CounterIterator { return newCounterIterator(s) }

func (s *Session) callWithMask(api, mask string, caseSensitive bool) int32 {
	data := append([]byte(mask), 0)
	stack := s.call("emscripten_stack_get_current")
	ptr := s.call("_emscripten_stack_alloc", int32(len(data)))
	defer s.call("_emscripten_stack_restore", stack)
	copy(s.runtime.buffer.Bytes()[uint32(ptr):], data)
	return s.call(api, s.ptr, ptr, bool32(caseSensitive))
}
func (s *Session) FindGroupId(mask string, sensitive ...bool) uint32 {
	return uint32(s.callWithMask("MpFindGroupId", mask, optionalBool(sensitive)))
}
func (s *Session) FindGroupIds(mask string, sensitive ...bool) []uint32 {
	return s.fetchScalars(s.scalarArray(s.callWithMask("MpFindGroupIds", mask, optionalBool(sensitive))))
}
func (s *Session) FindTimerId(mask string, sensitive ...bool) uint32 {
	return uint32(s.callWithMask("MpFindTimerId", mask, optionalBool(sensitive)))
}
func (s *Session) FindTimerIds(mask string, sensitive ...bool) []uint32 {
	return s.fetchScalars(s.scalarArray(s.callWithMask("MpFindTimerIds", mask, optionalBool(sensitive))))
}
func (s *Session) GetTimerIdsByGroupId(id uint32) *ScalarArray {
	return s.scalarArray(s.call("MpGetTimerIdsByGroupId", s.ptr, int32(id)))
}
func (s *Session) FetchTimerIdsByGroupId(id uint32) []uint32 {
	return s.fetchScalars(s.GetTimerIdsByGroupId(id))
}
func (s *Session) GetTimerIdsByGroupNames(mask string, sensitive ...bool) *ScalarArray {
	return s.scalarArray(s.callWithMask("MpGetTimerIdsByGroupNames", mask, optionalBool(sensitive)))
}
func (s *Session) FetchTimerIdsByGroupNames(mask string, sensitive ...bool) []uint32 {
	array := s.GetTimerIdsByGroupNames(mask, sensitive...)
	defer array.Dispose()
	return array.FetchAll()
}
func (s *Session) FindCounterId(mask string, sensitive ...bool) uint32 {
	return uint32(s.callWithMask("MpFindCounterId", mask, optionalBool(sensitive)))
}
func (s *Session) FindCounterIds(mask string, sensitive ...bool) []uint32 {
	return s.fetchScalars(s.scalarArray(s.callWithMask("MpFindCounterIds", mask, optionalBool(sensitive))))
}
func (s *Session) FindThreadId(mask string, sensitive ...bool) uint32 {
	return uint32(s.callWithMask("MpFindThreadId", mask, optionalBool(sensitive)))
}
func (s *Session) FindThreadIds(mask string, sensitive ...bool) []uint32 {
	return s.fetchScalars(s.scalarArray(s.callWithMask("MpFindThreadIds", mask, optionalBool(sensitive))))
}

func (s *sessionState) invalidateLabelCache(clearAll bool) {
	s.labelCacheDirty = true
	if clearAll {
		for chunkID := range s.labelCache {
			delete(s.labelCache, chunkID)
		}
	}
}
func (s *Session) refreshLabelCache() {
	if !s.labelCacheEnabled || !s.labelCacheDirty {
		return
	}
	ptr := s.call("MpInternalGetLabelBufferRegions", s.ptr)
	regions := newObjectArray(s, ptr, "MpInternalGetLabelBufferRegionFromArray",
		(*LabelBufferRegion).initShared, (*LabelBufferRegion).UnPack)
	defer regions.Dispose()
	for chunkID := range s.labelCache {
		chunkStart := chunkID * s.labelCacheChunkSize
		chunkEnd := chunkStart + s.labelCacheChunkSize
		covered := false
		for index := 0; index < regions.Size(); index++ {
			region := regions.Get(index)
			if region != nil && region.StartOffset() <= chunkStart && region.EndOffset() >= chunkEnd {
				covered = true
				break
			}
		}
		if !covered {
			delete(s.labelCache, chunkID)
		}
	}
	s.labelCacheDirty = false
}
func (s *Session) fetchLabel(offset uint64, api string, low, high uint32) (string, bool) {
	s.SyncIfRequired()
	s.refreshLabelCache()
	chunkID := offset / s.labelCacheChunkSize
	if s.labelCacheEnabled {
		if chunk := s.labelCache[chunkID]; chunk != nil {
			if value, ok := chunk[offset]; ok {
				return value, true
			}
		}
	}
	stack := s.call("emscripten_stack_get_current")
	lengthPtr := s.call("_emscripten_stack_alloc", 4)
	defer s.call("_emscripten_stack_restore", stack)
	binary.LittleEndian.PutUint32(s.runtime.buffer.Bytes()[uint32(lengthPtr):], 0)
	ptr := s.call(api, s.ptr, int32(low), int32(high), lengthPtr)
	if ptr == 0 {
		return "", false
	}
	length := binary.LittleEndian.Uint32(s.runtime.buffer.Bytes()[uint32(lengthPtr):])
	value := string(s.runtime.buffer.Bytes()[uint32(ptr) : uint32(ptr)+length])
	if s.labelCacheEnabled {
		chunk := s.labelCache[chunkID]
		if chunk == nil {
			chunk = make(map[uint64]string)
			s.labelCache[chunkID] = chunk
		}
		chunk[offset] = value
	}
	return value, true
}
func (s *Session) EnableLabelCache(enabled bool) {
	s.labelCacheEnabled = enabled
	if !enabled {
		for chunkID := range s.labelCache {
			delete(s.labelCache, chunkID)
		}
	}
}
func (s *Session) ClearLabelCache() { s.clearLabelCache() }
func (s *sessionState) clearLabelCache() {
	for chunkID := range s.labelCache {
		delete(s.labelCache, chunkID)
	}
	s.labelCacheDirty = false
}
func (s *Session) fetchLabelFromOffset(offset uint64) (string, bool) {
	if offset == 0 {
		return "", false
	}
	return s.fetchLabel(offset, "MpGetLabelFromOffsetParts", uint32(offset), uint32(offset>>32))
}
func optionalBool(values []bool) bool { return len(values) != 0 && values[0] }

func (w *LibMPInstance) readU64Pointer(ptr int32) (uint64, bool) {
	if ptr == 0 {
		return 0, false
	}
	value := binary.LittleEndian.Uint64(w.buffer.Bytes()[uint32(ptr):])
	return value, value != 0
}

func (w *LibMPInstance) OpenFromMem(data []byte) *Session {
	if uint64(len(data)) > uint64(^uint32(0)>>1) {
		panic("capture size exceeds the WASM signed 32-bit address range")
	}
	ptr := w.call("malloc", int32(len(data)))
	if ptr == 0 && len(data) != 0 {
		panic("unable to allocate WASM memory for the capture")
	}
	copy(w.buffer.Bytes()[uint32(ptr):], data)
	sessionPtr := w.call("MpOpenFromMemTrusted", ptr, int32(len(data)))
	if sessionPtr == 0 {
		w.call("free", ptr)
		return nil
	}
	return newSession(w, sessionPtr, 0, nil, ptr)
}

type bufferProvider struct{ data []byte }

func (p *bufferProvider) GetSize() uint64 { return uint64(len(p.data)) }
func (p *bufferProvider) Read(destination []byte, blockID uint32, offset uint64, size uint32) int {
	if blockID != 0 || offset > uint64(len(p.data)) || uint64(size) > uint64(len(p.data))-offset {
		return 0
	}
	return copy(destination, p.data[offset:offset+uint64(size)])
}

func (w *LibMPInstance) OpenFromBuffer(data []byte, cacheSize ...uint32) *Session {
	return w.OpenFromExternalProvider(&bufferProvider{data: data}, firstUint32(cacheSize), false)
}
func (w *LibMPInstance) OpenFromSlot(slotID uint32, cacheSize ...uint32) *Session { return nil }

type fileProvider struct {
	file *os.File
	size uint64
}

func (p *fileProvider) Close() bool {
	if p.file == nil {
		return true
	}
	err := p.file.Close()
	p.file = nil
	p.size = 0
	return err == nil
}
func (p *fileProvider) GetSize() uint64 { return p.size }
func (p *fileProvider) Read(destination []byte, blockID uint32, offset uint64, size uint32) int {
	if p.file == nil || blockID != 0 || offset > p.size || uint64(size) > p.size-offset {
		return 0
	}
	read, _ := p.file.ReadAt(destination[:int(size)], int64(offset))
	return read
}

func (w *LibMPInstance) OpenFromFile(path string, cacheSize ...uint32) (*Session, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	info, err := file.Stat()
	if err != nil {
		file.Close()
		return nil, err
	}
	provider := &fileProvider{file: file, size: uint64(info.Size())}
	session := w.OpenFromExternalProvider(provider, firstUint32(cacheSize), false)
	if session == nil {
		provider.Close()
	}
	return session, nil
}
func (w *LibMPInstance) OpenFromExternalProvider(provider DataProvider,
	cacheSize uint32, useBlockFetch bool) *Session {
	w.mu.Lock()
	if w.nextProviderID <= 0 {
		w.mu.Unlock()
		panic("external data handle space is exhausted")
	}
	id := w.nextProviderID
	w.nextProviderID++
	w.providers[id] = provider
	w.mu.Unlock()
	ptr := w.call("MpOpenFromExternalDataTrusted", id, bool32(useBlockFetch), int32(cacheSize))
	if ptr == 0 {
		w.mu.Lock()
		delete(w.providers, id)
		w.mu.Unlock()
		return nil
	}
	return newSession(w, ptr, id, provider, 0)
}
func firstUint32(values []uint32) uint32 {
	if len(values) == 0 {
		return 0
	}
	return values[0]
}

func (w *LibMPInstance) provider(handle int32) DataProvider {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.providers[handle]
}
func (w *LibMPInstance) report(message string, err error) {
	if w.options.OnError != nil {
		w.options.OnError(message, err)
	}
}
func (w *LibMPInstance) define(module, name string, callback interface{}) error {
	if err := w.linker.FuncWrap(module, name, callback); err != nil {
		return fmt.Errorf("define WASM import %s.%s: %w", module, name, err)
	}
	return nil
}
func (w *LibMPInstance) defineImports() error {
	definitions := []struct {
		module, name string
		callback     interface{}
	}{
		{"env", "emscripten_notify_memory_growth", func(_ int32) { w.buffer.refresh() }},
		{"env", "LibMpExtConsoleLog", func(level, ptr, size int32) {
			message := string(w.buffer.Bytes()[uint32(ptr) : uint32(ptr)+uint32(size)])
			if w.options.Log != nil {
				w.options.Log(uint32(level), message)
			}
		}},
		{"env", "LibMpExtControlMsg", func(requestPtr, requestSize, responsePtr, responseCapacity int32) int32 {
			if w.options.ControlMessage == nil {
				return 0
			}
			memory := w.buffer.Bytes()
			request := memory[uint32(requestPtr) : uint32(requestPtr)+uint32(requestSize)]
			response := memory[uint32(responsePtr) : uint32(responsePtr)+uint32(responseCapacity)]
			result := w.options.ControlMessage(request, response)
			if result < 0 || result > len(response) {
				return 0
			}
			return int32(result)
		}},
		{"env", "LibMpExtClearObjCacheCallback", func(handle, bucketPtr, count int32) {
			w.mu.Lock()
			session := w.sessionsByPtr[handle]
			w.mu.Unlock()
			if session == nil {
				return
			}
			memory := w.buffer.Bytes()
			for index := uint32(0); index < uint32(count); index++ {
				session.clearCacheBucket(binary.LittleEndian.Uint32(memory[uint32(bucketPtr)+index*4:]))
			}
		}},
		{"env", "LibMpExtSessionNotificationCallback", func(handle, flags int32) {
			w.mu.Lock()
			session := w.sessionsByPtr[handle]
			w.mu.Unlock()
			if session == nil {
				return
			}
			value := uint32(flags)
			if value&sessionNotifyLabelCacheReset != 0 {
				session.invalidateLabelCache(true)
			} else if value&sessionNotifyLabelRegionsChanged != 0 {
				session.invalidateLabelCache(false)
			}
		}},
		{"env", "LibMpExtToolOutput", func(dataPtr, size int32) (result int32) {
			if w.toolOutput == nil || dataPtr < 0 || size < 0 {
				return 0
			}
			start, byteCount := uint32(dataPtr), uint32(size)
			memory := w.buffer.Bytes()
			if start > uint32(len(memory)) || byteCount > uint32(len(memory))-start {
				return 0
			}
			defer func() {
				if recovered := recover(); recovered != nil {
					w.toolOutputErr = fmt.Errorf("tool output callback failed: %v", recovered)
					result = 0
				}
			}()
			if err := w.toolOutput(memory[start : start+byteCount]); err != nil {
				w.toolOutputErr = err
				return 0
			}
			return 1
		}},
		{"env", "GprxExtDataOpen", func(handle int32) int32 {
			provider := w.provider(handle)
			if provider == nil {
				return 0
			}
			if openable, ok := provider.(OpenableDataProvider); ok && !openable.Open() {
				return 0
			}
			return 1
		}},
		{"env", "GprxExtDataClose", func(handle int32) int32 {
			provider := w.provider(handle)
			if provider == nil {
				return 0
			}
			if closeable, ok := provider.(CloseableDataProvider); ok && !closeable.Close() {
				return 0
			}
			return 1
		}},
		{"env", "GprxExtDataGetSize", func(handle, lowPtr, highPtr int32) int32 {
			provider := w.provider(handle)
			if provider == nil {
				return 0
			}
			size := provider.GetSize()
			memory := w.buffer.Bytes()
			binary.LittleEndian.PutUint32(memory[uint32(lowPtr):], uint32(size))
			binary.LittleEndian.PutUint32(memory[uint32(highPtr):], uint32(size>>32))
			return 1
		}},
		{"env", "GprxExtDataRead", func(handle, destination, blockID, offsetLow, offsetHigh, size int32) int32 {
			provider := w.provider(handle)
			if provider == nil || size < 0 {
				return 0
			}
			offset := uint64(uint32(offsetLow)) | uint64(uint32(offsetHigh))<<32
			memory := w.buffer.Bytes()
			target := memory[uint32(destination) : uint32(destination)+uint32(size)]
			read := provider.Read(target, uint32(blockID), offset, uint32(size))
			if read < 0 || read > len(target) {
				return 0
			}
			return int32(read)
		}},
		{"wasi_snapshot_preview1", "fd_write", func(_, _, _, _ int32) int32 { return 0 }},
		{"wasi_snapshot_preview1", "fd_close", func(_ int32) int32 { return 0 }},
		{"wasi_snapshot_preview1", "fd_seek", func(_ int32, _ int64, _, _ int32) int32 { return 0 }},
	}
	for _, definition := range definitions {
		if err := w.define(definition.module, definition.name, definition.callback); err != nil {
			return err
		}
	}
	return nil
}
