// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

#include "types.h"
#include "container_context.h"
#include "kv_store.h"
#include "uid.h"

namespace GPRX
{

class KVHelper
{
public:
    /**
     * @brief Saves object data into 'data_block' and stores the resulting locator into 'kvstore' under 'uid' key
     * @return True on success
     */
    static bool PutObj(Uid& uid, const uint8_t* obj, mpsize_t size, ContainerContext& container, BlockContext& data_block, KVStore* kvstore, SimpleKVLog* kvsimple = nullptr);

    /**
     * @brief Retrieves the locator stored under 'uid' in 'kvstore' and prepares the referenced data for reading.
     * If 'in_out_size' is not provided (nullptr) or its value is 0, the object
     * is retrieved using its built-in size prefix, and that size is stored in 'in_out_size'.
     * @param out_locator Receives the locator of the requested object data
     * @return Pointer to the requested object data
     */
    static uint8_t* GetObj(Uid& uid, mpsize_t* in_out_size, ContainerContext& container, IKVStoreReader* kvstore, Locator* out_locator = nullptr);
};

} // GPRX
