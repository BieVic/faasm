#include <conf/FaasmConfig.h>
#include <threads/ThreadState.h>
#include <wasm/WasmExecutionContext.h>
#include <wasm/WasmModule.h>

#include <faabric/scheduler/Scheduler.h>
#include <faabric/snapshot/SnapshotRegistry.h>
#include <faabric/util/bytes.h>
#include <faabric/util/config.h>
#include <faabric/util/environment.h>
#include <faabric/util/func.h>
#include <faabric/util/gids.h>
#include <faabric/util/locks.h>
#include <faabric/util/logging.h>
#include <faabric/util/memory.h>
#include <faabric/util/snapshot.h>
#include <faabric/util/timing.h>

#include <boost/filesystem.hpp>
#include <sstream>
#include <sys/mman.h>
#include <sys/uio.h>

namespace wasm {

bool isWasmPageAligned(int32_t offset)
{
    if (offset & (WASM_BYTES_PER_PAGE - 1)) {
        return false;
    } else {
        return true;
    }
}

size_t getNumberOfWasmPagesForBytes(uint32_t nBytes)
{
    // Round up to nearest page
    size_t pageCount =
      (size_t(nBytes) + WASM_BYTES_PER_PAGE - 1) / WASM_BYTES_PER_PAGE;

    return pageCount;
}

uint32_t roundUpToWasmPageAligned(uint32_t nBytes)
{
    size_t nPages = getNumberOfWasmPagesForBytes(nBytes);
    return (uint32_t)(nPages * WASM_BYTES_PER_PAGE);
}

size_t getPagesForGuardRegion()
{
    size_t regionSize = GUARD_REGION_SIZE;
    size_t nWasmPages = getNumberOfWasmPagesForBytes(regionSize);
    return nWasmPages;
}

WasmModule::WasmModule()
  : WasmModule(faabric::util::getUsableCores())
{}

WasmModule::WasmModule(int threadPoolSizeIn)
  : threadPoolSize(threadPoolSizeIn)
{}

WasmModule::~WasmModule() {}

void WasmModule::flush() {}

storage::FileSystem& WasmModule::getFileSystem()
{
    return filesystem;
}

wasm::WasmEnvironment& WasmModule::getWasmEnvironment()
{
    return wasmEnvironment;
}

std::shared_ptr<faabric::util::SnapshotData> WasmModule::getSnapshotData()
{
    // Note - we only want to take the snapshot to the current brk, not the top
    // of the allocated memory
    uint8_t* memBase = getMemoryBase();
    size_t currentSize = currentBrk.load(std::memory_order_acquire);
    size_t maxSize = MAX_WASM_MEM;

    // Create the snapshot
    auto snap = std::make_shared<faabric::util::SnapshotData>(
      std::span<const uint8_t>(memBase, currentSize), maxSize);

    return snap;
}

faabric::util::MemoryView WasmModule::getMemoryView()
{
    uint8_t* memBase = getMemoryBase();
    size_t currentSize = currentBrk.load(std::memory_order_acquire);
    return faabric::util::MemoryView({ memBase, currentSize });
}

static std::string getAppSnapshotKey(const faabric::Message& msg)
{
    std::string funcStr = faabric::util::funcToString(msg, false);
    if (msg.appid() == 0) {
        SPDLOG_ERROR("Cannot create app snapshot key without app ID for {}",
                     funcStr);
        throw std::runtime_error(
          "Cannot create app snapshot key without app ID");
    }

    std::string snapshotKey = funcStr + "_" + std::to_string(msg.appid());
    return snapshotKey;
}

std::string WasmModule::getOrCreateAppSnapshot(const faabric::Message& msg,
                                               bool update)
{
    std::string snapshotKey = getAppSnapshotKey(msg);

    faabric::snapshot::SnapshotRegistry& reg =
      faabric::snapshot::getSnapshotRegistry();

    bool exists = reg.snapshotExists(snapshotKey);
    if (!exists) {
        faabric::util::FullLock lock(moduleMutex);
        exists = reg.snapshotExists(snapshotKey);

        if (!exists) {
            SPDLOG_DEBUG(
              "Creating app snapshot: {} for app {}", snapshotKey, msg.appid());
            snapshotWithKey(snapshotKey);

            return snapshotKey;
        }
    }

    if (update) {
        faabric::util::FullLock lock(moduleMutex);

        SPDLOG_DEBUG(
          "Updating app snapshot: {} for app {}", snapshotKey, msg.appid());

        std::vector<faabric::util::SnapshotDiff> updates =
          getMemoryView().getDirtyRegions();

        std::shared_ptr<faabric::util::SnapshotData> snap =
          reg.getSnapshot(snapshotKey);

        snap->queueDiffs(updates);
        snap->writeQueuedDiffs();

        // Reset dirty tracking
        faabric::util::resetDirtyTracking();
    }

    {
        faabric::util::SharedLock lock(moduleMutex);
        return snapshotKey;
    }
}

void WasmModule::deleteAppSnapshot(const faabric::Message& msg)
{
    std::string snapshotKey = getAppSnapshotKey(msg);

    faabric::snapshot::SnapshotRegistry& reg =
      faabric::snapshot::getSnapshotRegistry();

    if (reg.snapshotExists(snapshotKey)) {
        // Broadcast the deletion
        faabric::scheduler::Scheduler& sch = faabric::scheduler::getScheduler();
        sch.broadcastSnapshotDelete(msg, snapshotKey);

        // Delete locally
        reg.deleteSnapshot(snapshotKey);
    }
}

void WasmModule::snapshotWithKey(const std::string& snapKey)

{
    PROF_START(wasmSnapshot)
    std::shared_ptr<faabric::util::SnapshotData> data = getSnapshotData();

    faabric::snapshot::SnapshotRegistry& reg =
      faabric::snapshot::getSnapshotRegistry();
    reg.registerSnapshot(snapKey, data);

    PROF_END(wasmSnapshot)
}

std::string WasmModule::snapshot(bool locallyRestorable)
{
    uint32_t gid = faabric::util::generateGid();
    std::string snapKey =
      this->boundUser + "_" + this->boundFunction + "_" + std::to_string(gid);

    snapshotWithKey(snapKey);

    return snapKey;
}

void WasmModule::syncAppSnapshot(const faabric::Message& msg)
{
    faabric::snapshot::SnapshotRegistry& reg =
      faabric::snapshot::getSnapshotRegistry();
    std::string snapshotKey = getAppSnapshotKey(msg);

    SPDLOG_DEBUG("{} syncing with snapshot {}",
                 faabric::util::funcToString(msg, false),
                 snapshotKey);

    std::shared_ptr<faabric::util::SnapshotData> snap =
      reg.getSnapshot(snapshotKey);

    // Update the snapshot itself
    snap->writeQueuedDiffs();

    // Restore from snapshot
    restore(snapshotKey);
}

void WasmModule::restore(const std::string& snapshotKey)
{
    if (!isBound()) {
        SPDLOG_ERROR("Must bind wasm module before restoring snapshot {}",
                     snapshotKey);
        throw std::runtime_error("Cannot restore unbound wasm module");
    }

    faabric::snapshot::SnapshotRegistry& reg =
      faabric::snapshot::getSnapshotRegistry();

    // Expand memory if necessary
    auto data = reg.getSnapshot(snapshotKey);
    uint32_t memSize = getCurrentBrk();

    if (data->getSize() > memSize) {
        size_t bytesRequired = data->getSize() - memSize;
        SPDLOG_DEBUG("Growing memory by {} bytes to restore snapshot",
                     bytesRequired);
        this->growMemory(bytesRequired);
    } else if (data->getSize() < memSize) {
        size_t shrinkBy = memSize - data->getSize();
        SPDLOG_DEBUG("Shrinking memory by {} bytes to restore snapshot",
                     shrinkBy);
        this->shrinkMemory(shrinkBy);
    } else {
        SPDLOG_DEBUG("Memory already correct size for snapshot ({})", memSize);
    }

    // Map the snapshot into memory
    uint8_t* memoryBase = getMemoryBase();
    data->mapToMemory({ memoryBase, data->getSize() });
}

void WasmModule::ignoreThreadStacksInSnapshot(const std::string& snapKey)
{
    std::shared_ptr<faabric::util::SnapshotData> snap =
      faabric::snapshot::getSnapshotRegistry().getSnapshot(snapKey);

    // Stacks grow downwards and snapshot diffs are inclusive, so we need to
    // start the diff on the byte at the bottom of the stacks region
    uint32_t threadStackRegionStart =
      threadStacks.at(0) - (THREAD_STACK_SIZE - 1) - GUARD_REGION_SIZE;
    uint32_t threadStackRegionSize =
      threadPoolSize * (THREAD_STACK_SIZE + (2 * GUARD_REGION_SIZE));

    SPDLOG_TRACE("Ignoring snapshot diffs for {} for thread stacks: {}-{}",
                 snapKey,
                 threadStackRegionStart,
                 threadStackRegionStart + threadStackRegionSize);

    // Note - the merge regions for a snapshot are keyed on the offset, so
    // we will just overwrite the same region if another module has already
    // set it
    snap->addMergeRegion(threadStackRegionStart,
                         threadStackRegionSize,
                         faabric::util::SnapshotDataType::Raw,
                         faabric::util::SnapshotMergeOperation::Ignore,
                         true);
}

std::string WasmModule::getBoundUser()
{
    return boundUser;
}

std::string WasmModule::getBoundFunction()
{
    return boundFunction;
}

int WasmModule::getStdoutFd()
{
    if (stdoutMemFd == 0) {
        stdoutMemFd = memfd_create("stdoutfd", 0);
        SPDLOG_DEBUG("Capturing stdout: fd={}", stdoutMemFd);
    }

    return stdoutMemFd;
}

ssize_t WasmModule::captureStdout(const struct ::iovec* iovecs, int iovecCount)
{
    int memFd = getStdoutFd();
    ssize_t writtenSize = ::writev(memFd, iovecs, iovecCount);

    if (writtenSize < 0) {
        SPDLOG_ERROR("Failed capturing stdout: {}", strerror(errno));
        throw std::runtime_error(std::string("Failed capturing stdout: ") +
                                 strerror(errno));
    }

    SPDLOG_DEBUG("Captured {} bytes of formatted stdout", writtenSize);
    stdoutSize += writtenSize;
    return writtenSize;
}

ssize_t WasmModule::captureStdout(const void* buffer)
{
    int memFd = getStdoutFd();

    ssize_t writtenSize =
      dprintf(memFd, "%s\n", reinterpret_cast<const char*>(buffer));

    if (writtenSize < 0) {
        SPDLOG_ERROR("Failed capturing stdout: {}", strerror(errno));
        throw std::runtime_error("Failed capturing stdout");
    }

    SPDLOG_DEBUG("Captured {} bytes of unformatted stdout", writtenSize);
    stdoutSize += writtenSize;
    return writtenSize;
}

std::string WasmModule::getCapturedStdout()
{
    if (stdoutSize == 0) {
        return "";
    }

    // Go back to start
    int memFd = getStdoutFd();
    lseek(memFd, 0, SEEK_SET);

    // Read in and return
    std::string stdoutString(stdoutSize, '\0');
    read(memFd, stdoutString.data(), stdoutSize);
    SPDLOG_DEBUG("Read stdout length {}:\n{}", stdoutSize, stdoutString);

    return stdoutString;
}

void WasmModule::clearCapturedStdout()
{
    close(stdoutMemFd);
    stdoutMemFd = 0;
    stdoutSize = 0;
}

uint32_t WasmModule::getArgc()
{
    return argc;
}

uint32_t WasmModule::getArgvBufferSize()
{
    return argvBufferSize;
}

void WasmModule::bindToFunction(faabric::Message& msg, bool cache)
{
    if (_isBound) {
        throw std::runtime_error("Cannot bind a module twice");
    }

    _isBound = true;
    boundUser = msg.user();
    boundFunction = msg.function();

    // Call into subclass hook, setting the context beforehand
    WasmExecutionContext ctx(this, &msg);
    doBindToFunction(msg, cache);
}

void WasmModule::prepareArgcArgv(const faabric::Message& msg)
{
    // Here we set up the arguments to main(), i.e. argc and argv
    // We allow passing of arbitrary commandline arguments via the
    // invocation message. These are passed as a string with a space
    // separating each argument.
    argv = faabric::util::getArgvForMessage(msg);
    argc = argv.size();

    // Work out the size of the buffer to hold the strings (allowing
    // for null terminators)
    argvBufferSize = 0;
    for (const auto& thisArg : argv) {
        argvBufferSize += thisArg.size() + 1;
    }
}

/**
 * Maps the given state into the module's memory.
 *
 * If we are dealing with a chunk of a larger state value, the host memory
 * will be reserved for the full value, but only the necessary wasm pages
 * will be created. Loading many chunks of the same value leads to
 * fragmentation, but usually only one or two chunks are loaded per module.
 *
 * To perform the mapping we need to ensure allocated memory is
 * page-aligned.
 */
uint32_t WasmModule::mapSharedStateMemory(
  const std::shared_ptr<faabric::state::StateKeyValue>& kv,
  long offset,
  uint32_t length)
{
    // See if we already have this segment mapped into memory
    std::string segmentKey = kv->user + "_" + kv->key + "__" +
                             std::to_string(offset) + "__" +
                             std::to_string(length);
    if (sharedMemWasmPtrs.count(segmentKey) == 0) {
        // Lock and double check
        faabric::util::FullLock lock(sharedMemWasmPtrsMutex);

        if (sharedMemWasmPtrs.count(segmentKey) == 0) {
            // Page-align the chunk
            faabric::util::AlignedChunk chunk =
              faabric::util::getPageAlignedChunk(offset, length);

            // Create the wasm memory region and work out the offset to the
            // start of the desired chunk in this region (this will be zero
            // if the offset is already zero, or if the offset is
            // page-aligned already). We need to round the allocation up to
            // a wasm page boundary
            uint32_t allocSize = roundUpToWasmPageAligned(chunk.nBytesLength);
            uint32_t wasmBasePtr = this->growMemory(allocSize);
            uint32_t wasmOffsetPtr = wasmBasePtr + chunk.offsetRemainder;

            // Map the shared memory
            uint8_t* wasmMemoryRegionPtr = wasmPointerToNative(wasmBasePtr);
            kv->mapSharedMemory(static_cast<void*>(wasmMemoryRegionPtr),
                                chunk.nPagesOffset,
                                chunk.nPagesLength);

            // Cache the wasm pointer
            sharedMemWasmPtrs[segmentKey] = wasmOffsetPtr;
        }
    }

    // Return the wasm pointer
    {
        faabric::util::SharedLock lock(sharedMemWasmPtrsMutex);
        return sharedMemWasmPtrs[segmentKey];
    }
}

uint32_t WasmModule::getCurrentBrk()
{
    return currentBrk.load(std::memory_order_acquire);
}

int32_t WasmModule::executeTask(
  int threadPoolIdx,
  int msgIdx,
  std::shared_ptr<faabric::BatchExecuteRequest> req)
{
    faabric::Message& msg = req->mutable_messages()->at(msgIdx);
    std::string funcStr = faabric::util::funcToString(msg, true);

    if (!isBound()) {
        throw std::runtime_error(
          "WasmModule must be bound before executing anything");
    }

    assert(boundUser == msg.user());
    assert(boundFunction == msg.function());

    // Set up context for this task
    WasmExecutionContext ctx(this, &msg);

    // Modules must have provisioned their own thread stacks
    assert(!threadStacks.empty());
    uint32_t stackTop = threadStacks.at(threadPoolIdx);

    // Ignore stacks and guard pages in snapshot if present
    if (!msg.snapshotkey().empty()) {
        ignoreThreadStacksInSnapshot(msg.snapshotkey());
    }

    // Perform the appropriate type of execution
    int returnValue;
    if (req->type() == faabric::BatchExecuteRequest::THREADS) {
        switch (req->subtype()) {
            case ThreadRequestType::PTHREAD: {
                SPDLOG_TRACE("Executing {} as pthread", funcStr);
                returnValue = executePthread(threadPoolIdx, stackTop, msg);
                break;
            }
            case ThreadRequestType::OPENMP: {
                SPDLOG_TRACE("Executing {} as OpenMP (group {}, size {})",
                             funcStr,
                             msg.groupid(),
                             msg.groupsize());

                // Set up the level
                threads::setCurrentOpenMPLevel(req);
                returnValue = executeOMPThread(threadPoolIdx, stackTop, msg);
                break;
            }
            default: {
                SPDLOG_ERROR("{} has unrecognised thread subtype {}",
                             funcStr,
                             req->subtype());
                throw std::runtime_error("Unrecognised thread subtype");
            }
        }
    } else {
        // Vanilla function
        SPDLOG_TRACE("Executing {} as standard function", funcStr);
        returnValue = executeFunction(msg);

        deleteAppSnapshot(msg);
    }

    if (returnValue != 0) {
        msg.set_outputdata(
          fmt::format("Call failed (return value={})", returnValue));
    }

    // Add captured stdout if necessary
    conf::FaasmConfig& conf = conf::getFaasmConfig();
    if (conf.captureStdout == "on") {
        std::string moduleStdout = getCapturedStdout();
        if (!moduleStdout.empty()) {
            std::string newOutput = moduleStdout + "\n" + msg.outputdata();
            msg.set_outputdata(newOutput);

            clearCapturedStdout();
        }
    }

    return returnValue;
}

uint32_t WasmModule::createMemoryGuardRegion(uint32_t wasmOffset)
{

    uint32_t regionSize = GUARD_REGION_SIZE;
    uint8_t* nativePtr = wasmPointerToNative(wasmOffset);

    // NOTE: we want to protect these regions from _writes_, but we don't
    // want to stop them being read, otherwise snapshotting will fail.
    // Therefore we make them read-only
    int res = mprotect(nativePtr, regionSize, PROT_READ);
    if (res != 0) {
        SPDLOG_ERROR("Failed to create memory guard: {}", std::strerror(errno));
        throw std::runtime_error("Failed to create memory guard");
    }

    SPDLOG_TRACE(
      "Created guard region {}-{}", wasmOffset, wasmOffset + regionSize);

    return wasmOffset + regionSize;
}

void WasmModule::queuePthreadCall(threads::PthreadCall call)
{
    // We assume that all pthread calls are queued from the main thread before
    // await is called from the same thread, so this doesn't need to be
    // thread-safe.
    queuedPthreadCalls.emplace_back(call);
}

int WasmModule::awaitPthreadCall(const faabric::Message* msg, int pthreadPtr)
{
    // We assume that await is called in a loop from the master thread, after
    // all pthread calls have been queued, so this function doesn't need to be
    // thread safe.
    assert(msg != nullptr);

    // Execute the queued pthread calls
    if (!queuedPthreadCalls.empty()) {
        int nPthreadCalls = queuedPthreadCalls.size();

        // Set up the master snapshot if not already set up
        std::string snapshotKey = getOrCreateAppSnapshot(*msg, true);

        std::string funcStr = faabric::util::funcToString(*msg, true);
        SPDLOG_DEBUG("Executing {} pthread calls for {} with snapshot {}",
                     nPthreadCalls,
                     funcStr,
                     snapshotKey);

        std::shared_ptr<faabric::BatchExecuteRequest> req =
          faabric::util::batchExecFactory(
            msg->user(), msg->function(), nPthreadCalls);

        req->set_type(faabric::BatchExecuteRequest::THREADS);
        req->set_subtype(wasm::ThreadRequestType::PTHREAD);

        uint32_t groupGid = faabric::util::generateGid();

        for (int i = 0; i < nPthreadCalls; i++) {
            threads::PthreadCall p = queuedPthreadCalls.at(i);
            faabric::Message& m = req->mutable_messages()->at(i);

            // Propagate app ID
            m.set_appid(msg->appid());

            // Snapshot details
            m.set_snapshotkey(snapshotKey);

            // Function pointer and args
            // NOTE - with a pthread interface we only ever pass the
            // function a single pointer argument, hence we use the
            // input data here to hold this argument as a string
            m.set_funcptr(p.entryFunc);
            m.set_inputdata(std::to_string(p.argsPtr));

            // Assign a thread ID and increment. Our pthread IDs start
            // at 1. Set this as part of the group with the other threads.
            m.set_appidx(i + 1);
            m.set_groupid(groupGid);
            m.set_groupidx(i + 1);
            m.set_groupsize(nPthreadCalls);

            // Record this thread -> call ID
            SPDLOG_TRACE("pthread {} mapped to call {}", p.pthreadPtr, m.id());
            pthreadPtrsToChainedCalls.insert({ p.pthreadPtr, m.id() });
        }

        // Submit the call
        faabric::scheduler::Scheduler& sch = faabric::scheduler::getScheduler();
        sch.callFunctions(req);

        // Empty the queue
        queuedPthreadCalls.clear();
    }

    // Await the results of this call
    unsigned int callId = pthreadPtrsToChainedCalls[pthreadPtr];
    SPDLOG_DEBUG("Awaiting pthread: {} ({})", pthreadPtr, callId);
    auto& sch = faabric::scheduler::getScheduler();

    int returnValue = sch.awaitThreadResult(callId);

    // Remove the mapping for this pointer
    pthreadPtrsToChainedCalls.erase(pthreadPtr);

    // If we're the last, resync the snapshot
    if (pthreadPtrsToChainedCalls.empty()) {
        syncAppSnapshot(*msg);
    }

    return returnValue;
}

void WasmModule::createThreadStacks()
{
    SPDLOG_DEBUG("Creating {} thread stacks", threadPoolSize);

    for (int i = 0; i < threadPoolSize; i++) {
        // Allocate thread stack and guard pages
        uint32_t memSize = THREAD_STACK_SIZE + (2 * GUARD_REGION_SIZE);
        uint32_t memBase = growMemory(memSize);

        // Note that wasm stacks grow downwards, so we have to store the
        // stack top, which is the offset one below the guard region above
        // the stack Subtract 16 to make sure the stack is 16-aligned as
        // required by the C ABI
        uint32_t stackTop =
          memBase + GUARD_REGION_SIZE + THREAD_STACK_SIZE - 16;
        threadStacks.push_back(stackTop);

        // Add guard regions
        createMemoryGuardRegion(memBase);
        createMemoryGuardRegion(stackTop + 16);
    }
}

std::vector<uint32_t> WasmModule::getThreadStacks()
{
    return threadStacks;
}

bool WasmModule::isBound()
{
    return _isBound;
}

// ------------------------------------------
// Functions to be implemented by subclasses
// ------------------------------------------

void WasmModule::reset(faabric::Message& msg, const std::string& snapshotKey)
{
    SPDLOG_WARN("Using default reset of wasm module");
}

void WasmModule::doBindToFunction(faabric::Message& msg, bool cache)
{
    throw std::runtime_error("doBindToFunction not implemented");
}

void WasmModule::writeArgvToMemory(uint32_t wasmArgvPointers,
                                   uint32_t wasmArgvBuffer)
{
    throw std::runtime_error("writeArgvToMemory not implemented");
}

void WasmModule::writeWasmEnvToMemory(uint32_t envPointers, uint32_t envBuffer)
{
    throw std::runtime_error("writeWasmEnvToMemory not implemented");
}

uint32_t WasmModule::growMemory(uint32_t nBytes)
{
    throw std::runtime_error("growMemory not implemented");
}

uint32_t WasmModule::shrinkMemory(uint32_t nBytes)
{
    throw std::runtime_error("shrinkMemory not implemented");
}

uint32_t WasmModule::mmapMemory(uint32_t nBytes)
{
    throw std::runtime_error("mmapMemory not implemented");
}

uint32_t WasmModule::mmapFile(uint32_t fp, uint32_t length)
{
    throw std::runtime_error("mmapFile not implemented");
}

void WasmModule::unmapMemory(uint32_t offset, uint32_t nBytes)
{
    throw std::runtime_error("unmapMemory not implemented");
}

uint8_t* WasmModule::wasmPointerToNative(uint32_t wasmPtr)
{
    throw std::runtime_error("wasmPointerToNative not implemented");
}

void WasmModule::printDebugInfo()
{
    throw std::runtime_error("printDebugInfo not implemented");
}

size_t WasmModule::getMemorySizeBytes()
{
    throw std::runtime_error("getMemorySizeBytes not implemented");
}

uint8_t* WasmModule::getMemoryBase()
{
    throw std::runtime_error("getMemoryBase not implemented");
}

int32_t WasmModule::executeFunction(faabric::Message& msg)
{
    throw std::runtime_error("executeFunction not implemented");
}

int32_t WasmModule::executeOMPThread(int threadPoolIdx,
                                     uint32_t stackTop,
                                     faabric::Message& msg)
{
    throw std::runtime_error("executeOMPThread not implemented ");
}

int32_t WasmModule::executePthread(int32_t threadPoolIdx,
                                   uint32_t stackTop,
                                   faabric::Message& msg)
{
    throw std::runtime_error("executePthread not implemented");
}
}
