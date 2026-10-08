// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

#include "gprx_container/kv_store.h"
#include "flatbuffer_types/mp_types_ntd.h"

namespace LibMP
{

class KVStoreEx
{
public:
    inline static uint32_t GetKvStoreFrameIdFromMeta(const GPRX::BlockHeader* headerPtr)
    {
        return GPRX::Block::GetMeta1View(headerPtr)->parts.high;
    }

    inline static void PutKvStoreFrameIdToMeta(const GPRX::BlockHeader* headerPtr, uint32_t frameId)
    {
        GPRX::Block::GetMeta1View(headerPtr)->parts.high = frameId;
    }    
};

struct ValueBufEx : public GPRX::ValueBuf
{
    void FromCounterSample(const LibMP::TypesNtd::CounterSample& val)
    {
        WriteWrapper([&]()
        {
            bool mode = sequential_mode;
            sequential_mode = true;
            FromU32(val.frame_id());
            FromDouble(val.value());
            sequential_mode = mode;
        });
    }

    LibMP::TypesNtd::CounterSample ToCounterSample()
    {
        return ReadWrapper(3, [&]()
        {
            LibMP::TypesNtd::CounterSample result;
            result.mutate_frame_id(GetU32(0));
            result.mutate_value(GetDouble(1));
            return result;
        });
    }

    void FromU64Sample(const LibMP::TypesNtd::U64Sample& val)
    {
        WriteWrapper([&]()
        {
            bool mode = sequential_mode;
            sequential_mode = true;
            FromU32(val.frame_id());
            FromU64(val.value());
            sequential_mode = mode;
        });
    }

    LibMP::TypesNtd::U64Sample ToU64Sample()
    {
        return ReadWrapper(3, [&]()
        {
            LibMP::TypesNtd::U64Sample result;
            result.mutate_frame_id(GetU32(0));
            result.mutate_value(GetU64(1));
            return result;
        });
    }
};

} // LibMP
