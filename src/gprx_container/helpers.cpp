// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#include "helpers.h"

namespace GPRX
{

bool KVHelper::PutObj(Uid& uid, const uint8_t* obj, mpsize_t size, ContainerContext& container, BlockContext& data_block, KVStore* kvstore, SimpleKVLog* kvsimple)
{
    if (!obj || !size || !kvstore)
        return false;

    uint32_t out_offset = 0;
    if (!container.AppendPayloadDataContiguous(data_block, obj, size, out_offset, true))
        return false;
    Locator locator = BuildDataLocator(data_block.headerPtr->block_id, out_offset);

    ValueBuf locator_value_buf;
    locator_value_buf.FromLocator(locator);

    if (kvsimple)
        if (!kvsimple->PutKeyValue(&uid.entries[0], uid.size, locator_value_buf.elements_ptr, locator_value_buf.size_in_u32))
            return false;
    if (!kvstore->Put(&uid.entries[0], uid.size, &uid.precomputed_hash, locator_value_buf.elements_ptr, locator_value_buf.size_in_u32, KVStore::SetStrategy::PutOrReplaceSingle))
        return false;
    return true;
}

uint8_t* KVHelper::GetObj(Uid& uid, mpsize_t* in_out_size, ContainerContext& container, IKVStoreReader* kvstore, Locator* out_locator)
{
    if (!kvstore)
        return nullptr;

    ValueBuf locator_value_buf;
    bool key_found = kvstore->Get(&uid.entries[0], uid.size, &uid.precomputed_hash, locator_value_buf.elements_ptr, &locator_value_buf.size_in_u32);
    if (!key_found)
        return nullptr;

    mpsize_t in_size = in_out_size ? *in_out_size : 0;
    ContainerContext::TypeOfData type_of_data = in_size ? ContainerContext::TypeOfData::Default : ContainerContext::TypeOfData::SizePrefixedObject;

    Locator locator = locator_value_buf.ToLocator();
    if (out_locator)
        *out_locator = locator;
    uint8_t* obj = container.GetDataByLocator(locator, type_of_data, in_out_size);
    return obj;
}

} // GPRX
