/*
 * Copyright (c) 2021, 2025, Arm Limited or its affiliates. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "test_database.h"

static uint32_t donate_input_error_checks3_helper(uint32_t test_run_data, uint32_t fid)
{
    ffa_args_t payload;
    uint32_t status = VAL_SUCCESS;
    uint32_t client_logical_id = GET_CLIENT_LOGIC_ID(test_run_data);
    ffa_endpoint_id_t sender = val_get_endpoint_id(client_logical_id);
#if (PLATFORM_NS_HYPERVISOR_PRESENT == 1 && PLATFORM_SP_EL == -1)
    ffa_endpoint_id_t recipient = val_get_endpoint_id(VM2);
#else
    ffa_endpoint_id_t recipient = val_get_endpoint_id(SP2);
#endif
    mb_buf_t mb;
    uint8_t *pages = NULL;
    uint64_t size = 0x1000;
    ffa_memory_handle_t handle;
    mem_region_init_t mem_region_init;
    struct ffa_memory_region_constituent constituents[1];
    const uint32_t constituents_count = sizeof(constituents) /
                sizeof(struct ffa_memory_region_constituent);
    uint32_t status_32, status_64;

    status_64 = val_is_ffa_feature_supported(FFA_MEM_LEND_64);
    status_32 = val_is_ffa_feature_supported(FFA_MEM_LEND_32);
    if (status_64 && status_32)
    {
        LOG(TEST, "FFA_MEM_LEND not supported, skipping the check\n");
        return VAL_SKIP_CHECK;
    }

    mb.send = val_memory_alloc(size);
    mb.recv = val_memory_alloc(size);
    if (mb.send == NULL || mb.recv == NULL)
    {
        LOG(ERROR, "Failed to allocate RxTx buffer\n");
        status = VAL_ERROR_POINT(1);
        goto free_memory;
    }

    /* Map TX and RX buffers */
    if (val_rxtx_map_64((uint64_t)mb.send, (uint64_t)mb.recv, (uint32_t)(size/PAGE_SIZE_4K)))
    {
        LOG(ERROR, "RxTx Map failed\n");
        status = VAL_ERROR_POINT(2);
        goto free_memory;
    }
    val_memset(mb.send, 0, size);

    pages = (uint8_t *)val_memory_alloc(size);
    if (!pages)
    {
        LOG(ERROR, "Memory allocation failed\n");
        status = VAL_ERROR_POINT(3);
        goto rxtx_unmap;
    }

    constituents[0].address = val_mem_virt_to_phys((void *)pages);
    constituents[0].page_count = 1;

    val_memset(&mem_region_init, 0x0, sizeof(mem_region_init));
    mem_region_init.memory_region = mb.send;
    mem_region_init.sender = sender;
    mem_region_init.receiver = recipient;
    mem_region_init.tag = 0;
    mem_region_init.flags = 0;
    mem_region_init.data_access = FFA_DATA_ACCESS_RW;
    mem_region_init.instruction_access = FFA_INSTRUCTION_ACCESS_NOT_SPECIFIED;
    mem_region_init.type = FFA_MEMORY_NOT_SPECIFIED_MEM;
    mem_region_init.cacheability = 0;
    mem_region_init.shareability = 0;
    mem_region_init.multi_share = false;

    val_ffa_memory_region_init(&mem_region_init, constituents, constituents_count);
    val_memset(&payload, 0, sizeof(ffa_args_t));
    payload.arg1 = mem_region_init.total_length;
    payload.arg2 = mem_region_init.fragment_length;

    if (!status_64)
        val_ffa_mem_lend_64(&payload);
    else
        val_ffa_mem_lend_32(&payload);

    if (payload.fid == FFA_ERROR_32)
    {
        LOG(ERROR, "MEM_LEND request failed err %x\n", payload.arg2);
        status = VAL_ERROR_POINT(4);
    }
    LOG(DBG, "Mem Lend Complete\n");

    handle = ffa_mem_success_handle(payload);

    /* Must ensure that the memory region is in the Owner-EA state
     * for the Owner. It must return DENIED in case of error.
     */
    mem_region_init.data_access = FFA_DATA_ACCESS_NOT_SPECIFIED;
    val_ffa_memory_region_init(&mem_region_init, constituents, constituents_count);
    val_memset(&payload, 0, sizeof(ffa_args_t));
    payload.arg1 = mem_region_init.total_length;
    payload.arg2 = mem_region_init.fragment_length;

    if (fid == FFA_MEM_DONATE_64)
        val_ffa_mem_donate_64(&payload);
    else
        val_ffa_mem_donate_32(&payload);

    if (payload.fid != FFA_ERROR_32 && payload.arg2 != FFA_ERROR_DENIED)
    {
        LOG(ERROR, "MEM_DONATE request must failed err %x\n", payload.arg2);
        status = VAL_ERROR_POINT(5);
    }

    val_memset(&payload, 0, sizeof(ffa_args_t));
    payload.arg1 = (uint32_t)handle;
    payload.arg2 = (uint32_t)(handle >> 32);
    payload.arg3 = 0;
    val_ffa_mem_reclaim(&payload);
    if (payload.fid == FFA_ERROR_32)
    {
        LOG(ERROR, "Mem Reclaim failed err %x\n", payload.arg2);
        status = VAL_ERROR_POINT(6);
    }

    LOG(DBG, "Mem Reclaim Complete\n");

rxtx_unmap:
    if (val_rxtx_unmap(sender))
    {
        LOG(ERROR, "RXTX_UNMAP failed\n");
        status = status ? status : VAL_ERROR_POINT(7);
    }

free_memory:
    if (val_memory_free(mb.recv, size) || val_memory_free(mb.send, size))
    {
        LOG(ERROR, "free_rxtx_buffers failed\n");
        status = status ? status : VAL_ERROR_POINT(8);
    }

    if (val_memory_free(pages, size))
    {
        LOG(ERROR, "val_mem_free failed\n");
        status = status ? status : VAL_ERROR_POINT(9);
    }

    return status;

}

uint32_t donate_input_error_checks3_client(uint32_t test_run_data)
{
    uint32_t status_32, status_64, status;

    status_64 = val_is_ffa_feature_supported(FFA_MEM_DONATE_64);
    status_32 = val_is_ffa_feature_supported(FFA_MEM_DONATE_32);
    if (status_64 && status_32)
    {
        LOG(TEST, "FFA_MEM_DONATE not supported, skipping the check\n");
        return VAL_SKIP_CHECK;
    }
    else if (status_64 && !status_32)
    {
        status = donate_input_error_checks3_helper(test_run_data, FFA_MEM_DONATE_32);
    }
    else if (!status_64 && status_32)
    {
        status = donate_input_error_checks3_helper(test_run_data, FFA_MEM_DONATE_64);
    }
    else
    {
        status = donate_input_error_checks3_helper(test_run_data, FFA_MEM_DONATE_64);
        if (status)
            return status;

        status = donate_input_error_checks3_helper(test_run_data, FFA_MEM_DONATE_32);
        if (status)
            return status;
    }
    return status;
}
