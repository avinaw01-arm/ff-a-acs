/*
 * Copyright (c) 2024-2025, Arm Limited or its affiliates. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "test_database.h"

#define S_WD_WAIT      30U
#define S_WD_TIMEOUT   15U
#define REFCLK_WAIT    200U
#define IRQ_TRIGGERED  0xABCDABCD

static volatile bool interrupt_triggered;
static uint32_t *ptr;

static int wd_irq_handler(void)
{
    val_twdog_disable();
    interrupt_triggered = true;
    LOG(DBG, "T-WD IRQ Handler Processed\n");
    return 0;
}

static uint32_t sp3_flow(ffa_args_t args)
{
    ffa_args_t payload;
    uint32_t status = VAL_SUCCESS;
    ffa_endpoint_id_t sender = args.arg1 & 0xffff;
    ffa_endpoint_id_t receiver = (args.arg1 >> 16) & 0xffff;
    ffa_endpoint_id_t receiver_1 = val_get_endpoint_id(SP1);
    ffa_memory_handle_t handle;
    uint32_t test_run_data = (uint32_t)args.arg3;

    test_run_data = TEST_RUN_DATA(GET_TEST_NUM((uint32_t)test_run_data),
     (uint32_t)sender, (uint32_t)receiver_1, GET_TEST_TYPE((uint32_t)test_run_data));

    val_select_server_fn_direct(test_run_data, 0, 0, 0, 0);

    /* Wait for the message. */
    val_memset(&payload, 0, sizeof(ffa_args_t));
    payload = val_resp_client_fn_direct((uint32_t)args.arg3, 0, 0, 0, 0, 0);
    if (payload.fid != FFA_MSG_SEND_DIRECT_REQ_64)
    {
        LOG(ERROR, "Direct request failed, fid=0x%x, err 0x%x\n",
                  payload.fid, payload.arg2);
        status =  VAL_ERROR_POINT(1);
        goto exit;
    }

    handle = payload.arg3;
    val_memset(&payload, 0, sizeof(ffa_args_t));
    payload.arg1 =  ((uint32_t)sender << 16) | receiver_1;
    payload.arg3 = handle;
    val_ffa_msg_send_direct_req_64(&payload);
    if (payload.fid == FFA_ERROR_32)
    {
        LOG(ERROR, "Direct request failed, fid=0x%x, err 0x%x\n",
                  payload.fid, payload.arg2);
        status =  VAL_ERROR_POINT(2);
        goto exit;
    }

    /* Sleep until WD Interrupt is triggered */
    sp_sleep(S_WD_WAIT);
    LOG(DBG, "SP Sleep Complete\n");

    val_memset(&payload, 0, sizeof(ffa_args_t));
    payload.arg1 =  ((uint32_t)sender << 16) | receiver_1;
    val_ffa_msg_send_direct_req_64(&payload);
    if (payload.fid == FFA_ERROR_32)
    {
        LOG(ERROR, "Direct request failed, fid=0x%x, err 0x%x\n",
                  payload.fid, payload.arg2);
        status =  VAL_ERROR_POINT(3);
    }

exit:
    val_memset(&payload, 0, sizeof(ffa_args_t));
    payload.arg1 =  ((uint32_t)sender << 16) | receiver;
    val_ffa_msg_send_direct_resp_64(&payload);
    if (payload.fid == FFA_ERROR_32)
    {
        LOG(ERROR, "Direct response failed err %x\n", payload.arg2);
        status = status ? status : VAL_ERROR_POINT(4);
    }

    val_memset(&payload, 0, sizeof(ffa_args_t));
    payload = val_select_server_fn_direct(test_run_data, 0, 0, 0, 0);
    return status ? status : (uint32_t)payload.arg3;
}

static uint32_t sp1_flow(ffa_args_t args)
{
    ffa_args_t payload;
    uint32_t status = VAL_SUCCESS;
    ffa_endpoint_id_t sender = args.arg1 & 0xffff;
    ffa_endpoint_id_t receiver = (args.arg1 >> 16) & 0xffff;
    ffa_endpoint_id_t receiver_1 = val_get_endpoint_id(SP2);
    mb_buf_t mb;
    uint64_t size = 0x1000;
    mem_region_init_t mem_region_init;
    struct ffa_memory_region *memory_region;
    struct ffa_composite_memory_region *composite;
    ffa_memory_handle_t handle;
    uint32_t msg_size;
    memory_region_descriptor_t mem_desc;

    mb.send = val_memory_alloc(size);
    mb.recv = val_memory_alloc(size);
    if (mb.send == NULL || mb.recv == NULL)
    {
        LOG(ERROR, "Failed to allocate RxTx buffer\n");
        status = VAL_ERROR_POINT(5);
        goto free_memory;
    }

    /* Map TX and RX buffers */
    if (val_rxtx_map_64((uint64_t)mb.send, (uint64_t)mb.recv, (uint32_t)(size/PAGE_SIZE_4K)))
    {
        LOG(ERROR, "RxTx Map failed\n");
        status = VAL_ERROR_POINT(6);
        goto free_memory;
    }

    if (val_irq_register_handler(PLATFORM_TWDOG_INTID, wd_irq_handler))
    {
        LOG(ERROR, "WD interrupt register failed\n");
        status = VAL_ERROR_POINT(7);
        goto rxtx_unmap;
    }
    LOG(DBG, "T-WD IRQ Handler Registered\n");

    /* Wait for the message. */
    val_memset(&payload, 0, sizeof(ffa_args_t));
    payload = val_resp_client_fn_direct((uint32_t)args.arg3, 0, 0, 0, 0, 0);
    if (payload.fid != FFA_MSG_SEND_DIRECT_REQ_64)
    {
        LOG(ERROR, "Direct request failed, fid=0x%x, err 0x%x\n",
                  payload.fid, payload.arg2);
        status =  VAL_ERROR_POINT(8);
        goto free_interrupt;
    }

    handle = payload.arg3;

    val_memset(&mem_region_init, 0x0, sizeof(mem_region_init));
    mem_region_init.memory_region = mb.send;
    mem_region_init.sender = receiver_1;
    mem_region_init.receiver = sender;
    mem_region_init.tag = 0;
    mem_region_init.flags = 0;
    mem_region_init.data_access = FFA_DATA_ACCESS_RW;
    mem_region_init.instruction_access = FFA_INSTRUCTION_ACCESS_NOT_SPECIFIED;
    mem_region_init.type = FFA_MEMORY_NORMAL_MEM;
    mem_region_init.cacheability = FFA_MEMORY_CACHE_WRITE_BACK;
#if (PLATFORM_OUTER_SHAREABLE_SUPPORT_ONLY == 1)
    mem_region_init.shareability = FFA_MEMORY_OUTER_SHAREABLE;
#elif (PLATFORM_INNER_SHAREABLE_SUPPORT_ONLY == 1)
    mem_region_init.shareability = FFA_MEMORY_INNER_SHAREABLE;
#elif (PLATFORM_INNER_OUTER_SHAREABLE_SUPPORT == 1)
    mem_region_init.shareability = FFA_MEMORY_OUTER_SHAREABLE;
#endif
    mem_region_init.multi_share = false;
    mem_region_init.receiver_count = 1;

    msg_size = val_ffa_memory_retrieve_request_init(&mem_region_init, handle);

    val_memset(&payload, 0, sizeof(ffa_args_t));
    payload.arg1 = msg_size;
    payload.arg2 = msg_size;
    val_ffa_mem_retrieve_32(&payload);
    if (payload.fid != FFA_MEM_RETRIEVE_RESP_32)
    {
        LOG(ERROR, "Mem retrieve request failed err %d\n", payload.arg2);
        status =  VAL_ERROR_POINT(9);
        goto free_interrupt;
    }
    LOG(DBG, "FFA Mem Retrieve Complete\n");

    memory_region = (struct ffa_memory_region *)mb.recv;
    composite = ffa_memory_region_get_composite(memory_region, 0);
    ptr = (uint32_t *)composite->constituents[0].address;

    /* Map the region into endpoint translation */
    mem_desc.virtual_address = (uint64_t)composite->constituents[0].address;
    mem_desc.physical_address = (uint64_t)composite->constituents[0].address;
    mem_desc.length = size;
    if (VAL_IS_ENDPOINT_SECURE(val_get_endpoint_logical_id(receiver)))
        mem_desc.attributes = ATTR_RW_DATA;
    else
        mem_desc.attributes = ATTR_RW_DATA | ATTR_NS;

    if (val_mem_map_pgt(&mem_desc))
    {
        LOG(ERROR, "Va to pa mapping failed\n");
        status =  VAL_ERROR_POINT(10);
        goto free_interrupt;
    }

    /* Enable Trusted WD Interrupt. */
    val_twdog_enable(S_WD_TIMEOUT);
    LOG(DBG, "Page Table Mapped and T-WD Enabled\n");

    val_memset(&payload, 0, sizeof(ffa_args_t));
    payload.arg1 =  ((uint32_t)sender << 16) | receiver;
    val_ffa_msg_send_direct_resp_64(&payload);
    if (payload.fid != FFA_INTERRUPT_32)
    {
        LOG(ERROR, "FFA_INTERRUPT_32 not received fid %x\n", payload.fid);
        status = VAL_ERROR_POINT(11);
        goto free_interrupt;
    }

    /* Disable Trusted WD Interrupt. */
    val_twdog_intr_disable();
    LOG(DBG, "T-WD Disabled interrupt_triggered %x\n", interrupt_triggered);

    if (interrupt_triggered != true)
    {
        LOG(ERROR, "WD interrupt should be triggered\n");
        status =  VAL_ERROR_POINT(12);
        goto free_interrupt;
    }

    /* Wait for Systen Timer interrupt */
    sp_sleep(REFCLK_WAIT);
    LOG(DBG, "SP Sleep for System timer interrupt\n");

    if (*(volatile uint32_t *)ptr == (uint32_t)IRQ_TRIGGERED)
    {
        LOG(ERROR, "Sys Timer interrupt should be queued\n");
        status =  VAL_ERROR_POINT(13);
        goto free_interrupt;
    }
    LOG(DBG, "IRQ Status Check ptr %x\n", ptr);

    /* Enter Wait State back */
    val_memset(&payload, 0, sizeof(ffa_args_t));
#if (PLATFORM_FFA_V >= FFA_V_1_2)
    /* Prevent RX Buffer Release*/
    payload.arg2 = 0x1;
#endif
    val_ffa_msg_wait(&payload);
    if (payload.fid != FFA_MSG_SEND_DIRECT_REQ_64)
    {
        LOG(ERROR, "DIRECT_REQ_64 not received fid %x\n", payload.fid);
        status = VAL_ERROR_POINT(14);
        goto free_interrupt;
    }

    /* relinquish the memory and notify the sender. */
    ffa_mem_relinquish_init((struct ffa_mem_relinquish *)mb.send, handle, 0, sender, 0x1);
    val_memset(&payload, 0, sizeof(ffa_args_t));
    val_ffa_mem_relinquish(&payload);
    if (payload.fid == FFA_ERROR_32)
    {
        LOG(ERROR, "Mem relinquish failed err %x\n", payload.arg2);
        status = status ? status : VAL_ERROR_POINT(15);
    }
    LOG(DBG, "FFA Mem Relinquish Complete\n");

free_interrupt:
    if (val_irq_unregister_handler(PLATFORM_TWDOG_INTID))
    {
        LOG(ERROR, "IRQ handler unregister failed\n");
        status = status ? status : VAL_ERROR_POINT(16);
    }

#if (PLATFORM_FFA_V >= FFA_V_1_2)
    if (val_rx_release())
    {
        LOG(ERROR, "val_rx_release failed\n");
        status = status ? status : VAL_ERROR_POINT(17);
    }
#endif

rxtx_unmap:
    if (val_rxtx_unmap(sender))
    {
        LOG(ERROR, "RXTX_UNMAP failed\n");
        status = status ? status : VAL_ERROR_POINT(18);
    }

free_memory:
    if (val_memory_free(mb.recv, size) || val_memory_free(mb.send, size))
    {
        LOG(ERROR, "free_rxtx_buffers failed\n");
        status = status ? status : VAL_ERROR_POINT(19);
    }

    val_memset(&payload, 0, sizeof(ffa_args_t));
    payload.arg1 =  ((uint32_t)sender << 16) | receiver;
    val_ffa_msg_send_direct_resp_64(&payload);
    if (payload.fid == FFA_ERROR_32)
    {
        LOG(ERROR, "Direct response failed err %x\n", payload.arg2);
        status = status ? status : VAL_ERROR_POINT(20);
    }
    return status;
}

uint32_t other_secure_int6_server(ffa_args_t args)
{

    uint32_t status = VAL_SUCCESS;
    ffa_endpoint_id_t curr_ep_logical_id;

    curr_ep_logical_id = val_get_curr_endpoint_logical_id();

    if (curr_ep_logical_id == SP3)
    {
        status = sp3_flow(args);
    }
    else if (curr_ep_logical_id == SP1)
    {
        status = sp1_flow(args);
    }

    return status;
}

