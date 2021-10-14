#include <faabric/util/locks.h>
#include <faabric/util/logging.h>
#include <sgx/WAMREnclave.h>
#include <sgx/system.h>

#include <boost/filesystem/operations.hpp>

namespace sgx {
std::mutex enclaveMx;

WAMREnclave::WAMREnclave()
{
    init();
}

void WAMREnclave::init()
{
    // Skip set-up if enclave already exists
    if (enclaveId != 0) {
        SPDLOG_DEBUG("SGX enclave already exists ({})", enclaveId);
        return;
    }

    faasm_sgx_status_t returnValue;

#if (!SGX_SIM_MODE)
    returnValue = faasm_sgx_get_sgx_support();
    if (returnValue != FAASM_SGX_SUCCESS) {
        SPDLOG_ERROR("Machine doesn't support SGX {}",
                     faasmSgxErrorString(returnValue));
        throw std::runtime_error("Machine doesn't support SGX");
    }
#endif

    // Check enclave file exists
    if (!boost::filesystem::exists(FAASM_ENCLAVE_PATH)) {
        SPDLOG_ERROR("Enclave file {} does not exist", FAASM_ENCLAVE_PATH);
        throw std::runtime_error("Could not find enclave file");
    }

    // Create the enclave
    sgx_status_t sgxReturnValue;
    sgx_launch_token_t sgxEnclaveToken = { 0 };
    uint32_t sgxEnclaveTokenUpdated = 0;
    sgxReturnValue = sgx_create_enclave(FAASM_ENCLAVE_PATH,
                                        SGX_DEBUG_FLAG,
                                        &sgxEnclaveToken,
                                        (int*)&sgxEnclaveTokenUpdated,
                                        &enclaveId,
                                        nullptr);

    if (sgxReturnValue != SGX_SUCCESS) {
        SPDLOG_ERROR("Unable to create enclave: {}",
                     sgxErrorString(sgxReturnValue));
        throw std::runtime_error("Unable to create enclave");
    }

    SPDLOG_DEBUG("Created SGX enclave: {}", enclaveId);

    sgxReturnValue = enclaveInitWamr(enclaveId, &returnValue);
    if (sgxReturnValue != SGX_SUCCESS) {
        SPDLOG_ERROR("Unable to enter enclave: {}",
                     sgxErrorString(sgxReturnValue));
        throw std::runtime_error("Unable to enter enclave");
    }

    if (returnValue != FAASM_SGX_SUCCESS) {
        if (FAASM_SGX_OCALL_GET_SGX_ERROR(returnValue)) {
            SPDLOG_ERROR(
              "Unable to initialise WAMR due to an SGX error: {}",
              sgxErrorString(
                (sgx_status_t)FAASM_SGX_OCALL_GET_SGX_ERROR(returnValue)));
            throw std::runtime_error(
              "Unable to initialise WAMR due to an SGX error");
        }
        SPDLOG_ERROR("Unable to initialise WAMR: {}",
                     faasmSgxErrorString(returnValue));
        throw std::runtime_error("Unable to initialise WAMR");
    }

    SPDLOG_DEBUG("Initialised WAMR in SGX enclave {}", enclaveId);
}

void WAMREnclave::tearDown()
{
    if (enclaveId == 0) {
        return;
    }

    SPDLOG_DEBUG("Destroying enclave {}", enclaveId);

    sgx_status_t sgxReturnValue = sgx_destroy_enclave(enclaveId);
    if (sgxReturnValue != SGX_SUCCESS) {
        SPDLOG_ERROR("Unable to destroy enclave {}: {}",
                     enclaveId,
                     sgxErrorString(sgxReturnValue));
        throw std::runtime_error("Unable to destroy enclave");
    }

    enclaveId = 0;
}

bool WAMREnclave::isSetUp()
{
    return enclaveId != 0;
}

bool WAMREnclave::isWasmLoaded()
{
    return !loadedBytes.empty();
}

bool WAMREnclave::isWasmLoaded(const std::vector<uint8_t>& dataToLoad)
{
    return loadedBytes == dataToLoad;
}

sgx_enclave_id_t WAMREnclave::getId()
{
    return enclaveId;
}

void WAMREnclave::loadWasmModule(std::vector<uint8_t>& data)
{
    faasm_sgx_status_t returnValue;
    // Note - loading and instantiating happen in the same ecall
    sgx_status_t status = enclaveLoadModule(
      enclaveId, &returnValue, (void*)data.data(), (uint32_t)data.size());

    if (status != SGX_SUCCESS) {
        SPDLOG_ERROR("Unable to enter enclave: {}", sgxErrorString(status));
        throw std::runtime_error("Unable to enter enclave");
    }

    if (returnValue != FAASM_SGX_SUCCESS) {
        SPDLOG_ERROR("Unable to load WASM module: {}",
                     faasmSgxErrorString(returnValue));
        throw std::runtime_error("Unable to load WASM module");
    }

    // If succesful, store the loaded bytes for caching purposes
    loadedBytes = std::move(data);
}

void WAMREnclave::unloadWasmModule()
{
    SPDLOG_DEBUG("Unloading SGX wasm module");

    // TODO - thing how to make reset work even if the module is not sent
    // and wether we should keep track of the set modules outside the enclave
    // as well
    faasm_sgx_status_t returnValue;
    sgx_status_t sgxReturnValue = enclaveUnloadModule(enclaveId, &returnValue);

    if (sgxReturnValue != SGX_SUCCESS) {
        SPDLOG_ERROR("Unable to unbind function due to SGX error: {}",
                     sgxErrorString(sgxReturnValue));
        throw std::runtime_error("Unable to unbind function due to SGX error");
    }

    if (returnValue != FAASM_SGX_SUCCESS) {
        SPDLOG_ERROR("Unable to unbind function: {}",
                     faasmSgxErrorString(returnValue));
        throw std::runtime_error("Unable to unbind function");
    }

    // If succesful, clear the stored bytes
    loadedBytes.clear();
}

void WAMREnclave::callMainFunction()
{
    // Enter enclave and call function
    faasm_sgx_status_t returnValue;
    sgx_status_t sgxReturnValue = enclaveCallFunction(enclaveId, &returnValue);

    if (sgxReturnValue != SGX_SUCCESS) {
        SPDLOG_ERROR("Unable to enter enclave: {}",
                     sgxErrorString(sgxReturnValue));
        throw std::runtime_error("Unable to enter enclave");
    }

    if (returnValue != FAASM_SGX_SUCCESS) {
        // Check if an ocall has failed
        sgxReturnValue =
          (sgx_status_t)FAASM_SGX_OCALL_GET_SGX_ERROR(returnValue);
        if (sgxReturnValue) {
            SPDLOG_ERROR("An OCALL failed: {}", sgxErrorString(sgxReturnValue));
            throw std::runtime_error("OCALL failed");
        }

        SPDLOG_ERROR("Error occurred during function execution: {}",
                     faasmSgxErrorString(returnValue));
        throw std::runtime_error("Error occurred during function execution");
    }
}

std::shared_ptr<WAMREnclave> acquireGlobalWAMREnclave()
{
    SPDLOG_TRACE("Locking WAMR Enclave");
    enclaveMx.lock();

    static std::shared_ptr<WAMREnclave> enclave =
      std::make_shared<WAMREnclave>();
    enclave->init();
    return enclave;
}

void releaseGlobalWAMREnclave()
{
    SPDLOG_TRACE("Unlocking WAMR Enclave");
    enclaveMx.unlock();
}
}
