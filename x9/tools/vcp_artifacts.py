#!/usr/bin/env python3
"""Inspect explicit local X9 LK/VCP firmware, ELF modules and captured FDT offline.

Writes a NEW private output directory; never loads modules, executes firmware,
opens a device, generates a boot image or claims runtime readiness. Certificates
and opaque trailers are preserved, not verified or stripped. Python stdlib only.
The rv_v2 mailbox/layout model is based on the captured stock vcp_v2.ko; source
names are explanatory. Neither hashes nor vermagic prove a loaded-image pairing.
An explicit captured sysfs GNU note can prove a matching build-id only, not a
loaded text hash, runtime firmware residency, SHM state or hardware readiness.
"""
import argparse
import hashlib
import json
import os
import stat
import struct
from pathlib import Path

MAX_INPUT = 16 << 20
MAX_LK_INPUT = 64 << 20  # bounded partition copy, NOT /dev or a live reader


def span(blob, at, size):
    if at < 0 or size < 0 or at > len(blob) or size > len(blob) - at:
        raise ValueError('range outside artifact')
    return blob[at:at + size]


def cstring(blob, at):
    if not 0 <= at < len(blob):
        raise ValueError('string offset outside artifact')
    end = blob.find(b'\0', at)
    if end < 0:
        raise ValueError('unterminated string')
    return blob[at:end].decode('utf-8', errors='strict')


def read_input(path, limit=MAX_INPUT):
    # An explicit local regular file only, no /dev/mem or FIFO reads.
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, 'rb') as f:
        info = os.fstat(f.fileno())
        if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= limit:
            raise ValueError(f'input must be a nonempty regular file <={limit} bytes')
        blob = f.read(limit + 1)
        if len(blob) != info.st_size:
            raise ValueError('input size changed during read')
    return blob


def identity(path, blob):
    return dict(source=str(path.absolute()), bytes=len(blob),
                sha256=hashlib.sha256(blob).hexdigest())


def lk_bootstrap_evidence(blob):
    """An explicit MTK LK container; link RVAs exclude the 512B header.

    Whole DECLARED payload gates the captured boot chain. Partition padding
    is recorded separately; no signature interpretation or SMC execution.
    """
    header = span(blob, 0, 512)
    magic, size = struct.unpack_from('<II', header)
    ext, hsize, hver = struct.unpack_from('<III', header, 48)
    if (magic != 0x58881688 or ext != 0x58891689 or hsize != 512 or hver != 1 or
            cstring(header[8:40], 0) != 'lk' or not 0 < size <= MAX_INPUT or
            struct.unpack_from('<I', header, 72)[0] != 0):
        raise ValueError('unsupported MTK LK container')
    payload = span(blob, 512, size)
    digest = hashlib.sha256(payload).hexdigest()
    result = dict(profile_match=False, payload_bytes=size, payload_sha256=digest,
        header_bytes=512, rva_file_offset=512, signature_verified=False,
        header_raw_hex=header.hex(), runtime_loaded_firmware_proven=False,
        runtime_ready_proven=False, runtime_preserve_proven=False,
        tail=dict(offset=512 + size, bytes=len(blob) - 512 - size,
                  sha256=hashlib.sha256(blob[512 + size:]).hexdigest(),
                  certificate_and_padding_semantics_unclassified=True))
    if size != 1835856 or digest != '2665132514f272bd18f1fadc99524adc8d35c198019aa230dfbd847986140529':
        return result
    spans = (
        ('boot_calls_app_load_vcp', 0x13b68, 0x13b98),
        ('rv55_tcm_and_dram_named_loader_calls', 0x53b08, 0x53b50),
        ('verify_prepare_cmd6', 0x53b9c, 0x53bcc),
        ('verify_load_cmd2_tcm_and_dram_arguments', 0x53bcc, 0x53bf8),
        ('post_load_cmd0_arguments', 0x53c10, 0x53c58),
        ('load_return_status_custom_policy', 0x53c58, 0x53c68),
        ('smccc_wrapper_smc_and_result_copy', 0x224dc, 0x224ec),
    )
    result.update(profile_match=True, profile='captured LK signed VCP load chain ABI 1',
        app_load_vcp_rva=0x52f98, smc_function_id=0xc2000413,
        load_commands=[6, 2, 0], reset_release_is_separate_function_id=0xc200052c,
        tcm_payload_name=cstring(payload, 0x112a66),
        dram_payload_name=cstring(payload, 0x10d31a),
        dram_staging_offset=0x200000, dram_loader_capacity=0x300000,
        load_cmd2_tcm_offset_argument=0x2000,
        load_status_does_not_prove_runtime_ready=True,
        boot_mode_and_secure_monitor_success_semantics_not_executed=True,
        instruction_evidence=[dict(name=name, rva=start, end_exclusive=end,
            file_offset=512 + start, raw_hex=span(payload, start, end - start).hex())
            for name, start, end in spans])
    return result


def gnu_build_id(blob):
    """Parse bounded ELF notes; reject ambiguous GNU build-id evidence."""
    found, at = [], 0
    while at < len(blob):
        nsize, dsize, typ = struct.unpack('<III', span(blob, at, 12))
        at += 12
        name = span(blob, at, nsize)
        span(blob, at, (nsize + 3) & ~3)
        at += (nsize + 3) & ~3
        desc = span(blob, at, dsize)
        span(blob, at, (dsize + 3) & ~3)
        at += (dsize + 3) & ~3
        if name == b'GNU\0' and typ == 3:
            if not 0 < dsize <= 64:
                raise ValueError('unsupported GNU build-id size')
            found.append(desc.hex())
    if len(found) > 1:
        raise ValueError('ambiguous GNU build-id notes')
    return found[0] if found else None


def captured_loaded_note(path):
    blob = read_input(path)
    if len(blob) > 4096:
        raise ValueError('captured loaded-module note exceeds 4 KiB')
    build_id = gnu_build_id(blob)
    if build_id is None:
        raise ValueError('captured note has no GNU build-id')
    return identity(path, blob) | dict(build_id=build_id,
        provenance_verified=False, loaded_text_hash_proven=False)


def rv55_wire_evidence(data):
    """Recognize the observed C.57 converter; never infer a runtime alias.

    These are instruction/table bytes in the firmware PAYLOAD, not the signed
    container or LK physical addresses. Only the 16 HFRP rows are described;
    other remap rows and the opaque custom loop instruction remain uninterpreted.
    """
    checks = ((0x10fe0, '370500f8'),       # LUI: key mask f8000000
              (0x10fe6, '13f7f500'),       # ANDI: high IOVA nibble
              (0x1101e, '0841'),           # load selected alias base
              (0x11020, 'b7050008fd15'),   # mask = 07ffffff
              (0x11026, 'b375b8004d8d'))  # preserve low27, OR alias
    if len(data) != 184320 or any(span(data, at, len(bytes.fromhex(raw))) !=
                                  bytes.fromhex(raw) for at, raw in checks):
        return dict(profile_match=False, runtime_alias_proven=False)
    rows = []
    for i in range(16):
        at = 0x1c220 + i * 16
        low, high, alias, reserved = struct.unpack('<4I', span(data, at, 16))
        if (low, high, alias, reserved) != (0x40000000 + i * 0x8000000,
                                          1, 0x30000000 + i * 0x8000000, 0):
            return dict(profile_match=False, runtime_alias_proven=False)
        iova = (high << 32) | low
        rows.append(dict(payload_offset=at, iova_start=iova,
                         iova_end=iova + 0x8000000, firmware_alias_start=alias,
                         firmware_alias_end=alias + 0x8000000,
                         raw_words=[low, high, alias, reserved]))
    return dict(profile_match=True, profile='observed C.57 RV55 converter',
                payload_link_base=0, converter_offset=0x10fdc,
                converter_end=0x11030, preserved_low_bits=27,
                service_id=1, transport_out_channel=0, transport_in_channel=1,
                query_cap=0xa100, query_cap_done=0xb100, shm_alloc=0xc005,
                shm_alloc_done_source_enum=0xd005, shm_payload_bytes=64,
                hfrp_alias_windows=rows, runtime_alias_proven=False,
                loaded_module_pairing_proven=False, signature_verified=False)


def rv55_shm_evidence(tcm, dram):
    """Match both observed payloads before describing the service workspace.

    Link addresses are firmware addresses, NOT PA. The common pointer reaches
    a parameter dispatcher, but its custom selectors remain opaque; neither
    the size nor this call proves enable_smmu's offset or a codec callback ABI.
    """
    result = dict(profile_match=False, common_field_abi_proven=False,
                  loaded_module_pairing_proven=False, runtime_shm_proven=False)
    if (tcm is None or dram is None or len(tcm) != 184320 or
            len(dram) != 2057906 or not rv55_wire_evidence(tcm)['profile_match']):
        return result
    checks = (
        ('TCM', 0x117ee,
         'b6552655eff0affe6c181018341878105c181308810297402f00e78080e6'),
        ('TCM', 0x11872,
         '4255b25522562e95f645329563fba5007370043071659305700913054547efe07f9d'),
        ('DRAM', 0x30566c,
         '059085903a8a37470300b68a3284ae84b795330013060505b7163e00130707d5'
         '23aca59e23a2c674856618c03796330093860680428903a5859fbe893a950567'
         '232ea69e2320da00130707400325c69f3695b796330023a0a6a02320e800'
         '03a5859f88c00325c69f23a0aa0003a506a088c3'),
        ('DRAM', 0x30a3e2,
         '37953300856522c026c40325859f938505390a868146b304b400dd452ac88840ef60a6ab'),
        ('DRAM', 0x2706bc,
         '059085902e84b7152700b289aa840945938575f922863689ef203d8f85cc5d4563438504'),
        ('DRAM', 0x2707a8, '9854a848a2854e86ca860297'),
    )
    evidence = []
    for segment, address, raw in checks:
        base, blob = (0, tcm) if segment == 'TCM' else (0x200000, dram)
        expected = bytes.fromhex(raw)
        offset = address - base
        if span(blob, offset, len(expected)) != expected:
            return result
        evidence.append(dict(segment=segment, link_address=address,
                             payload_offset=offset, bytes=len(expected), raw_hex=raw))
    result.update(profile_match=True, profile='observed RV55 SHM service layout',
        instruction_evidence=evidence, minimum_vdec_bytes=0x35950,
        partitions=[dict(name='common_opaque', offset=0, bytes=0x50),
                    dict(name='instance_pool', offset=0x50, bytes=0x33d00),
                    dict(name='auxiliary_0', offset=0x33d50, bytes=0x800),
                    dict(name='auxiliary_1', offset=0x34550, bytes=0x1400)],
        workspace_layout_function=0x30566c, common_pointer_global=0x3389f8,
        instance_pool_pointer_global=0x3e1744,
        separate_prop_log_regions=True,
        common_parameter_call=dict(caller=0x30a402, callee=0x2706bc,
            parameter_id=0x17, argument_block_common_pointer_offset=0x10,
            custom_dispatch_selectors_opaque=True, selected_callback_proven=False),
        normal_indirect_dispatch=dict(address=0x2707a8,
            instance_callback_offset=0x28, instance_codec_handle_offset=0x50,
            common_parameter_reaches_this_branch_proven=False))
    return result


def rv55_init_evidence(dram):
    """Captured payload INIT framing; custom branches remain uninterpreted."""
    result = dict(profile_match=False, runtime_init_proven=False,
                  complete_vsi_abi_proven=False, context_echo_proven=False,
                  module_firmware_pairing_proven=False)
    if (dram is None or len(dram) != 2057906 or
            hashlib.sha256(dram).hexdigest() !=
            '200294bca18891fe4cc8b8cc93489b586305f1acb80aeea176566f1726392c3f'):
        return result
    spans = (
        ('init_without_existing_instance_dispatch', 0x305c6a, 0x305c8e),
        ('init_branch', 0x305ee0, 0x305ee6),
        ('init_ack_buffer_zeroing', 0x30659c, 0x3065c4),
        ('codec_check_id', 0x3065fe, 0x306608),
        ('candidate_codec_and_svp_getters', 0x306608, 0x30661c),
        ('codec_candidate_lookup', 0x30571e, 0x30577a),
        ('svp_candidate_lookup_custom_index_opaque', 0x30577a, 0x3057d2),
        ('reverse_codec_check_send', 0x306646, 0x306666),
        ('candidate_loop_and_exhaustion', 0x306666, 0x30667a),
        ('init_ack_id_token_alias', 0x3070b8, 0x3070ea),
        ('init_ack_send_32_bytes', 0x307118, 0x307126),
        ('send_stub_to_generic_transport', 0x3052f2, 0x3052fc),
        ('generic_transport_copy', 0x30507a, 0x3050b4),
    )
    evidence = [dict(name=name, segment='DRAM', link_address=start,
                    end_exclusive=end, payload_offset=start - 0x200000,
                    raw_hex=span(dram, start - 0x200000, end - start).hex())
                for name, start, end in spans]
    result.update(profile_match=True, profile='captured RV55 INIT framing',
        init=0xa000, init_done=0xb000, init_done_payload_bytes=32,
        reverse_codec_check=dict(message=0xc008, source_name='CHECK_CODEC_ID',
            payload_bytes=40, status_field_is_candidate_svp_not_errno=True,
            codec_offset=24, opaque_data_offset=28, opaque_payload_offset=32,
            ap_token_offset=8, status_offset=16, candidate_loop_limit=31,
            physical_handling_implemented=False,
            candidate_status_branch_custom_opcode_opaque=True),
        ack=dict(message_offset=0, context_offset=4, ap_token_offset=8,
            status_offset=16, firmware_vsi_alias_offset=24,
            alias_high_word_explicitly_zero=True,
            context_policy='not proved echoed; observe separately, match fresh AP token'),
        instance_pool_pointer_global=0x3e1744, instance_slot_stride=0x19e8,
        instance_pool_offset=0x50, instance_pool_bytes=0x33d00,
        instance_slot_capacity=0x33d00 // 0x19e8,
        capacity_is_layout_not_live_allocation=True,
        stride_is_not_complete_vsi_layout=True,
        codec_candidate_table=dict(link_address=0x30e5b4, row_bytes=12,
            rows=[dict(codec_id=codec, candidate_indices=[first, second])
                  for codec, first, second in struct.iter_unpack(
                      '<3I', span(dram, 0x30e5b4 - 0x200000, 9 * 12))],
            raw_hex=span(dram, 0x30e5b4 - 0x200000, 9 * 12).hex(),
            table_presence_is_not_runtime_codec_availability=True),
        instruction_evidence=evidence, custom_opcodes_opaque=True)
    return result


def rv55_start_evidence(dram, init):
    """START framing only; custom decode/ACK ownership is not inferred."""
    result = dict(profile_match=False, runtime_start_proven=False,
                  hardware_completion_proven=False, linear_pixels_proven=False,
                  complete_vsi_abi_proven=False, module_firmware_pairing_proven=False)
    if not init['profile_match']:
        return result
    spans = (
        ('start_dispatch_upper_bound_a001', 0x305d5c, 0x305d68),
        ('init_or_start_branch_and_ack_zeroing', 0x305ee0, 0x305efe),
        ('start_request_alias_load', 0x305f16, 0x305f1e),
        ('start_ack_id_and_helper', 0x305fb2, 0x305fc6),
        ('start_ack_send_40_bytes', 0x30b5fe, 0x30b610),
    )
    result.update(profile_match=True, profile='captured RV55 START framing',
        start=0xa001, start_done=0xb001, start_done_payload_bytes=40,
        request_instance_alias_offset=8,
        request_alias_is_not_ap_token=True, ack_context_echo_proven=False,
        ack_token_construction_complete_proven=False,
        ack_is_not_displayable_frame_or_hardware_completion=True,
        decode_callback_and_custom_branches_opaque=True,
        instruction_evidence=[dict(name=name, segment='DRAM', link_address=start,
            end_exclusive=end, payload_offset=start - 0x200000,
            raw_hex=span(dram, start - 0x200000, end - start).hex())
            for name, start, end in spans])
    return result


def rv55_vp_evidence(dram, init):
    """VP reverse request/publication consumers; selectors remain opaque."""
    result = dict(profile_match=False, runtime_vp_proven=False,
                  dma_mapping_and_cache_ownership_proven=False,
                  module_firmware_pairing_proven=False)
    if not init['profile_match']:
        return result
    spans = (
        ('zero_40_byte_reverse_request', 0x3088a8, 0x3088c0),
        ('common_smmu_depth_and_selected_publication', 0x3089d4, 0x3089f8),
        ('c009_status0_id0_ap_token_send40', 0x3089f8, 0x308a1c),
        ('selected_publication_recheck_custom_branch_opaque', 0x308a4e, 0x308a5e),
        ('three_iovas_to_parameter_1e_callback_opaque', 0x308a5e, 0x308a7c),
    )
    result.update(profile_match=True, profile='captured RV55 VP reverse request',
        request=dict(message=0xc009, service_id=1, payload_bytes=40,
            status=0, parameter_id=0, ap_token_offset=8,
            bitdepth_in_request=False, context_initially_zero=True),
        bitdepth_offset=0xf80, group_index='bitdepth == 8 ? 0 : 1',
        common_global=0x3389f8, smmu_flag_offset=24, publication_flag_offsets=[25, 26],
        iova_tuple_offset=32, iova_tuple_stride=24,
        publication_branch_custom_opcode_opaque=True,
        parameter_1e_callback_target_and_success_proven=False,
        instruction_evidence=[dict(name=name, segment='DRAM', link_address=start,
            end_exclusive=end, payload_offset=start - 0x200000,
            raw_hex=span(dram, start - 0x200000, end - start).hex())
            for name, start, end in spans])
    return result


def rv55_service_dispatch_evidence(tcm, dram, init):
    """Captured service init descriptors -> queues -> common dispatcher.

    Gate BOTH whole payloads; no task creation or custom branch is executed.
    Descriptor words and globals are firmware link addresses, never host PAs.
    Startup calls descriptor+8 to initialize software service queues/tasks,
    not to acquire a clock/DMA owner. Named exit functions call vTaskDelete;
    the wrapper's zero return does not prove a join or hardware retirement.
    """
    result = dict(profile_match=False, runtime_dispatch_proven=False,
                  runtime_registration_success_proven=False,
                  runtime_ready_proven=False, module_firmware_pairing_proven=False,
                  runtime_task_exit_producer_exclusion_proven=False,
                  runtime_task_exit_dma_retirement_proven=False)
    if (not init['profile_match'] or tcm is None or len(tcm) != 184320 or
            hashlib.sha256(tcm).hexdigest() !=
            'f59d646ba74fab1c40500139c40d67c97a0e8ab96c1569f738a0c7403cc49508'):
        return result
    spans = (
        ('service1_register_callback', 'TCM', 0x11500, 0x11516),
        ('service1_store_dispatcher', 'TCM', 0x11516, 0x11526),
        ('service2_register_callback', 'TCM', 0x1164a, 0x11660),
        ('service2_store_dispatcher_create_queue', 'TCM', 0x11660, 0x1168c),
        ('service2_queue_handle_and_task_descriptor', 'TCM', 0x1168c, 0x116d0),
        ('service2_callback_queue_record_custom_length_opaque', 'TCM', 0x118ac, 0x1192c),
        ('task_entry_calls_descriptor_word3', 'DRAM', 0x30501e, 0x30502c),
        ('service_registrar_callback_table', 'DRAM', 0x30502c, 0x30507a),
        ('task_create_helper', 'DRAM', 0x2470ba, 0x24710e),
        ('service2_worker_payload_stack', 'DRAM', 0x328058, 0x32805e),
        ('service2_worker_receive_queue', 'DRAM', 0x32805e, 0x328072),
        ('service2_worker_direct_dispatch_and_loop', 'DRAM', 0x3280c4, 0x3280d8),
        ('startup_calls_service_descriptor8_before_clock_init', 'DRAM', 0x31694c, 0x31696e),
        ('service1_first_queue_create_and_handle_store', 'TCM', 0x1153c, 0x11562),
        ('service1_second_queue_create_and_handle_store', 'TCM', 0x11562, 0x1158a),
        ('service1_task_descriptor_and_create_arguments', 'TCM', 0x115a6, 0x115ec),
        ('service1_worker_receive_queue_record', 'DRAM', 0x327ef6, 0x327f0e),
        ('service1_worker_common_dispatch_and_receive_loop', 'DRAM', 0x327fac, 0x327fc8),
        ('service1_named_exit_calls_task_exit_helpers', 'DRAM', 0x327fc8, 0x327fe4),
        ('service1_core_exit_helper_handle_and_call', 'DRAM', 0x305a8a, 0x305aa0),
        ('service1_res_exit_helper_handle_and_call', 'DRAM', 0x305ac2, 0x305aee),
        ('thread_exit_load_delete_log_and_zero_return', 'DRAM', 0x24710e, 0x247166),
        ('task_delete_entry_hartid_and_current_task_selection', 'TCM', 0x753c, 0x755a),
        ('task_delete_list_mutation_candidate', 'TCM', 0x75ce, 0x75f6),
        ('task_delete_return_or_reschedule_frontier', 'TCM', 0x7606, 0x7656),
    )
    evidence = []
    for name, segment, start, end in spans:
        payload, base = (tcm, 0) if segment == 'TCM' else (dram, 0x200000)
        evidence.append(dict(name=name, segment=segment, link_address=start,
            end_exclusive=end, payload_offset=start - base,
            raw_hex=span(payload, start - base, end - start).hex()))
    descriptors = []
    for service, address in ((1, 0x20b88), (2, 0x20ba0)):
        raw = span(tcm, address, 24)
        words = list(struct.unpack('<6I', raw))
        descriptors.append(dict(service=service, segment='TCM', link_address=address,
            words=words, raw_hex=raw.hex(), magic=words[0],
            name_address=words[1], name=cstring(tcm, words[1]),
            startup_pointer_cell=0x3389e8 if service == 1 else 0x3389f0,
            startup_pointer_raw_hex=span(dram,
                (0x3389e8 if service == 1 else 0x3389f0) - 0x200000, 4).hex(),
            startup_initializer_offset=8, startup_initializer=words[2],
            word3_is_task_callback=True, exit_member_offset=20,
            captured_exit_member=words[5], other_words_not_runtime_state=True,
            runtime_pointer_and_descriptor_overwrite_exclusion_proven=False))
    result.update(profile_match=True, profile='captured RV55 queued service dispatch ABI 1',
        payload_hashes_gate_both_segments=True, service_registrar=0x30502c,
        callback_table_base=0x1f7c4, callback_table_stride=8,
        common_dispatcher=0x305c4c,
        service1=dict(callback=0x116f2, dispatcher_global=0x29918,
            initializer=0x114e0, task_callback=0x327ea6,
            queue_handle_global=0x1f870, alternate_queue_handle_global=0x1f878,
            queue_entries=2, queue_record_bytes=72,
            outer_service_offset=0, outer_length_offset=4, payload_offset=8,
            worker_receive_call=0x327f06, worker_receive_helper=0x5e08,
            worker_receive_timeout_argument=-1,
            worker_payload_stack_offset=8, direct_dispatch_call=0x327fc2,
            custom_queue_selection_and_control_flow_opaque=True),
        service2=dict(callback=0x118ac, dispatcher_global=0x2991c,
            queue_handle_global=0x1f874, queue_entries=2, queue_record_bytes=72,
            outer_service_offset=0, outer_length_offset=4, payload_offset=8,
            maximum_payload_bytes=64, task_callback=0x328024,
            worker_payload_stack_offset=0x10, direct_dispatch_call=0x3280d2,
            custom_length_selection_and_control_flow_opaque=True),
        task_entry=0x30501e, task_create_helper=0x2470ba,
        task_descriptors=descriptors, instruction_evidence=evidence,
        static_service1_dispatch_edge_proven=True,
        static_service2_dispatch_edge_proven=True,
        named_service_exit=dict(service1=0x327fc8, service2=0x3280d8,
            service1_name_address=0x1c513, service1_name=cstring(tcm, 0x1c513),
            service1_core_res_exit_helper=0x305a8a,
            core_res_task_handle_cells=[0x338a10, 0x338a14],
            core_res_task_exit_calls=[0x305a96, 0x305ac6],
            service_task_exit_call=0x327fd6, service_task_handle_cell=0x29750,
            thread_exit_helper=0x24710e, thread_exit_name_address=0x247f78,
            thread_exit_name=cstring(dram, 0x247f78 - 0x200000),
            thread_delete_call=0x247148, thread_delete_helper=0x753c,
            thread_exit_zero_assignment=0x247160, thread_exit_return=0x247164,
            thread_wrapper_zero_return_does_not_prove_join=True,
            task_delete_vendor_selection_and_cleanup_semantics_unresolved=True,
            task_delete_list_changes_are_not_codec_buffer_retirement=True,
            service_exit_is_not_per_buffer_free_ack=True),
        runtime_queue_ownership_allocation_and_completion_proven=False)
    return result


def rv55_output_evidence(dram, init):
    """Full-payload identity gate; only valid RV32 instruction-boundary spans.

    Incoming common-sender branches include custom opcodes. They are NOT
    asserted to be success paths. Nominal normal-path token/status sources
    are recorded separately from runtime success. Publisher writes do not
    prove cache/IRQ; service dispatch has its own two-payload identity gate.
    """
    result = dict(profile_match=False, runtime_output_registration_proven=False,
                  runtime_ring_completion_proven=False, runtime_pixels_proven=False,
                  runtime_mapping_proven=False, runtime_ready_proven=False)
    if not init['profile_match']:
        return result
    spans = (
        ('a005_dispatch', 0x306016, 0x30601e),
        ('a005_zero40_reply_and_request_firmware_alias', 0x30669e, 0x3066c2),
        ('a005_load_saved_init_ap_token', 0x306ebe, 0x306eda),
        ('init_save_ap_token_to_instance', 0x307200, 0x307262),
        ('a005_output_token_to_shared_vsi', 0x306f40, 0x306f5c),
        ('a005_y_iova_to_shared_vsi', 0x306f5c, 0x306f78),
        ('a005_c_iova_to_shared_vsi', 0x306f78, 0x306f94),
        ('a005_index_general_iova_size_to_shared_vsi', 0x306f94, 0x306fcc),
        ('a005_output_object_calls_nominal_exit', 0x307030, 0x30706e),
        ('b005_error_minus14_zero_token_service2', 0x306796, 0x3067b4),
        ('b005_incoming_branch_not_success_proof', 0x30746a, 0x3074a0),
        ('a005_token_to_free_ring_zero_timestamp_flags', 0x307a04, 0x307a1a),
        ('free_ring_then_common_b005_sender', 0x307a1a, 0x307a28),
        ('b005_instance_send40_explicit_service', 0x30b5fe, 0x30b610),
        ('async_zero40', 0x3096d4, 0x3096e4),
        ('e000_instance_flag_and_shared_sender_jump', 0x30978c, 0x30979c),
        ('e000_shared_token_status0_data_service1_send40', 0x309896, 0x3098b4),
        ('fb_ring_one_empty_slot_guard', 0x30a074, 0x30a09a),
        ('fb_ring_full_path_without_publication', 0x30a09a, 0x30a0c6),
        ('fb_ring_stride24_token_words', 0x30a0f4, 0x30a108),
        ('fb_ring_timestamp_raw_flags_index_count', 0x30a11e, 0x30a13c),
        ('fb_publisher_second_direct_call', 0x309876, 0x309878),
        ('fb_publisher_third_token_timestamp_arguments_and_call', 0x3099d8, 0x3099ee),
        ('bs_ring_nonnull_vsi_and_nonzero_token_gate', 0x30b268, 0x30b276),
        ('bs_ring_count64_full_guard', 0x30b2ae, 0x30b2ba),
        ('bs_ring_full_log_and_no_publication_exit', 0x30b2ca, 0x30b2de),
        ('bs_ring_stride8_and_token_words', 0x30b2de, 0x30b2ec),
        ('bs_ring_write_index_and_count_loads', 0x30b2ec, 0x30b2f8),
        ('bs_ring_count_increment_and_index_count_stores', 0x30b2fe, 0x30b30a),
        ('bs_ring_start_path_query_publish_release_and_ack_jump', 0x306be8, 0x306c0a),
        ('bs_ring_second_query_publish_release_loop', 0x30af9a, 0x30afc6),
    )
    result.update(profile_match=True, profile='captured RV55 output sender/publisher ABI 1',
        a005=dict(message=0xa005, alias_offset=8,
            shared_vsi_writes=[dict(name=name, request_offset=request,
                                   vsi_offset=vsi, width=width)
                for name, request, vsi, width in (
                    ('output_token', 24, 0xf20, 8), ('y_iova', 32, 0xe98, 8),
                    ('c_iova', 40, 0xea0, 8), ('general_iova', 48, 0x1208, 8),
                    ('general_size_s32', 56, 0x1214, 4), ('buffer_index', 60, 0xf2c, 4))],
            static_shared_field_consumption_proven=True,
            object_operation_success_and_custom_branches_proven=False,
            service2_input_dispatch_and_registration_success_proven=False),
        b005=dict(message=0xb005, service=2, bytes=40, observed_error_status=-14,
            error_token_zero=True, observed_error_context_initially_zero=True,
            incoming_branches=[0x30749c, 0x307a24],
            success_status_token_and_context_proven=False,
            nominal_normal_path=dict(saved_ap_token_instance_offset=0x1380,
                saved_ap_token_width=8, token_registers=['s8', 's7'],
                reply_zeroed_at=0x30669e, initial_status=0, initial_context=0,
                object_exit_jump=0x30706a, common_sender_jump=0x307a24,
                token_write_offsets=[8, 12],
                runtime_operation_success_not_inferred=True),
            resource_wakeup_not_completion=True),
        async_notification=dict(message=0xe000, service=1, bytes=40, status=0,
            ap_token_offset=8, data_offset=28, context_initially_zero=True,
            pre_send_instance_byte_offset=0x1002, pre_send_instance_byte_value=1,
            ap_clear_reference='vdec_vcp_if.c async case (source only, captured AP bytes unavailable)',
            flag_is_not_proven_producer_exclusion_or_completion=True,
            one_way=True, no_ack_or_pixel_completion_inferred=True),
        fb_ring=dict(capacity=64, maximum_visible_entries=63,
            full='((write + 1) & 63) == read', stride=24,
            token_offset=0, timestamp_offset=8, raw_flags_offset=20,
            metadata_offsets=[0x600, 0x604, 0x608],
            count='(write - read) & 63', custom_wrap_selection_opaque=True,
            publisher_does_not_write_field_or_frame_type=True,
            free_ring_publication_can_precede_b005=True,
            direct_publication_calls=[0x307a16, 0x309876, 0x3099ec],
            all_direct_indirect_and_other_core_callers_proven=False,
            hardware_completion_and_cache_barrier_proven=False),
        bs_ring=dict(capacity=64, maximum_visible_entries=64,
            full='count == 64', stride=8, token_offset=0,
            metadata_offsets=[0x200, 0x204, 0x208],
            publisher_count='read shared count, add1, store shared count',
            consumer_count='stock reference AP consumer reads shared count, subtracts1, stores',
            consumer_reference='vdec_vcp_if.c:vdec_get_bs (source, not loaded AP proof)',
            equal_indices_and_count64_is_full_not_empty=True,
            count_is_not_fb_derived_distance=True,
            write_index_before_count_store=True,
            cross_endpoint_count_rmw=True,
            direct_publication_calls=[0x306bfa, 0x30afb6],
            normal_start_path_post_publication_jump=0x305fa4,
            start_reply_after_one_call_does_not_exclude_other_publishers=True,
            all_direct_indirect_and_other_core_callers_proven=False,
            live_producer_exclusion_proven=False,
            custom_wrap_selection_opaque=True,
            hardware_completion_and_cache_barrier_proven=False),
        instruction_evidence=[dict(name=name, segment='DRAM', link_address=start,
            end_exclusive=end, payload_offset=start - 0x200000,
            raw_hex=span(dram, start - 0x200000, end - start).hex())
            for name, start, end in spans])
    return result


def rv55_clock_evidence(dram, init):
    """Valid RV32 framing spans only; custom opcode routing remains opaque."""
    result = dict(profile_match=False, runtime_clock_ownership_proven=False,
                  runtime_irq_owner_proven=False, runtime_ready_proven=False)
    if not init['profile_match']:
        return result
    spans = (
        ('clock_buffer_zero40', 0x3086c6, 0x3086e0),
        ('lat_lock_id', 0x3087a4, 0x3087aa),
        ('core_unlock_ao_id', 0x3087b6, 0x3087bc),
        ('core_lock_unlock_id_payload0', 0x3087c0, 0x3087d8),
        ('lat_unlock_id_hardware1', 0x3087dc, 0x3087e8),
        ('core_ao_lock_id_hardware0_payload1', 0x30880a, 0x308818),
        ('clock_token_status0_data_service1_send40', 0x308818, 0x30882e),
    )
    evidence = [dict(name=name, segment='DRAM', link_address=start,
        end_exclusive=end, payload_offset=start - 0x200000,
        raw_hex=span(dram, start - 0x200000, end - start).hex())
        for name, start, end in spans]
    result.update(profile_match=True, profile='captured RV55 clock request framing',
        requests=[0xc001, 0xc002, 0xc003, 0xc004], service=1, bytes=40, status=0,
        token_offset=8, hardware_id_offset=24, data_offset=28, payload_offset=32,
        hardware_ids=dict(core=0, lat=1), core_ao_payload=1,
        custom_selector_branches_opaque=True, reply_success_predicate_not_proven=True,
        instruction_evidence=evidence)
    return result


def rv55_mem_wait_evidence(dram, init):
    """Senders and registered free edges, not a DMA-retirement contract.

    Full-payload INIT identity gates these PC/byte interpretations. RV32
    encoded-length framing was checked independently of r2's invalid-op
    fallback: a bogus call at 0x246a46 is inside an unresolved FOUR-byte
    instruction at 0x246a44. Custom branches, callback-table lifetime, codec
    destruction and transport execution remain unproved. Pre-free list/vote
    mutations and asynchronous task submission do not establish exclusion.
    The conditional hardware-disable helper has a real local register path,
    but its descriptor selector, full dispatcher and physical drain are not
    proved by those writes or by the helper's name.
    """
    result = dict(profile_match=False, runtime_allocation_proven=False,
                  runtime_irq_completion_proven=False, runtime_mapping_proven=False,
                  runtime_free_proven=False, runtime_dma_retirement_proven=False,
                  runtime_cache_handoff_proven=False,
                  runtime_producer_exclusion_proven=False,
                  runtime_hardware_disable_proven=False)
    if not init['profile_match']:
        return result
    spans = (
        ('wait_zero40', 0x308556, 0x308568),
        ('wait_c007_selector_and_id', 0x3085de, 0x3085f6),
        ('wait_service1_send40', 0x3085fa, 0x308608),
        ('allocation_zero64', 0x308ab0, 0x308ac0),
        ('allocation_c005_token_type_length', 0x308ac0, 0x308adc),
        ('allocation_service1_send64', 0x308b18, 0x308b26),
        ('allocation_copy_returned_three_u64_fields', 0x308b28, 0x308b42),
        ('free_zero64', 0x308c28, 0x308c3a),
        ('free_c006_token_copy32_mem_object', 0x308c3a, 0x308c56),
        ('free_service1_send64', 0x308cae, 0x308cbc),
        ('free_callback_address_and_block_slot', 0x30584a, 0x305868),
        ('reverse_callback_block_service1_install', 0x305898, 0x3058a6),
        ('reverse_table_stride96_destination_and_copy', 0x242f02, 0x242f3e),
        ('free_wrapper_size_service_and_ap_token_loads', 0x246962, 0x246978),
        ('free_wrapper_first_callback_lookup', 0x2469bc, 0x2469d0),
        ('free_wrapper_rejected_false_call_encoding', 0x246a44, 0x246a48),
        ('free_wrapper_stack_mem_object32', 0x246ab8, 0x246acc),
        ('free_wrapper_second_lookup_token_and_indirect_call', 0x246ae4, 0x246af6),
        ('a002_discriminator_and_dispatch_edge', 0x305d68, 0x305d70),
        ('a002_instance_source_and_unresolved_branch', 0x306146, 0x306174),
        ('instance_cleanup_codec_deinit_call', 0x306cfa, 0x306d02),
        ('instance_cleanup_codec_release_call', 0x306d28, 0x306d2e),
        ('instance_cleanup_free_wrapper_call', 0x306db6, 0x306dd2),
        ('instance_cleanup_host_fields_and_ack_join', 0x306dd2, 0x306df4),
        ('a002_reply_id_after_cleanup_join', 0x3061aa, 0x3061b6),
        ('codec_deinit_flag_context_and_indirect_call', 0x2707f8, 0x270804),
        ('codec_deinit_clear_software_flag', 0x27080e, 0x270812),
        ('codec_release_object_copy_and_free_call', 0x27088a, 0x2708a4),
        ('native_codec_create_request_and_call', 0x307f88, 0x307fb2),
        ('codec_create_callback_destination_and_selector', 0x270006, 0x270028),
        ('codec_ops_h265_table_materialization', 0x223468, 0x223476),
        ('codec_ops_copy64_destination_and_tcm_call', 0x223490, 0x22349a),
        ('codec_deinit_prefix_helper_is_ret_only', 0x222b70, 0x222b72),
        ('h265_table_deinit_magic_gate_and_destructor_call', 0x2cd5ea, 0x2cd606),
        ('h265_destructor_initial_cleanup_call', 0x2d2688, 0x2d2696),
        ('h265_initial_cleanup_first_free_loop', 0x2f7d6a, 0x2f7dc0),
        ('h265_destructor_sps_free_loop', 0x2d26b6, 0x2d26ea),
        ('h265_destructor_pps_free_loop', 0x2d270a, 0x2d273e),
        ('h265_destructor_three_mem_objects_free_loop', 0x2d273e, 0x2d2786),
        ('h265_destructor_final_helper_and_handle_clear', 0x2d27ee, 0x2d2804),
        ('pre_free_dvfs_instance_key_and_list_search', 0x23ec6e, 0x23ec9c),
        ('pre_free_dvfs_list_detach_and_software_clear', 0x23ef60, 0x23ef7e),
        ('pre_free_vote_byte_and_counter_update', 0x23ac50, 0x23ac7e),
        ('pre_free_trigger_type2_create_and_submit', 0x23ac90, 0x23aca0),
        ('pre_free_job_type_and_context_store', 0x23ad56, 0x23ad68),
        ('pre_free_job_output_store_and_return_zero', 0x23ae0e, 0x23ae40),
        ('pre_free_job_queue_and_submit_branches', 0x23ae7c, 0x23ae98),
        ('pre_free_task_queue_link_and_count_increment', 0x2420d4, 0x2420ee),
        ('pre_free_task_queue_unlock_and_return_zero', 0x24211a, 0x242128),
        ('pre_free_conditional_vote_and_global_flag', 0x30b50c, 0x30b528),
        ('pre_free_optional_disable_call_and_flag_clear', 0x30b53a, 0x30b55e),
        ('pre_free_global_option_byte_load', 0x2259f6, 0x225a00),
        ('pre_free_disable_object_field108_and_tail', 0x26fec4, 0x26feca),
        ('pre_free_disable_descriptor_and_unresolved_selector', 0x2490de, 0x24910c),
        ('pre_free_disable_local_registers_then_dispatch_and_result', 0x24910c, 0x24911e),
        ('pre_free_local_register_write_mask_and_store', 0x2487fc, 0x248822),
        ('pre_free_local_register_read', 0x24882c, 0x24883c),
        ('pre_free_disable_register_read_bit_and_conditional_clear', 0x225938, 0x225964),
        ('pre_free_release_software_counter_decrement', 0x23bc48, 0x23bc4e),
        ('pre_free_release_software_clear_and_unlock', 0x23bc5e, 0x23bc7c),
        ('disable_dispatch_candidate_release_call', 0x248e7e, 0x248e90),
        ('disable_candidate_counted_release_call', 0x224b3a, 0x224b4a),
        ('counted_release_tcm_call_and_success_decrement', 0x22435c, 0x224374),
    )
    # The table is DATA, not decoded instructions. Its deinit slot is established
    # by the object+16 installation and object+44 indirect call. Reaching the
    # table-selection block still depends on unresolved vendor branches.
    ops_table_address = 0x3356b0
    ops_table_raw = span(dram, ops_table_address - 0x200000, 64)
    branch_candidates = []
    for address in (0x223400, 0x223410, 0x306170):
        raw = span(dram, address - 0x200000, 4)
        word = int.from_bytes(raw, 'little')
        displacement = (((word >> 31) & 1) << 12) | (((word >> 7) & 1) << 11)
        displacement |= (((word >> 25) & 63) << 5) | (((word >> 8) & 15) << 1)
        if displacement & 0x1000:
            displacement -= 0x2000
        branch_candidates.append(dict(address=address, raw_hex=raw.hex(),
            opcode=word & 127, funct3=(word >> 12) & 7, rs1=(word >> 15) & 31,
            literal_field=(word >> 20) & 31,
            candidate_standard_B_displacement=displacement,
            candidate_target=address + displacement,
            interpretation='hypothesis using standard B immediate bit layout, NOT decoded JALR',
            vendor_semantics_or_taken_edge_proven=False))
    result.update(profile_match=True, profile='captured RV55 instance mem/wait senders',
        allocation=dict(message=0xc005, service=1, bytes=64, initial_status=0,
            type_offset=24, requested_length_offset=28, address_fields_initially_zero=True,
            post_reply_copies_offsets=[32, 40, 48],
            complete_reply_status_validation_proven=False),
        free=dict(message=0xc006, service=1, bytes=64, copied_mem_bytes=32,
            sender=0x308c1a, wrapper=0x24695a, wrapper_object_bytes=88,
            callback_registration=dict(service=1, install_call=0x3058a2,
                installer=0x242f00, block_stack_offset=24,
                free_block_stack_offset=56, free_callback_offset=32,
                table_base=0x32c0f0, table_stride=96,
                service1_free_callback_cell=0x32c170,
                registered_target=0x308c1a,
                runtime_registration_and_table_overwrite_exclusion_proven=False),
            indirect_call=dict(address=0x246af4, callback_register='a3',
                callback_offset=32, object_service_offset=64,
                object_ap_token_offsets=[80, 84], request_object_stack_offset=24,
                callback_arguments=['AP token low', 'AP token high', 'mem object pointer'],
                table_select='0x32c0f0 + object.service * 96',
                registered_target_edge_is_conditional_on_table_contents=True,
                wrapper_control_flow_complete=False, runtime_call_proven=False),
            instance_cleanup=dict(free_call=0x306dce,
                object_offset=0x1990, presence_field_offset=0x198c,
                codec_deinit_call=0x306cfc, codec_release_call=0x306d2a,
                deinit_wrapper=0x2707d2, release_wrapper=0x270874,
                a002_dispatch_edge=0x305d6c, a002_entry=0x306106,
                cleanup_reply_join=0x3061aa, reply_id=0xb002,
                a002_to_cleanup_control_flow_proven=False,
                free_result_checked_before_cleanup_join=False,
                codec_deinit_indirect_call=0x270802,
                codec_deinit_callback_offset=44, codec_context_offset=80,
                codec_software_flag_offset=84,
                codec_deinit_target_and_dma_drain_proven=False,
                release_copy_source_offset=640, release_copy_bytes=88,
                release_free_call=0x2708a0),
            codec_ops_and_destructor=dict(native_create_call=0x307faa,
                create_wrapper=0x26ff2c, ops_selector=0x2233ec,
                object_ops_offset=16, copied_ops_bytes=64, tcm_copy_target=0x701c,
                table=dict(address=ops_table_address, bytes=64,
                    words=list(struct.unpack('<16I', ops_table_raw)),
                    raw_hex=ops_table_raw.hex(), deinit_offset=28,
                    deinit_target=struct.unpack_from('<I', ops_table_raw, 28)[0],
                    selected_table_runtime_proven=False,
                    codec_candidate_selection_control_flow_complete=False),
                deinit_wrapper=0x2cd5e6, required_context_magic=0xabba4321,
                destructor_call=0x2cd5fe, destructor=0x2d2648,
                destructor_name_address=0x2d6532,
                destructor_name=cstring(dram, 0x2d6532 - 0x200000),
                wrapper_reports_one_without_checking_destructor_result=True,
                initial_cleanup=0x2f7d46, initial_cleanup_call=0x2d2692,
                initial_cleanup_first_free_call=0x2f7dac,
                initial_cleanup_first_loop=dict(object_base_offset=0x1b38,
                    object_stride=88, objects=7, presence_u64_offset=56,
                    skip_if_object_field72_nonzero=True,
                    object_roles_and_dma_retirement_proven=False),
                destructor_mem_free_call=0x2d2772,
                destructor_mem_loop=dict(object_base_offset=0x1840,
                    object_stride=88, objects=3, presence_u64_offset=56,
                    object_roles_and_dma_retirement_proven=False),
                sps_cleanup=dict(entries=32, call=0x2d26da, callee=0x282d16),
                pps_cleanup=dict(entries=256, call=0x2d272e, callee=0x284d72),
                software_counts_are_not_hardware_capacity=True,
                deinit_prefix_helper=0x222b70, prefix_helper_is_ret_only=True,
                destructor_complete_control_flow_and_dma_drain_proven=False),
            pre_free_helpers=dict(
                dvfs_list=dict(wrapper=0x23ef84, detach=0x23ef56,
                    find=0x23ec6a, context_key_offset=0x12e0,
                    detach_node_links_offsets=[92, 96],
                    context_halfword_clear_offset=0x1006,
                    name_address=0x23f7e0,
                    name=cstring(dram, 0x23f7e0 - 0x200000),
                    list_detach_is_not_dma_drain=True),
                software_vote=dict(helper=0x23ac3a,
                    context_index_offset=4, byte_table=0x382a00,
                    counter=0x382a10, unresolved_counter_op=0x23ac74,
                    counter_decrement_semantics_complete=False,
                    lock_and_software_vote_are_not_producer_exclusion=True),
                task_trigger=dict(helper=0x23ac86, name_address=0x23d526,
                    name=cstring(dram, 0x23d526 - 0x200000),
                    create_call=0x23ac96, create=0x23ad20, type_argument=2,
                    job_type_offset=4, job_context_offset=16,
                    submit_call=0x23ac9e, submit=0x23ae48,
                    queue=0x382c10, queue_submit=0x241ff4,
                    queue_submit_edge=0x23ae8e,
                    queue_job_link_stores=[0x2420de, 0x2420e2],
                    queue_count_increment=0x2420ec,
                    create_and_submit_are_not_worker_join=True,
                    worker_consumption_and_producer_exclusion_proven=False),
                conditional_hardware_disable=dict(wrapper=0x30b4e6,
                    object_offset=0x1390, global_option_byte=0x329730,
                    per_index_option_table=0x3e1830,
                    object_call=0x30b54e, id_argument=5, data_argument=1,
                    object_helper=0x26fec4, object_field_offset=108,
                    disable_helper=0x2490be, name_address=0x24b0ef,
                    name=cstring(dram, 0x24b0ef - 0x200000),
                    descriptor_stack_offset=0,
                    descriptor_fields=dict(object=0, literal480=4,
                        literal1000=12, unresolved_selector=16,
                        object_field380=20, zero_byte=24, data_argument=28),
                    selector_encoding=0x2490e8,
                    selector_value_and_field_roles_proven=False,
                    local_register_helper_call=0x24910c,
                    local_register_helper=0x2258e2,
                    local_masked_write=0x2487e2, local_read=0x248828,
                    conditional_read_bit_clear_call=0x225960,
                    dispatcher_call=0x249112, dispatcher=0x24897c,
                    dispatcher_result_returned_by_helper=True,
                    result_checked_by_outer_wrapper=False,
                    outer_option_clear=0x30b558,
                    optional_route_and_software_clear_are_not_retirement=True,
                    full_dispatcher_and_dma_drain_proven=False,
                    candidate_release=dict(dispatcher_call=0x248e84,
                        helper=0x224aa0, counted_call=0x224b46,
                        counted_helper=0x224308, tcm_call=0x22435e,
                        tcm_target=0x20f8, decrement_store=0x224370,
                        clock_name_address=0x226d3e,
                        clock_name=cstring(dram, 0x226d3e - 0x200000),
                        named_venc_candidate_is_not_a_proven_vdec_stop=True,
                        dispatcher_to_candidate_control_flow_proven=False,
                        tcm_target_semantics_and_dma_drain_proven=False)),
                software_release=dict(helper=0x23bc28,
                    candidate_counters=[0x3829f0, 0x3829f4],
                    counter_selection_vendor_branches_unresolved=True,
                    zero_store=0x23bc6c, unresolved_address_op=0x23bc68,
                    complete_address_selection_proven=False,
                    release_is_not_firmware_or_hardware_exit=True),
                producer_exclusion_and_buffer_retirement_proven=False),
            unresolved_branch_layout_candidates=branch_candidates,
            raw_request_or_software_deinit_is_retirement_authority=False,
            transport_return_is_hardware_drain_proof=False,
            unsafe_false_call=dict(address=0x246a46, real_encoding_address=0x246a44,
                real_encoding_bytes=4, rejected_as_instruction_interior=True)),
        wait=dict(message=0xc007, service=1, bytes=40, hardware_selector_offset=16,
            id_offset=24, id_formula='hardware_selector == 1 ? 1 : 0',
            timeout_in_request=False, irq_or_pixels_proven=False),
        custom_opcode_control_flow_complete=False,
        instruction_evidence=[dict(name=name, segment='DRAM', link_address=start,
            end_exclusive=end, payload_offset=start - 0x200000,
            raw_hex=span(dram, start - 0x200000, end - start).hex())
            for name, start, end in spans])
    return result


def global_boundary_evidence(rv55, rv33):
    """Both cores consume +4c as a protection boundary, not an allocation.

    Match complete DECLARED payloads (no 512B header/alignment padding) before
    attaching the captured instruction interpretation. Custom ops and runtime
    residency/permission are deliberately not inferred from this static path.
    """
    result = dict(profile_match=False, runtime_residency_proven=False,
        runtime_mapping_proven=False, protection_attributes_fully_decoded=False,
        second_logger_extent_proven=False)
    profiles = (
        ('RV55', rv55, 184320, 'f59d646ba74fab1c40500139c40d67c97a0e8ab96c1569f738a0c7403cc49508'),
        ('RV33', rv33, 297984, '20f8310787c20782548d4561ccb30b93b6fdd27217fdecc040f7e5ea4abac692'),
    )
    if any(data is None or len(data) != size or hashlib.sha256(data).hexdigest() != digest
           for _, data, size, digest in profiles):
        return result
    spans = (
        ('RV55', 'cfg44_48_4c_loads_and_packed_split', 0xff62, 0xff82),
        ('RV55', 'global_start_id9_logger_end_id10_global_end_id11', 0xffbc, 0xffe4),
        ('RV55', 'region_id_lookup_stride12_and_boundary_store', 0x10e52, 0x10e72),
        ('RV33', 'cfg44_4c_48_loads_and_packed_split', 0x154c0, 0x154fc),
        ('RV33', 'id10_global_start_to_logger_start', 0x154fc, 0x15518),
        ('RV33', 'id11_logger_start_to_global_end', 0x15518, 0x15540),
        ('RV33', 'region_id_lookup_stride16_and_lower_upper_stores', 0x15eb8, 0x15efc),
    )
    payloads = {'RV55': rv55, 'RV33': rv33}
    result.update(profile_match=True, profile='captured dual-core global protection boundaries 1',
        payload_profiles=[dict(core=core, bytes=size, sha256=digest)
                          for core, _, size, digest in profiles],
        firmware_cfg_views=dict(RV55=0x2120a000, RV33=0x2120d000),
        ap_cfg_views_are_different=True,
        rv55=dict(converter=0x10fdc, setter=0x10e52, table_stride=12,
            boundaries={9: 'global start alias', 10: 'logger end alias', 11: 'global end alias'}),
        rv33=dict(converter=0x15f64, setter=0x15eb8, table_stride=16, table_entries=16,
            regions={10: ['global start alias', 'logger start alias'],
                     11: ['logger start alias', 'global end alias']}),
        logger_end_does_not_grant_next_mblock=True,
        custom_opcode_control_flow_complete=False,
        instruction_evidence=[dict(core=core, name=name, payload_offset=start,
            link_address=start, end_exclusive=end,
            raw_hex=span(payloads[core], start, end-start).hex())
            for core, name, start, end in spans])
    return result


def rv55_clock_install_evidence(tcm, dram, init):
    """Captured domain/clock objects and registration, not live DMA authority.

    Both full payload identities gate the packed-object and PC interpretations.
    The real registration stores do not prove that boot called the initializer,
    that its vendor-indexed vector was populated correctly, or that a later
    lookup reaches the captured callbacks. A VENC-named candidate in the shared
    dispatcher must not be promoted into a VDEC stop edge. The timeout callback
    contains register reads/logging, not a substitute clock disable or drain.
    The voter writes a mask and polls registers, but its error frontier calls
    an ASSERT/ECALL path before the return sequence. Neither a normal error
    return nor the diagnostic fallback is guaranteed by these image bytes.
    """
    result = dict(profile_match=False, runtime_registration_proven=False,
                  runtime_callback_target_proven=False,
                  runtime_decoder_stop_proven=False,
                  runtime_producer_exclusion_proven=False,
                  runtime_dma_retirement_proven=False,
                  scalar_value_helper_vendor_semantics_proven=False,
                  vote_register_address_and_mask_semantics_proven=False,
                  vote_delay_time_unit_proven=False,
                  vote_assert_trap_disposition_proven=False,
                  startup_hart_ordering_and_vdec_owner_proven=False)
    if (not init['profile_match'] or tcm is None or len(tcm) != 184320 or
            hashlib.sha256(tcm).hexdigest() !=
            'f59d646ba74fab1c40500139c40d67c97a0e8ab96c1569f738a0c7403cc49508'):
        return result
    spans = (
        ('DRAM', 'vdec_clock_lookup2_and_store', 0x223ad2, 0x223ae8),
        ('DRAM', 'vdec_clock_lookup0_1_and_stores', 0x223ae8, 0x223b0a),
        ('DRAM', 'venc_clock_lookup4_7_and_stores', 0x223c2e, 0x223c52),
        ('DRAM', 'venc_clock_lookup9_11_13_and_stores', 0x223c52, 0x223c7a),
        ('DRAM', 'venc_clock_software_index_count_setup', 0x223c7a, 0x223c8a),
        ('DRAM', 'vdec_named_counted_release_candidate', 0x224556, 0x224572),
        ('DRAM', 'vdec_soc_named_counted_release_candidate', 0x22458a, 0x2245a4),
        ('DRAM', 'venc_named_counted_release_candidate', 0x224b3a, 0x224b4a),
        ('TCM', 'domain_vector_create_arguments', 0x9170, 0x9184),
        ('TCM', 'domain_object_vector_unresolved_index_and_stride', 0x9184, 0x91ac),
        ('TCM', 'domain0_registration_call_and_result', 0x91ac, 0x91cc),
        ('TCM', 'domain_register_bound_stride_and_stores', 0x2324, 0x2350),
        ('TCM', 'clock_lookup_domain_table_and_unresolved_index', 0x19ba, 0x19ec),
        ('TCM', 'unprepare_callback_block24', 0x1dbc, 0x1dd4),
        ('TCM', 'disable_callback_block8', 0x1fba, 0x1fd2),
        ('TCM', 'unprepare_clock_vote_callback_prefix', 0x38f0, 0x3932),
        ('TCM', 'unprepare_clock_vote_callback_tail', 0x3960, 0x397a),
        ('TCM', 'fallback_block64_call_on_minus25', 0x2120, 0x2140),
        ('TCM', 'fallback_named_register_read_diagnostics', 0x3d06, 0x3da6),
        ('TCM', 'register_read_helper', 0x921c, 0x9220),
        ('TCM', 'vote_inputs_and_ordinary_index_bound', 0x2d64, 0x2d98),
        ('TCM', 'vote_map_and_unresolved_address_selection', 0x2db8, 0x2de6),
        ('TCM', 'vote_invalid_mask_minus22', 0x2e76, 0x2e84),
        ('TCM', 'vote_special_index52_53_54_selection', 0x2e84, 0x2e9e),
        ('TCM', 'vote_masked_poll627_and_opaque_limit_branch', 0x2ed8, 0x2ef2),
        ('TCM', 'vote_first_mask_write', 0x2f3e, 0x2f48),
        ('TCM', 'vote_first_postwrite_poll14', 0x2f48, 0x2f6e),
        ('TCM', 'vote_second_mask_write', 0x2f98, 0x2fa2),
        ('TCM', 'vote_second_postwrite_poll14', 0x2fa2, 0x2fc6),
        ('TCM', 'vote_final_masked_poll_and_opaque_limit_branch', 0x3154, 0x317e),
        ('TCM', 'vote_zero_status_after_release_helper', 0x31a0, 0x31ac),
        ('TCM', 'vote_poll626_vendor_bit_test_and_countdown', 0x3288, 0x32b0),
        ('TCM', 'vote_minus30_joins_error_frontier', 0x338c, 0x3390),
        ('TCM', 'vote_minus25_log_and_interrupt_mask_frontier', 0x33ee, 0x340a),
        ('TCM', 'vote_assert_file_line_arguments_and_call', 0x340a, 0x3418),
        ('TCM', 'vote_status_return_with_vendor_epilogue', 0x3418, 0x3426),
        ('TCM', 'register_write_interrupt_mask_and_vendor_fence', 0x9204, 0x921c),
        ('TCM', 'vote_delay_counter_poll_and_vendor_arithmetic', 0x15ed4, 0x15f10),
        ('TCM', 'assert_entry_and_file_line_log', 0x10266, 0x10290),
        ('TCM', 'assert_task_file_line_log', 0x102ee, 0x10304),
        ('TCM', 'assert_module_log_and_ecall50_tail', 0x1035a, 0x10378),
        ('TCM', 'ecall_argument_moves_trap_and_return', 0x114d0, 0x114e0),
        ('TCM', 'startup_hartid_zero_and_opaque_nonzero_selector', 0xfdc6, 0xfde0),
        ('TCM', 'startup_nonzero_hart_vdec_init_candidate_call', 0xfdec, 0xfdfe),
        ('TCM', 'startup_zero_hart_platform_init_call', 0xfe3c, 0xfe4a),
        ('TCM', 'platform_init_domain_registration_call', 0x11072, 0x11092),
        ('DRAM', 'vdec_init_preceding_callbacks_and_clock_lookup_init', 0x31694c, 0x316974),
        ('TCM', 'initial_hart_helper_csr_write_and_return', 0x16344, 0x1634a),
    )
    objects = []
    for index in range(14):
        address = 0x20018 + index * 59
        raw = span(tcm, address, 59)
        name_address = struct.unpack_from('<I', raw, 8)[0]
        objects.append(dict(index=index, address=address, bytes=59,
            raw_hex=raw.hex(), id=struct.unpack_from('<I', raw, 0)[0],
            name_address=name_address, name=cstring(tcm, name_address),
            flags=struct.unpack_from('<I', raw, 12)[0],
            ops_address=struct.unpack_from('<I', raw, 24)[0],
            initial_counter_bytes=list(raw[28:30]),
            layout_and_initial_bytes_are_not_live_vector_proof=True))
    ops = span(tcm, 0x18100, 68)
    result.update(profile_match=True,
        profile='captured RV55 clock domain0 registration and packed objects',
        domain_registration=dict(initializer=0x9154,
            initializer_runtime_called_proven=False,
            initializer_static_caller=0x1107a, caller_helper=0x11030,
            vector_create_call=0x917e, vector_create=0x22d4,
            entries=14, vector_storage=0x24688,
            object_base=0x20018, object_stride=59,
            vendor_index_store=0x91a0, vector_population_semantics_complete=False,
            registration_call=0x91ba, registration=0x2322,
            domain_argument=0, name_address=0x18901,
            name=cstring(tcm, 0x18901), register_base_argument=0x31400000,
            table_base=0x20c10, table_stride=12,
            table_name_store=0x234c, table_vector_store=0x234e,
            domain_registration_is_not_dma_ownership=True),
        captured_objects=objects,
        ops_table=dict(address=0x18100, bytes=68, raw_hex=ops.hex(),
            words=list(struct.unpack('<17I', ops)),
            unprepare_offset=24, unprepare_target=struct.unpack_from('<I', ops, 24)[0],
            disable_offset=8, captured_disable_target=struct.unpack_from('<I', ops, 8)[0],
            fallback_offset=64, fallback_target=struct.unpack_from('<I', ops, 64)[0],
            runtime_overwrite_exclusion_proven=False),
        clock_lookup=dict(helper=0x19a4, table_base=0x20c10,
            table_vector_offset=4, vendor_domain_id_selection_unresolved=True,
            scalar_value_helper=0x1449c,
            scalar_combine_vendor_encodings_unresolved=True),
        vdec_initialization=dict(helper=0x223a34,
            helper_static_caller=0x31696a, caller_helper=0x316948,
            lookup_ids=[2, 0, 1], result_cells=[0x3296a0, 0x3296a4, 0x3296a8],
            release_candidate=0x2244ba,
            named_release_calls=[0x224570, 0x22459e],
            clock_name_addresses=[0x226c43, 0x226c36],
            clock_names=[cstring(dram, address - 0x200000)
                for address in (0x226c43, 0x226c36)],
            complete_dispatcher_route_and_live_target_proven=False),
        venc_initialization=dict(helper=0x223b0e,
            lookup_ids=[4, 7, 9, 11, 13],
            result_cells=[0x3296ac, 0x3296b0, 0x3296b4, 0x3296b8, 0x3296cc],
            software_index_table=0x3296d4, release_candidate=0x224aa0,
            clock_name_address=0x226d3e,
            clock_name=cstring(dram, 0x226d3e - 0x200000),
            encoder_named_candidate_is_not_certified_decoder_stop=True),
        callback_boundary=dict(unprepare_indirect_call=0x1dd2,
            disable_indirect_call=0x1fd0,
            counted_gates_can_bypass_callback=True,
            helper_zero_return_is_not_callback_execution_proof=True,
            unprepare_vote_tail=0x3976, vote_helper=0x2d64,
            full_vote_control_flow_and_hardware_exit_proven=False,
            fallback_name_address=0x17fb0,
            fallback_name=cstring(tcm, 0x17fb0),
            fallback_register_read=0x921c, fallback_log_helper=0x1712a,
            fallback_diagnostic_is_not_recovery_or_drain=True,
            minus25_return_and_fallback_execution_not_guaranteed=True),
        vote_register_sequence=dict(helper=0x2d64,
            argument_mask_move=0x2d6c, argument_mask_source='a3',
            ordinary_index_upper_bound=51, special_indices=[52, 53, 54],
            mapping_helper=0x91d2,
            mapping_and_vendor_address_arithmetic_unresolved=True,
            register_read_helper=0x921c, register_write_helper=0x9204,
            mask_write_calls=[0x2f42, 0x2f9c], mask_write_source='s0',
            register_write_store=0x9214,
            register_write_interrupt_mask=8,
            register_write_interrupt_mask_bit=3,
            register_write_restores_saved_mstatus=True,
            register_write_vendor_fence=0x9210,
            vendor_fence_semantics_proven=False,
            observed_poll_counter_literals=[627, 626, 14],
            poll_limit_branches_include_opaque_vendor_encodings=True,
            final_masked_read=0x3156, final_mask_compare=0x315c,
            release_helper_call=0x31a4, release_helper=0x161ae,
            zero_status_assignment=0x31a8, zero_status_jump=0x31aa,
            status_move_to_a0=0x3418, return_instruction=0x3424,
            zero_status_path_is_not_buffer_retirement=True,
            complete_control_flow_proven=False),
        vote_delay=dict(helper=0x15ed4, observed_poll_argument=16,
            multiply_helper_call=0x15edc, multiply_helper=0x12a3a,
            literal_factor=26, counter_pointer_cell=0x29b6c,
            counter_load_offset=8, repeated_counter_load=0x15ef2,
            counter_difference_branch=0x15f08,
            arithmetic_has_unresolved_vendor_encodings=True,
            timer_calibration_and_time_unit_proven=False,
            polling_is_not_dma_drain=True),
        vote_error_frontier=dict(negative_status_literals=[-22, -29, -30, -25],
            common_log=0x3402, log_helper=0x1712a,
            log_format_address=0x17a23, log_format=cstring(tcm, 0x17a23),
            interrupt_mask_instruction=0x3406, interrupt_mask=8,
            assert_call=0x3414, assert_helper=0x10266,
            assert_file_address=0x179f8, assert_file=cstring(tcm, 0x179f8),
            assert_line_argument=699,
            assert_file_line_format_address=0x1b616,
            assert_file_line_format=cstring(tcm, 0x1b616),
            assert_task_file_line_format_address=0x1b5e3,
            assert_task_file_line_format=cstring(tcm, 0x1b5e3),
            assert_ecall_number=50, assert_ecall_tail=0x10374,
            ecall_wrapper=0x114d0, ecall_instruction=0x114da,
            ecall_return_instruction=0x114de,
            trap_handler_and_return_disposition_proven=False,
            error_is_not_proven_normal_recoverable_return=True,
            post_assert_vendor_epilogue_semantics_unresolved=True),
        startup_hart_split=dict(helper_candidate=0xfdc6,
            hartid_read=0xfdca, hartid_stack_store=0xfdce,
            initial_hart_helper_call=0xfdd2, initial_hart_helper=0x16344,
            initial_hart_helper_csr_write=0x16344, initial_hart_helper_csr=0x9c7,
            initial_hart_helper_return=0x16348,
            initial_hart_helper_is_not_proven_registry_ready_barrier=True,
            zero_hart_branch=0xfdd8, zero_hart_target=0xfdfe,
            nonzero_hart_selector=0xfddc,
            nonzero_selector_vendor_semantics_unresolved=True,
            zero_hart_platform_init_call=0xfe3e,
            platform_init_helper=0x11030, domain_init_call=0x1107a,
            domain_init_helper=0x9154,
            nonzero_hart_vdec_init_candidate_call=0xfdf8,
            candidate_call_auipc=0xfdf4, vdec_init_helper=0x316948,
            vdec_clock_init_call=0x31696a, vdec_clock_init_helper=0x223a34,
            vdec_pre_init_callback_cells=[0x3389e8, 0x3389f0],
            vdec_pre_init_callback_calls=[0x316962, 0x316968],
            vdec_pre_init_callback_offset=8,
            captured_pre_init_descriptors=[0x20b88, 0x20ba0],
            captured_pre_init_names=[cstring(tcm, 0x1c3b0), cstring(tcm, 0x1c3bd)],
            captured_pre_init_callbacks=[0x114e0, 0x11624],
            captured_callbacks_initialize_software_services=True,
            service_initializers_are_not_registry_ordering_proof=True,
            pre_init_callback_identity_and_runtime_target_proven=False,
            startup_entry_execution_proven=False,
            cross_hart_registry_visibility_and_ordering_proven=False,
            separate_hart_paths_are_not_sequential_boot_proof=True),
        instruction_evidence=[dict(segment=segment, name=name,
            link_address=start, end_exclusive=end,
            payload_offset=start - (0x200000 if segment == 'DRAM' else 0),
            raw_hex=span(dram if segment == 'DRAM' else tcm,
                start - (0x200000 if segment == 'DRAM' else 0), end-start).hex())
            for segment, name, start, end in spans])
    return result


def rv55_vdec_irq_evidence(tcm, dram, init):
    """Packed registration -> table consumer -> named ISR/ACK/signal edges.

    Full payload identity and independently framed PCs constrain this static
    interpretation. Descriptor bytes 1/2 are NOT assigned priority/affinity
    meanings. The initializer does not explicitly set a1 after registration;
    the separate named enable helper does, after reconfiguring the descriptor.
    The mapping helper translates AP-address arguments through a captured table;
    those arguments are not literal RV55 pointers or an authorized AP mapping.
    ISR status/ACK and a shared semaphore count do not prove token association,
    producer exclusion, DMA drain or retirement of any codec buffer.
    The registered service1 pfnWaitISR sends c007; a separately captured H265
    HAL call goes through the local shared wait helper. Neither registration
    nor a wrapper's literal return proves that route ran or that a read
    succeeded. The post-wait scalar is a named HEVC VLD word75 read. Captured
    context writers bind a fixed common-HAL object whose initializer installs a
    plain word-load callback; the initializer and AP-address table selection
    are not runtime mapping authority. The visible null-handle fallback and
    split scalar are not a successful buffer read or a buffer token. Captured
    post-wait uses call them LCU coordinates and compare them against two
    context bounds before updating byte85. The embedded hardware instance
    points back to its codec owner; that owner's candidate H265 ops+60 callback
    skips its downstream helper with the wait path's nonzero a2 argument.
    Separate Free/Display FB software lists have visible producers and clearing
    consumers. The candidate selector/ops route connects them to shared-ring
    publication, but vendor branches, live ownership and DMA retirement do not
    follow from those software writes or from the publisher's return.
    """
    result = dict(profile_match=False, runtime_registration_proven=False,
                  runtime_dispatch_target_proven=False,
                  runtime_irq_enable_proven=False,
                  runtime_irq_ack_proven=False,
                  runtime_mapping_proven=False,
                  runtime_semaphore_wakeup_proven=False,
                  runtime_semaphore_consumption_proven=False,
                  runtime_semaphore_reset_proven=False,
                  runtime_wait_instance_association_proven=False,
                  runtime_registered_reverse_wait_call_proven=False,
                  runtime_h265_local_wait_call_proven=False,
                  runtime_h265_context_instance_install_proven=False,
                  runtime_common_hal_object_install_proven=False,
                  runtime_common_hal_scalar_callback_target_proven=False,
                  runtime_h265_create_ops_consumer_proven=False,
                  runtime_h265_post_wait_callback_target_proven=False,
                  runtime_h265_lcu_result_state_mutation_proven=False,
                  runtime_h265_fb_list_to_ring_route_proven=False,
                  runtime_buffer_completion_proven=False,
                  runtime_producer_exclusion_proven=False,
                  runtime_dma_retirement_proven=False)
    if (not init['profile_match'] or tcm is None or len(tcm) != 184320 or
            hashlib.sha256(tcm).hexdigest() !=
            'f59d646ba74fab1c40500139c40d67c97a0e8ab96c1569f738a0c7403cc49508'):
        return result
    spans = (
        ('DRAM', 'vdec_shared_semaphore_creation_arguments', 0x223a36, 0x223a56),
        ('DRAM', 'vdec_lat_descriptor_registration_and_control', 0x223a56, 0x223a7a),
        ('DRAM', 'vdec_core_descriptor_registration_and_control', 0x223a7a, 0x223a9e),
        ('DRAM', 'vdec_third_descriptor_registration_and_control', 0x223a9e, 0x223ac4),
        ('DRAM', 'third_descriptor_callback_is_ret_only', 0x223a30, 0x223a32),
        ('TCM', 'descriptor_validation_vendor_branches', 0x421c, 0x4240),
        ('TCM', 'descriptor_callback_context_stores_and_failure_clear', 0x4240, 0x4274),
        ('TCM', 'descriptor_registration_result_and_vendor_epilogue', 0x4274, 0x4288),
        ('TCM', 'descriptor_unpack_three_fields_and_config_call', 0x3fb0, 0x3fd2),
        ('TCM', 'descriptor_config_index_bound_and_vendor_field_checks', 0x3eaa, 0x3ebe),
        ('TCM', 'descriptor_config_bank_mask_and_vendor_register_address', 0x3ebe, 0x3efe),
        ('TCM', 'descriptor_config_field2_register_or_and_paths', 0x3efe, 0x3f3c),
        ('TCM', 'descriptor_config_field1_routing_register', 0x3f3c, 0x3f68),
        ('TCM', 'descriptor_config_final_register_and_vendor_selector', 0x3f68, 0x3fb0),
        ('TCM', 'descriptor_control_mask_and_nonzero_zero_register_paths', 0x4056, 0x40c0),
        ('TCM', 'dispatch_hart_gate_and_source_call', 0x4288, 0x42a6),
        ('TCM', 'dispatch_stride40_callback_and_context_loads', 0x42ae, 0x42ce),
        ('TCM', 'dispatch_context_argument_and_indirect_call', 0x42f6, 0x4316),
        ('TCM', 'dispatch_source_registers_and_vendor_index_calculation', 0x43f8, 0x443e),
        ('TCM', 'ap_address_mapping_lookup_and_vendor_selection', 0x10fdc, 0x11030),
        ('TCM', 'lat_isr_addresses_and_status_mask', 0x15208, 0x15230),
        ('TCM', 'lat_isr_status_logs_and_ack_tail', 0x15230, 0x15266),
        ('TCM', 'lat_isr_ack_and_conditional_signal_frontier', 0x15294, 0x152aa),
        ('TCM', 'core_isr_addresses_and_status_mask', 0x150dc, 0x15104),
        ('TCM', 'core_isr_status_logs_and_ack_tail', 0x15104, 0x1513a),
        ('TCM', 'core_isr_ack_and_conditional_signal_frontier', 0x15168, 0x1517e),
        ('TCM', 'lat_ack_register_rmw_and_hw_pulse', 0x1517e, 0x15208),
        ('TCM', 'core_ack_register_rmw_and_hw_pulse', 0x15052, 0x150dc),
        ('TCM', 'shared_semaphore_signal_isr_thread_paths', 0x14fda, 0x15052),
        ('TCM', 'isr_signal_queue_count_bound_and_increment', 0x6d92, 0x6dac),
        ('TCM', 'isr_signal_waiter_and_vendor_wakeup_path', 0x6dec, 0x6e08),
        ('DRAM', 'semaphore_create_handle_store_and_result', 0x246fb2, 0x246ffe),
        ('TCM', 'static_queue_capacity_item_size_and_reset_call', 0x660a, 0x66c2),
        ('TCM', 'queue_reset_count_zero_and_wait_list_initialization', 0x6592, 0x660a),
        ('TCM', 'queue_count_read_under_paired_helpers', 0x6f44, 0x6f6e),
        ('TCM', 'queue_take_argument_and_item_size_gates', 0x68e2, 0x6934),
        ('TCM', 'queue_take_nonzero_count_decrement_and_waiter', 0x6934, 0x695e),
        ('TCM', 'queue_take_block_wait_and_count_recheck', 0x69e0, 0x6a04),
        ('TCM', 'queue_take_result_and_vendor_epilogue', 0x6a1a, 0x6a28),
        ('DRAM', 'wait_isr_argument_object_context_and_initial_status_clear', 0x246b82, 0x246b9a),
        ('DRAM', 'wait_isr_vdec_venc_cells_and_vendor_index', 0x246c1c, 0x246c54),
        ('DRAM', 'wait_isr_context_profiling_stop_after_local_wait', 0x246c54, 0x246c62),
        ('DRAM', 'wait_isr_conditional_shared_status_to_argument', 0x246ca2, 0x246cb4),
        ('DRAM', 'timed_wait_handle_duration_arithmetic_and_tcm_take', 0x246cee, 0x246d46),
        ('DRAM', 'vdec_disable_irq_lat_descriptor_and_call', 0x2253c4, 0x2253d8),
        ('DRAM', 'vdec_disable_irq_lat_pending_count_and_take', 0x2253f6, 0x225430),
        ('DRAM', 'vdec_disable_irq_core_descriptor_and_call', 0x225436, 0x225446),
        ('DRAM', 'vdec_disable_irq_core_pending_count_and_take', 0x225464, 0x22549e),
        ('DRAM', 'vdec_disable_irq_common_tail_arguments', 0x22549e, 0x2254a8),
        ('TCM', 'disable_descriptor_to_mask_helper', 0x403c, 0x4056),
        ('TCM', 'disable_mask_registers_and_vendor_bit_operations', 0x3fd2, 0x403c),
        ('DRAM', 'wait_isr_vdec_pre_wait_tcm_control_call', 0x246c0a, 0x246c1c),
        ('DRAM', 'wait_isr_vdec_post_wait_tcm_control_call', 0x246c2e, 0x246c44),
        ('DRAM', 'context_profiling_begin_timestamp_and_flag_set', 0x2433ac, 0x24340a),
        ('DRAM', 'context_profiling_stop_timestamp_difference_callback_and_flag_clear', 0x243534, 0x2435d6),
        ('DRAM', 'named_video_time_difference_arithmetic_and_overflow_logs', 0x2435d6, 0x24365c),
        ('DRAM', 'timestamp_helper_two_word_output_and_arithmetic_literals', 0x2463c4, 0x246438),
        ('DRAM', 'context_callback_pair_table_registration_stores', 0x242f00, 0x242f30),
        ('DRAM', 'named_irq_enable_reconfigure_and_explicit_nonzero_control', 0x223886, 0x223968),
        ('DRAM', 'other_codec_shared_queue_create_and_status_writer_registration', 0x223b0e, 0x223b78),
        ('TCM', 'other_codec_irq_mapped_status_write_and_shared_signal', 0x155cc, 0x15640),
        ('TCM', 'wla_pre_wait_history_reload_counter_and_register_paths', 0xc40a, 0xc516),
        ('TCM', 'wla_post_wait_history_reload_counter_and_register_paths', 0xc31c, 0xc40a),
        ('TCM', 'wla_paired_control_enter_hart_accounting_and_lock_call', 0x160ec, 0x1613c),
        ('TCM', 'wla_paired_control_leave_hart_accounting_and_restore_tail', 0x161ae, 0x161f8),
        ('TCM', 'wla_control_unlock_word_zero_store', 0x1613c, 0x16170),
        ('TCM', 'wla_control_leave_unlock_call_and_scheduler_frontier', 0x16170, 0x161ae),
        ('DRAM', 'service1_callback_block_zero_and_wait_lock_addresses', 0x305812, 0x30584a),
        ('DRAM', 'service1_callback_block_install_call', 0x305898, 0x3058a6),
        ('DRAM', 'service_callback_block_stride96_copy', 0x242f02, 0x242f3e),
        ('DRAM', 'service_callback_named_wait_lock_alloc_free_log_arguments', 0x242f48, 0x242f72),
        ('DRAM', 'registered_wait_callback_c007_selector_and_service1_send40', 0x3085de, 0x308608),
        ('DRAM', 'h265_hal_wait_context_mode_and_unresolved_timeout_arithmetic', 0x2ffcf2, 0x2ffd20),
        ('DRAM', 'h265_hal_wait_context_flags_and_local_call', 0x2ffd54, 0x2ffd88),
        ('DRAM', 'h265_hal_wait_unresolved_result_branch_and_return', 0x2ffd88, 0x2ffda6),
        ('DRAM', 'hal_local_wait_argument88_and_direct_shared_wait_call', 0x249544, 0x249554),
        ('DRAM', 'hal_local_wait_instance_object456_and_followup_call', 0x249574, 0x24957c),
        ('DRAM', 'hal_local_wait_status_bit16_and_reset_candidate_call', 0x2495de, 0x249606),
        ('DRAM', 'hal_local_wait_conditional_object_callback76', 0x24962c, 0x24963e),
        ('DRAM', 'hal_local_wait_saved_result_and_return_epilogue', 0x24971e, 0x249738),
        ('DRAM', 'h265_main_wait_call_saved_result_and_zero_result_log', 0x2d053e, 0x2d0558),
        ('DRAM', 'h265_main_post_wait_vld_word75_query_and_other_calls', 0x2d058a, 0x2d05ba),
        ('DRAM', 'h265_vld_word75_arguments_split_halfwords_and_output_stores', 0x2ffc10, 0x2ffc3e),
        ('DRAM', 'named_hevc_vld_read_index_shift_and_fallback_result', 0x254c28, 0x254c78),
        ('DRAM', 'hal_scalar_read_base36_callback1332_and_indirect_call', 0x254806, 0x25481a),
        ('DRAM', 'h265_constructor_captured_driver_context_parameter', 0x2d2818, 0x2d2820),
        ('DRAM', 'h265_constructor_hal_context_allocator_call_and_saved_result', 0x2d28ce, 0x2d28dc),
        ('DRAM', 'h265_constructor_hal_context_and_outer_context_cross_stores', 0x2d2916, 0x2d2922),
        ('DRAM', 'h265_constructor_instance24_store_and_common_hal_bind_call', 0x2d2acc, 0x2d2af6),
        ('DRAM', 'h265_context28_common_hal_getter_and_store', 0x300806, 0x300818),
        ('DRAM', 'common_hal_fixed_global_object_getter', 0x2283c6, 0x2283d0),
        ('DRAM', 'common_hal_global_object1360_zero_arguments', 0x227eb4, 0x227ecc),
        ('DRAM', 'common_hal_base_physical_arguments_map_call_and_field36_store', 0x227ece, 0x227eec),
        ('DRAM', 'common_hal_vld_candidate_offset400_literal_store', 0x227fd4, 0x227fec),
        ('DRAM', 'common_hal_callback_targets_materialization_and_stores', 0x22827c, 0x2282c8),
        ('DRAM', 'common_hal_installed_scalar_reader_plain_word_load', 0x2282e6, 0x2282ea),
        ('DRAM', 'common_hal_base_mapper_tcm_call_and_saved_result', 0x24690c, 0x24691c),
        ('TCM', 'common_hal_base_map_table_compare_and_offset_merge', 0x10fde, 0x1102c),
        ('DRAM', 'global_init_common_hal_installer_after_shared_irq_init', 0x316948, 0x316974),
        ('DRAM', 'h265_create_wrapper_captured_instance_input_parameter', 0x2cd662, 0x2cd668),
        ('DRAM', 'h265_create_wrapper_driver372_store_and_direct_constructor_call', 0x2cd6c8, 0x2cd6ee),
        ('DRAM', 'codec_create_ops_destination_and_selector_call', 0x270006, 0x270028),
        ('DRAM', 'codec_create_flags_and_candidate_ops32_callback_lookup', 0x27005e, 0x270072),
        ('DRAM', 'codec_create_ops32_instance108_and_output80_arguments', 0x270072, 0x270084),
        ('DRAM', 'codec_create_legacy_ops0_callback_and_same_instance_arguments', 0x27009e, 0x2700b2),
        ('DRAM', 'codec_create_embedded_instance160_store_and_initializer_call', 0x2700d2, 0x2700e6),
        ('DRAM', 'codec_create_instance456_owner_and460_object_stores', 0x2700ea, 0x270100),
        ('DRAM', 'embedded_instance_initializer_pointer_and448_452_argument_stores', 0x224b7c, 0x224b8e),
        ('DRAM', 'h265_ops60_callback_nonnull_nonzero_argument_fast_branch', 0x2cdb2e, 0x2cdb36),
        ('DRAM', 'h265_ops60_callback_fast_return_epilogue', 0x2cdb8e, 0x2cdb94),
        ('DRAM', 'h265_word75_coordinate_bounds_compare_and_byte85_clear', 0x2d05e2, 0x2d0610),
        ('DRAM', 'h265_word75_matched_coordinate_decode_log_and_join', 0x2d0626, 0x2d0646),
        ('DRAM', 'h265_word75_repeated_coordinate_compare_and_byte85_update', 0x2d078a, 0x2d07b6),
        ('DRAM', 'h265_post_wait_display_then_free_list_calls', 0x2d0ab2, 0x2d0ad0),
        ('DRAM', 'h265_post_wait_second_display_list_call', 0x2d0c92, 0x2d0c9e),
        ('DRAM', 'h265_post_wait_third_display_list_call', 0x2d0e7e, 0x2d0e8a),
        ('DRAM', 'h265_free_list_base_and_candidate_search', 0x2cdffa, 0x2ce014),
        ('DRAM', 'h265_free_list_empty_slot_search_and_literal64', 0x2ce014, 0x2ce02c),
        ('DRAM', 'h265_free_list_pointer_and_state4_stores', 0x2ce076, 0x2ce082),
        ('DRAM', 'h265_display_list_base_search_stride16_and_literal20', 0x2ce126, 0x2ce140),
        ('DRAM', 'h265_display_list_first_pointer_and_state2_stores', 0x2ce170, 0x2ce184),
        ('DRAM', 'h265_display_list_alternate_pointer_and_state2_stores', 0x2ce186, 0x2ce19a),
        ('DRAM', 'h265_display_list_two_additional_word_stores', 0x2ce19a, 0x2ce1b2),
        ('DRAM', 'generic_fb_selector_saved_and_candidate_vendor_dispatch', 0x270480, 0x27049e),
        ('DRAM', 'generic_display_callback_result_and_owner_list_followup', 0x270570, 0x27058c),
        ('DRAM', 'generic_free_callback_result_and_owner_list_followup', 0x27059a, 0x2705b4),
        ('DRAM', 'h265_display_getter_wrapper_magic_and_direct_consumer_call', 0x2cd36c, 0x2cd384),
        ('DRAM', 'h265_free_getter_wrapper_magic_and_direct_consumer_call', 0x2cd422, 0x2cd43a),
        ('DRAM', 'h265_display_consumer_stride16_selected_pointer_and_clears', 0x2d3a3a, 0x2d3a64),
        ('DRAM', 'h265_display_consumer_literal1_and_output_pointer_store', 0x2d3a8a, 0x2d3a94),
        ('DRAM', 'h265_free_consumer_matching_list_base_and_slot_load', 0x2d3ad8, 0x2d3aec),
        ('DRAM', 'h265_free_consumer_state_pointer_clears_and_output_store', 0x2d3afc, 0x2d3b24),
        ('DRAM', 'display_list_selector2_query_and_saved_pointer', 0x3097d2, 0x3097e8),
        ('DRAM', 'display_frame_to_ap_token_converter_call_and_result', 0x3097f6, 0x30981a),
        ('DRAM', 'display_frame_saved_token_and_shared_ring_call', 0x309858, 0x309878),
        ('DRAM', 'free_list_selector3_query_and_saved_pointer', 0x309956, 0x30996c),
        ('DRAM', 'free_frame_to_ap_token_converter_call_and_result', 0x309990, 0x3099a8),
        ('DRAM', 'free_frame_saved_token_and_shared_ring_call', 0x3099d8, 0x3099ee),
        ('DRAM', 'frame_to_ap_token_converter_candidate_address_and_pair_load', 0x309e46, 0x309e60),
        ('DRAM', 'shared_fb_publisher_token_pair_stores', 0x30a0f4, 0x30a108),
        ('DRAM', 'fb_list_context_resolver_nonnull_pointer_identity_and_output', 0x2cc992, 0x2cc9a8),
        ('DRAM', 'fb_list_context_resolver_mismatch_return_and_epilogue', 0x2cc9ea, 0x2cc9f0),
    )
    descriptors = []
    for address, callback, role, register_call, control_call in (
            (0x203a0, 0x15208, 'lat', 0x223a6c, 0x223a76),
            (0x203a3, 0x150dc, 'core', 0x223a90, 0x223a9a),
            (0x2039d, 0x223a30, 'ret-only callback, role unresolved',
             0x223ab6, 0x223ac0)):
        raw = span(tcm, address, 3)
        descriptors.append(dict(address=address, bytes=3, raw_hex=raw.hex(),
            captured_fields=list(raw), callback_argument=callback,
            context_argument=0, role_from_callback_log=role,
            registration_call=register_call, control_call=control_call,
            control_a1_explicitly_initialized_at_callsite=False))
    mapping_records = []
    for index in range(7):
        address = 0x1c1e0 + index * 16
        raw = span(tcm, address, 16)
        words = list(struct.unpack('<4I', raw))
        mapping_records.append(dict(address=address, raw_hex=raw.hex(), words=words,
            comparison_region=words[0], comparison_attribute=words[1],
            mapped_region=words[2], fourth_word_semantics_proven=False))
    callbacks = []
    for (role, callback, register_argument, ack, ack_call, signal_jump, handle_cell,
            pulse, log_addresses) in (
            ('lat', 0x15208, 0x36010000, 0x1517e, 0x15294, 0x152a4,
             0x1f1b4, 1, (0x2269ee, 0x226a28, 0x226996)),
            ('core', 0x150dc, 0x36025000, 0x15052, 0x15168, 0x15178,
             0x1f1b0, 16, (0x22692d, 0x226963, 0x2268d9))):
        callbacks.append(dict(role=role, callback=callback,
            register_argument=register_argument, shared_status_argument=0x36000000,
            mapping_helper=0x10fdc, mapping_attribute_argument=0,
            shared_status_offset=0, shared_status_mask=16,
            local_status_offset=164,
            local_status_test_vendor_semantics_unresolved=True,
            log_formats=[dict(address=a, text=cstring(dram, a - 0x200000))
                for a in log_addresses],
            ack_helper=ack, ack_call=ack_call,
            ack_register_sequence=dict(local_offset=164,
                first_rmw_or_mask=17, second_rmw_and_mask_s32=-17,
                pulse_register_argument=0x36098000, pulse_offset=12,
                pulse_value=pulse, pulse_final_value=0,
                saves_and_restores_mstatus=True, interrupt_mask=8,
                interrupt_mask_bit=3, vendor_fence_semantics_proven=False),
            signal_jump=signal_jump, signal_helper=0x14fda,
            signal_handle_cell=handle_cell, signal_isr_argument=1,
            signal_frontier_vendor_branch_unresolved=True,
            ack_or_signal_is_not_buffer_retirement=True))
    result.update(profile_match=True,
        profile='captured RV55 VDEC packed registration and named IRQ callbacks',
        initializer=0x223a34, descriptors=descriptors,
        registration=dict(helper=0x421c, callback_table=0x20fa0,
            dispatch_table_stride=40, callback_offset=0, context_offset=4,
            callback_store=0x4264, context_store=0x4268,
            register_index_arithmetic=0x424c,
            register_index_vendor_semantics_proven=False,
            existing_callback_nonzero_branch=0x4260,
            configure_call=0x426c, configure_helper=0x3fb0,
            failure_callback_clear=0x4270,
            failure_path_has_no_explicit_context_clear=True,
            field_config_helper=0x3eaa, field0_upper_bound=127,
            field1_routing_shift=4, field2_selects_register_or_and=True,
            field1_priority_affinity_and_field2_hardware_meanings_proven=False,
            paired_helpers=[0x5864, 0x58cc],
            exclusion_scope_and_cross_hart_visibility_proven=False),
        control=dict(helper=0x4056, field0_bank_shift=5,
            mask_formula='1 << (field0 & 31)', register_base_literal=0x2120b040,
            vendor_bank_address_instruction=0x4082,
            nonzero_a1_selects_or=True, zero_a1_selects_and_not=True,
            complete_register_address_and_live_enable_proven=False),
        dispatch=dict(helper_candidate=0x4288, source_helper=0x43f8,
            source_base_literals=[0x2120b18c, 0x2120b01c],
            source_register_and_index_vendor_semantics_proven=False,
            stride_multiply=0x42b6, callback_load=0x42c2,
            context_load=0x42ca, context_argument=0x4304, indirect_call=0x4306,
            hart_gate_and_selected_callback_runtime_proven=False),
        mapping=dict(helper=0x10fdc, captured_table=0x1c1e0,
            captured_records=mapping_records, region_mask=0xf8000000,
            attribute_mask=15, offset_mask=0x07ffffff,
            candidate_output_or_offset_sequence=True,
            complete_table_extent_proven=False,
            table_loop_vendor_selection_and_runtime_mutability_proven=False,
            input_addresses_are_not_literal_rv55_pointers=True,
            input_addresses_are_not_authorized_ap_mmio=True),
        callbacks=callbacks,
        shared_signal=dict(helper=0x14fda, handle_load_offset=0,
            isr_helper=0x6d36, thread_helper=0x6788,
            isr_call=0x14fe8, thread_call=0x1502c,
            conditional_yield_call=0x1500c, yield_helper=0x57da,
            complete_vendor_result_and_yield_control_flow_proven=False,
            log_formats=[dict(address=a, text=cstring(dram, a - 0x200000))
                for a in (0x247eb8, 0x247e8f)],
            queue_count_offset=56, queue_limit_offset=60,
            queue_count_increment_store=0x6da6,
            waiter_operation_call=0x6df6, waiter_operation_helper=0x8378,
            creator=0x246fb2, create_calls=[0x223a46, 0x223a52],
            handle_cells=[0x1f1b0, 0x1f1b4],
            captured_handle_raw_hex=span(tcm, 0x1f1b0, 8).hex(),
            create_entries_argument=1, create_item_bytes_argument=0,
            initial_queue_count_proven=False,
            create_type_literal=3, create_handle_store=0x246fce,
            captured_zero_handles_are_not_runtime_state=True,
            shared_hardware_signal_is_not_instance_token_or_buffer_identity=True,
            wait_consumer_and_reset_lifetime_proven=False,
            software_signal_or_yield_is_not_dma_drain=True),
        shared_wait=dict(helper=0x246b82,
            name_address=0x2479fb, name=cstring(dram, 0x2479fb - 0x200000),
            argument_instance_offset=0, argument_hardware_selector_offset=4,
            argument_duration_offset=8, argument_status_offset=16,
            initial_status_clear=0x246b90, instance_context_offset=460,
            vdec_cells_base=0x1f1b0, venc_cells_base=0x1f1b8,
            cell_index_vendor_operations=[0x246c24, 0x246c4a],
            complete_selector_and_instance_authority_proven=False,
            timed_helper=0x246cee, timed_calls=[0x246c2a, 0x246c50],
            duration_multiply=0x246cfa, duration_divide=0x246cfe,
            duration_arithmetic_literal=1000,
            duration_is_wrapping_u32_multiply_then_unsigned_divide=True,
            tick_calibration_and_all_input_time_units_proven=False,
            take_helper=0x68e2, take_call=0x246d06,
            queue_count_offset=56, nonzero_count_branch=0x6936,
            decrement_store=0x693a,
            block_helper_call=0x69e4, block_helper=0x82ee,
            count_recheck=0x69fc, available_count_backedge=0x69fe,
            queue_result_one_on_observed_count_take=True,
            full_vendor_timeout_result_and_scheduling_semantics_proven=False,
            post_wait_profiling_stop_call=0x246c5a,
            post_wait_profiling_stop_helper=0x243534,
            vdec_pre_wait_control_call=0x246c14,
            vdec_pre_wait_control_helper=0xc40a,
            vdec_post_wait_control_call=0x246c3a,
            vdec_post_wait_control_helper=0xc31c,
            pre_post_wait_control_semantics_and_exclusion_proven=False,
            conditional_argument_status_store=0x246cae,
            conditional_status_source_cell=0x1f1cc,
            conditional_status_requires_context_word0_zero=True,
            conditional_status_branch=0x246ca6,
            status_cell_runtime_writer_and_instance_owner_proven=False,
            consumed_count_does_not_identify_a_codec_buffer=True),
        registered_reverse_wait_route=dict(service=1,
            installer=0x242f00, install_call=0x3058a2,
            callback_block_stack_offset=24, callback_offset=0,
            callback_address_materialization=0x305832,
            callback_stack_store=0x305842,
            callback_table_base=0x32c0f0, callback_table_stride=96,
            service1_wait_callback_cell=0x32c150,
            registered_target=0x30854a,
            registration_log_address=0x244b26,
            registration_log=cstring(dram, 0x244b26 - 0x200000),
            registration_name_address=0x244ba3,
            registration_name=cstring(dram, 0x244ba3 - 0x200000),
            lock_callback_offset=8, lock_callback_target=0x3086c2,
            message=0xc007, message_bytes=40,
            hardware_selector_offset=16, hardware_id_offset=24,
            send_call=0x308604, send_helper=0x3052f2,
            callback_route_is_remote_request_not_local_queue_take=True,
            indirect_consumer_and_full_reachability_proven=False,
            runtime_registration_and_table_overwrite_exclusion_proven=False),
        h265_local_wait_route=dict(helper=0x2ffcee,
            name_address=0x3013e8,
            name=cstring(dram, 0x3013e8 - 0x200000),
            log_format_address=0x3013c6,
            log_format=cstring(dram, 0x3013c6 - 0x200000),
            codec_context_input_register='a0',
            context_mode_word_offset=0x2ecc,
            context_mode_compare_literal=48,
            selector_sequence=dict(subtract=0x2ffd12, seqz=0x2ffd1a,
                output_register='s2', selector_move=0x2ffd7e),
            timeout_literal_load=0x2ffcfa, timeout_literal=200,
            arithmetic_other_literal=50,
            unresolved_timeout_arithmetic=0x2ffd16,
            actual_timeout_and_full_vendor_semantics_proven=False,
            hardware_instance_context_offset=24,
            local_call=0x2ffd84, local_helper=0x249526,
            local_argument_bytes=88, local_argument_stack_offset=8,
            local_argument_fields=dict(instance=0, selector=4, duration=8),
            local_shared_wait_call=0x249550, local_shared_wait_helper=0x246b82,
            unresolved_wait_result_branch=0x2ffd8c,
            visible_tail_return_literal=1,
            full_result_branch_and_codec_completion_proven=False,
            local_post_wait_object_offset=456,
            local_post_wait_call=0x24957a, local_post_wait_helper=0x249738,
            local_status_mask=16, local_status_gate=0x2495e2,
            local_reset_candidate_call=0x249604, local_reset_candidate=0x2491fc,
            local_conditional_callback=dict(call=0x24963c,
                object_callback_offset=76, object_argument_offset=80,
                argument_pointer_load=0x249634,
                argument_is_pointer_value_loaded_from_object_field=True,
                argument_is_not_object_plus80_address=True,
                other_arguments=['selector', 1, 0],
                captured_candidate_target=0x2cdb2c,
                installed_target_and_full_branch_proven=False),
            local_saved_wait_result_register='s2', local_result_move=0x24972e,
            main_caller=0x2d04e8, main_wait_call=0x2d0542,
            main_zero_result_branch=0x2d0548,
            main_post_wait_query_call=0x2d0592, main_post_wait_query=0x2ffc0e,
            post_wait_query=dict(context_handle_offset=28,
                context_byte_offset=0x2ed0, register_word_index_literal=75,
                call=0x2ffc26, helper=0x254c28,
                raw_return_register='a0', low16_store=0x2ffc36,
                high16_store=0x2ffc38,
                read_name_address=0x2566b3,
                read_name=cstring(dram, 0x2566b3 - 0x200000),
                selected_base_log_address=0x2566ca,
                selected_base_log=cstring(dram, 0x2566ca - 0x200000),
                index_shift=0x254c3e, index_shift_bits=2,
                candidate_selected_base_field_offset=400,
                selected_base_vendor_address_arithmetic_proven=False,
                null_handle_fallback=0xffff, fallback_return=0x254c70,
                scalar_read_call=0x254c4c, scalar_read_helper=0x254806,
                scalar_read_base_offset=36, scalar_read_callback_offset=1332,
                scalar_read_callback_call=0x254814,
                scalar_read_callback_captured_store_target=0x2282e6,
                scalar_read_callback_runtime_target_proven=False,
                caller_split_wrapper_return_literal=1,
                wrapper_return_one_is_not_read_success_or_completion=True,
                query_target_layout_and_return_semantics_proven=False,
                split_scalar_is_not_buffer_token_or_retirement=True),
            local_route_is_distinct_from_registered_c007_callback=True,
            context_instance_writer_and_runtime_owner_proven=False,
            wait_status_reset_or_callback_is_not_dma_retirement=True),
        common_hal_object_route=dict(
            h265_create_wrapper=0x2cd65c,
            create_wrapper_instance_input_register='a1',
            create_wrapper_instance_saved_register='s6',
            create_wrapper_instance_store=0x2cd6cc,
            create_wrapper_instance_store_offset=372,
            create_wrapper_constructor_call=0x2cd6ea,
            captured_ops_table=0x3356b0, captured_ops_create_member_offset=32,
            captured_ops_create_member_raw_hex=span(dram,
                0x3356b0 + 32 - 0x200000, 4).hex(),
            ops_selection_and_create_wrapper_instance_input_producer_proven=False,
            h265_constructor=0x2d2810,
            constructor_name_address=0x2d6668,
            constructor_name=cstring(dram, 0x2d6668 - 0x200000),
            constructor_driver_context_input_register='a0',
            constructor_driver_context_saved_register='s2',
            hal_context_allocator_call=0x2d28d2,
            hal_context_allocator=0x2cc9f0,
            hal_context_saved_register='s11',
            outer_context_hal_pointer_store=0x2d2916,
            outer_context_hal_pointer_offset=1296,
            hal_context_outer_pointer_store=0x2d291e,
            hal_context_outer_pointer_offset=784,
            driver_instance_source_load=0x2d2acc,
            driver_instance_source_offset=372,
            hal_instance_store=0x2d2ad6, hal_instance_offset=24,
            driver_instance_original_writer_and_runtime_owner_proven=False,
            common_hal_bind_call=0x2d2af2, common_hal_bind_helper=0x300806,
            common_hal_bind_getter_call=0x30080a,
            common_hal_bind_store=0x300812, common_hal_bind_offset=28,
            common_hal_bind_return_literal=1,
            common_hal_bind_one_does_not_validate_initialized_object=True,
            getter=0x2283c6, fixed_global_object=0x329938,
            getter_returns_address_not_initialization_receipt=True,
            global_installer=0x227eb0, global_object_zero_bytes=1360,
            global_initializer_caller_window=0x316948,
            shared_irq_init_call=0x31696a, shared_irq_init_helper=0x223a34,
            global_installer_call=0x316970,
            global_initialization_full_reachability_and_order_proven=False,
            base_physical_literal=0x36000000, base_physical_field_offset=56,
            base_map_call=0x227ee0, base_map_wrapper=0x246908,
            base_map_arguments=dict(a0=0x36000000, a1=0, a2=0x40000, a3=8),
            base_result_store=0x227eea, base_result_field_offset=36,
            base_log_physical_address=0x2283d0,
            base_log_physical=cstring(dram, 0x2283d0 - 0x200000),
            base_log_result_address=0x2283e1,
            base_log_result=cstring(dram, 0x2283e1 - 0x200000),
            base_mapper_tcm_call_pair=[0x246912, 0x246916],
            base_mapper_tcm_helper=0x10fdc,
            map_table_compare_start=0x1c1e0,
            map_table_record_stride=16, map_table_address_mask=0xf8000000,
            map_table_other_argument_mask=15,
            map_table_candidate_record=dict(address=0x1c200,
                raw_hex=tcm[0x1c200:0x1c210].hex(),
                words=list(struct.unpack_from('<IIII', tcm, 0x1c200))),
            map_selected_low_address_mask=0x07ffffff,
            map_unresolved_table_branch=0x10ff2,
            map_unresolved_selected_record_instruction=0x1101a,
            map_failure_visible_return_literal=0,
            map_selection_runtime_authority_and_ap_mapping_proven=False,
            candidate_vld_offset_field=400, candidate_vld_offset_literal=0x28000,
            candidate_vld_offset_store=0x227fe8,
            vld_selected_hw_address_arithmetic_and_word75_semantics_proven=False,
            read_callback_field_offset=1332,
            read_callback_materialize=0x2282a0,
            read_callback_store=0x2282b4, read_callback_target=0x2282e6,
            read_callback_word_load=0x2282e6, read_callback_return=0x2282e8,
            callback_install_overwrite_exclusion_and_runtime_target_proven=False,
            fixed_global_object_is_not_per_buffer_completion_or_dma_retirement=True),
        embedded_instance_and_post_wait_route=dict(
            generic_create=0x26ff2c, codec_owner_ops_offset=16,
            ops_selector_call=0x270022, ops_selector=0x2233ec,
            captured_h265_ops_table=0x3356b0,
            candidate_create_table_member_offset=32,
            candidate_create_object_callback_offset=48,
            candidate_create_callback_load=0x27006e,
            candidate_create_callback_call=0x270080,
            candidate_create_target=0x2cd65c,
            generic_create_branch_byte_offsets=[120, 112],
            candidate_create_additional_word_gate_offset=88,
            word88_mode_and_runtime_selected_route_proven=False,
            create_argument_a0_output_pointer_offset=80,
            create_argument_a1_instance_pointer_offset=108,
            create_argument_a2_object_pointer_offset=116,
            create_argument_a3_codec_word_offset=0,
            create_argument_a4_stack_offset=4, create_argument_a5_literal=0,
            legacy_create_callback_load=0x27009e,
            legacy_create_table_member_offset=0, legacy_create_call=0x2700b0,
            legacy_and_candidate_ops32_routes_are_not_interchangeable=True,
            embedded_instance_owner_offset=160,
            embedded_instance_address_materialize=0x2700d2,
            embedded_instance_pointer_store=0x2700d8,
            embedded_instance_pointer_owner_field_offset=108,
            instance_initializer_call=0x2700e2, instance_initializer=0x224b78,
            instance_initializer_argument_a0_is_owner108_field_address=True,
            instance_initializer_load_pointer=0x224b7c,
            instance_initializer_argument_fields=dict(a2=448, a3=452),
            instance_initializer_return_and_all_mapping_success_proven=False,
            owner_backpointer_instance_offset=456,
            owner_backpointer_store=0x2700ec,
            companion_object_instance_offset=460,
            companion_object_owner_source_offset=116,
            companion_object_store=0x2700f2,
            instance_initialized_byte_offset=112,
            instance_initialized_byte_store=0x2700fc,
            initialized_byte_is_not_runtime_hardware_ready_or_dma_receipt=True,
            wait_followup_owner_context_load_offset=456,
            wait_callback_owner_field_offset=76,
            wait_callback_h265_table_member_offset=60,
            wait_callback_h265_table_member_raw_hex=span(dram,
                0x3356b0 + 60 - 0x200000, 4).hex(),
            wait_callback_candidate_target=0x2cdb2c,
            wait_callback_argument_a0_is_loaded_owner80_pointer_value=True,
            wait_callback_argument_a2_literal=1,
            callback_nonnull_gate=0x2cdb30,
            callback_nonzero_a2_fast_branch=0x2cdb34,
            callback_fast_branch_target=0x2cdb8e,
            callback_fast_branch_saved_literal=1,
            callback_zero_a2_downstream_call=0x2cdb3a,
            callback_zero_a2_downstream_helper=0x2d4662,
            callback_nonnull_nonzero_a2_skips_downstream_helper=True,
            callback_fast_return_is_not_buffer_completion_or_dma_drain=True,
            owner_route_and_callback_actual_runtime_target_proven=False),
        h265_word75_post_wait_state=dict(
            query_wrapper=0x2ffc0e, register_word_index=75,
            low16_caller_stack_offset=184, high16_caller_stack_offset=180,
            context_bound_word_offsets=[1444, 1448],
            compared_values_are_context_bounds_minus_one=True,
            low16_compare=0x2d05fc, high16_compare=0x2d0608,
            mismatch_target=0x2d0696,
            matched_byte_clear=0x2d060c, context_byte_offset=85,
            matched_log_address=0x2d539a,
            matched_log=cstring(dram, 0x2d539a - 0x200000),
            error_coordinate_log_addresses=[0x2d5208, 0x2d5285],
            error_coordinate_logs=[cstring(dram, address - 0x200000)
                for address in (0x2d5208, 0x2d5285)],
            repeated_compares=[0x2d0792, 0x2d079e],
            repeated_match_clear=0x2d07a2,
            candidate_mismatch_nonzero_store=0x2d07b0,
            mismatch_store_unresolved_branch=0x2d07ac,
            selected_sps_bounds_writer_and_complete_control_flow_proven=False,
            scalar_usage_is_lcu_xy_not_buffer_token=True,
            coordinate_match_or_byte85_clear_is_not_dma_retirement=True),
        h265_software_fb_lists_and_candidate_ring_route=dict(
            post_wait_main=0x2d04e8,
            main_display_calls=[0x2d0ac2, 0x2d0c9a, 0x2d0e86],
            main_free_call=0x2d0acc,
            main_complete_cfg_and_all_callers_proven=False,
            free_producer=dict(entry=0x2cdfba, name_address=0x2d496e,
                name=cstring(dram, 0x2d496e - 0x200000),
                log_address=0x2d4988,
                log=cstring(dram, 0x2d4988 - 0x200000),
                context_pointer_array_offset=0x12e4, stride=16,
                visible_search_bound_literal=64,
                pointer_store=0x2ce07c, preceding_word_store=0x2ce07e,
                preceding_word_literal=4,
                full_vendor_search_and_lock_semantics_proven=False),
            display_producer=dict(entry=0x2ce0c2, name_address=0x2d4a1a,
                name=cstring(dram, 0x2d4a1a - 0x200000),
                log_address=0x2d4a34,
                log=cstring(dram, 0x2d4a34 - 0x200000),
                context_pointer_array_offset=0x16e4, stride=16,
                visible_search_bound_literal=20,
                pointer_stores=[0x2ce17a, 0x2ce198],
                preceding_word_stores=[0x2ce17c, 0x2ce194],
                preceding_word_literal=2,
                additional_word_stores=dict(minus12=0x2ce19c, minus8=0x2ce1ae),
                ordering_metadata_full_semantics_and_branch_selection_proven=False),
            generic_query=dict(entry=0x27047c,
                display_caller_selector=2, free_caller_selector=3,
                display_query=0x3097da, free_query=0x30995e,
                display_callback_object_offset=28,
                display_callback_table_offset=12,
                display_callback_call=0x270574,
                free_callback_object_offset=32,
                free_callback_table_offset=16,
                free_callback_call=0x27059e,
                callback_argument_is_loaded_owner80_pointer=True,
                display_captured_table_target=0x2cd362,
                free_captured_table_target=0x2cd418,
                display_table_target_raw_hex=span(dram,
                    0x3356b0 + 12 - 0x200000, 4).hex(),
                free_table_target_raw_hex=span(dram,
                    0x3356b0 + 16 - 0x200000, 4).hex(),
                display_owner_result_slot=728, free_owner_result_slot=732,
                display_owner_list_followup=0x26f85c,
                free_owner_list_followup=0x26f8bc,
                vendor_selector_taken_edges_and_runtime_ops_selection_proven=False),
            context_resolver=dict(entry=0x2cc990,
                input_object_pointer_offset=368, pointer_load=0x2cc996,
                resolved_object_identity_word_offset=0,
                identity_compare=0x2cc99e,
                expected_identity_is_input_a1=True,
                resolved_pointer_output_store=0x2cc9a4,
                successful_return_literal=1,
                null_input_return=-1, null_pointer_return=-2,
                identity_mismatch_return=-3,
                pointer_identity_is_not_ns_mapping_or_dma_authority=True),
            display_consumer=dict(entry=0x2d3956, wrapper_call=0x2cd380,
                context_resolver=0x2cc990, context_resolver_call=0x2d396e,
                selected_pointer_array_offset=0x16e4,
                selected_preceding_word_offset=0x16e0, stride=16,
                pointer_load=0x2d3a54,
                preceding_word_clear=0x2d3a58, pointer_clear=0x2d3a5c,
                output_pointer_store=0x2d3a8c,
                selection_reordering_and_all_vendor_edges_proven=False),
            free_consumer=dict(entry=0x2d3a94, wrapper_call=0x2cd436,
                context_resolver=0x2cc990, context_resolver_call=0x2d3aa8,
                pointer_array_offset=0x12e4, stride=16,
                pointer_load=0x2d3ae6,
                preceding_word_clear=0x2d3b00, pointer_clear=0x2d3b04,
                output_pointer_store=0x2d3b1c,
                complete_search_and_all_vendor_edges_proven=False),
            ring_transfer=dict(display_loaded_pointer=0x3097e2,
                free_loaded_pointer=0x309966,
                frame_pointer_is_not_the_ap_token=True,
                ap_token_converter=0x309dd4,
                display_converter_call=0x309810, free_converter_call=0x3099a0,
                converter_pair_loads=[0x309e56, 0x309e58],
                converter_failure_pair=[-1, -1],
                converter_vendor_arithmetic_and_failure_rejection_proven=False,
                shared_publisher=0x30a06e,
                display_publication_call=0x309876,
                free_publication_call=0x3099ec,
                token_pair_stores=[0x30a100, 0x30a104],
                runtime_durable_dequeue_and_publication_receipt_proven=False),
            software_search_literals_are_not_hardware_buffer_capacity=True,
            software_slot_clears_are_not_hardware_or_dma_retirement=True,
            runtime_context_identity_producer_exclusion_and_cache_authority_proven=False),
        context_profiling=dict(begin_helper=0x2433ac,
            stop_helper=0x243534, timestamp_helper=0x2463c4,
            context_category_offset=0, context_callback_argument_offset=16,
            selector_argument_register='a1',
            flag_table=0x32c210, category_flag_stride=3,
            begin_flag_store=0x243402, begin_flag_value=1,
            stop_flag_clear=0x2435cc,
            timestamp_table=0x32c1b0, category_timestamp_stride=48,
            selector_timestamp_stride=16, timestamp_record_bytes=8,
            begin_timestamp_call=0x2433e8, stop_timestamp_call=0x24357e,
            stop_timestamp_record_offset=8,
            difference_call=0x2435a6, difference_helper=0x2435d6,
            difference_name_address=0x244dac,
            difference_name=cstring(dram, 0x244dac - 0x200000),
            difference_scale_literal=1000000,
            difference_log_formats=[dict(address=a,
                text=cstring(dram, a - 0x200000))
                for a in (0x244d78, 0x244dbf)],
            callback_table=0x32c0e0, category_callback_stride=8,
            callback_load=0x2435bc, callback_call=0x2435c0,
            callback_arguments=['context word at +16', 'selector',
                                'nonzero time difference'],
            callback_table_writer=0x242f00,
            callback_pair_source_argument='a2',
            callback_pair_source_offsets=[0, 4],
            callback_pair_stores=[0x242f2e, 0x242f28],
            category_selector_vendor_bounds_proven=False,
            timestamp_source_and_all_arithmetic_helper_semantics_proven=False,
            selected_callback_identity_and_side_effects_proven=False,
            stop_body_is_not_a_direct_hardware_status_query=True,
            profiling_result_is_not_irq_or_dma_completion=True),
        conditional_status_provenance=dict(cell=0x1f1cc,
            captured_raw_hex=span(tcm, 0x1f1cc, 4).hex(),
            writer_callback=0x155cc, writer_store=0x155de,
            mapped_register_argument=0x38020000,
            mapping_helper=0x10fdc, mapping_attribute_argument=0,
            mapped_status_load=0x155d8, mapped_status_offset=92,
            secondary_mapping_argument=0x38020060,
            secondary_mapping_call=0x155ea,
            status_handler_call=0x155f4, status_handler=0x154b4,
            signal_tail=0x1563e, signal_helper=0x14fda,
            signal_cell=0x1f1b8, signal_isr_argument=1,
            registration_initializer=0x223b0e,
            registration_descriptor=0x20388,
            registration_descriptor_raw_hex=span(tcm, 0x20388, 3).hex(),
            registration_callback_argument=0x155cc,
            registration_context_argument=0,
            registration_call=0x223b66, registration_helper=0x421c,
            descriptor_control_call=0x223b70, descriptor_control_helper=0x403c,
            create_calls=[0x223b20, 0x223b2c, 0x223b38, 0x223b44, 0x223b50],
            created_cell_base=0x1f1b8, created_cell_count=5,
            wait_consumer_cell_base=0x1f1b8,
            wait_consumer_zero_context_word0_branch=0x246b9a,
            wait_consumer_vendor_selector_proven=False,
            log_formats=[dict(address=a, text=cstring(dram, a - 0x200000))
                for a in (0x226b77, 0x226ba3)],
            association='VENC-region shared signal, not VDEC LAT/core cells',
            registration_runtime_execution_and_hardware_source_proven=False,
            all_writers_and_runtime_cell_mutability_proven=False,
            per_instance_or_buffer_identity_proven=False,
            captured_status_is_not_vdec_buffer_retirement=True),
        queue_initialization=dict(static_creator=0x660a,
            captured_storage_arguments=[0x1f1d0, 0x1f224],
            static_object_bytes_literal=84,
            capacity_store=0x66a6, capacity_offset=60,
            item_bytes_store=0x66aa, item_bytes_offset=64,
            reset_call=0x66ae, reset_helper=0x6592,
            reset_new_queue_argument=1, count_zero_store=0x65c8,
            count_offset=56, wait_list_offsets=[16, 36],
            wait_list_init_helper=0x547c,
            type_store=0x66b0, type_offset=80, type_literal=3,
            runtime_creation_reset_or_live_handle_lifetime_proven=False),
        pending_signal_cleanup=dict(helper=0x2253c4,
            log_format_address=0x22689e,
            log_format=cstring(dram, 0x22689e - 0x200000),
            zero_argument_role='core', nonzero_argument_role='lat',
            descriptor_helper=0x403c, mask_helper=0x3fd2,
            descriptor_calls=[0x2253d4, 0x225442],
            pending_count_helper=0x6f44,
            lat_count_call=0x225402, core_count_call=0x22546e,
            lat_zero_count_branch=0x225406, core_zero_count_branch=0x225472,
            lat_take_call=0x225410, core_take_call=0x22547e,
            take_duration_argument_u32=0xffffffff,
            one_explicit_take_call_per_count_query=True,
            take_is_not_a_whole_queue_reset=True,
            count_check_and_take_are_separate_operations=True,
            complete_cleanup_vendor_control_flow_proven=False,
            irq_mask_vendor_bit_semantics_and_cross_hart_exclusion_proven=False,
            common_tail=0x2254a4, common_tail_helper=0x223886,
            common_tail_is_named_enable_helper=True,
            final_irq_mask_state_or_codec_stop_proven=False,
            pending_signal_cleanup_is_not_dma_retirement=True),
        explicit_enable_path=dict(helper=0x223886,
            name_address=0x2267bf, name=cstring(dram, 0x2267bf - 0x200000),
            vdec_log_format_address=0x22680c,
            vdec_log_format=cstring(dram, 0x22680c - 0x200000),
            descriptor_reconfigure_calls=[0x2238f0, 0x223922],
            descriptor_reconfigure_helper=0x3fb0,
            explicit_a1_one_instructions=[0x2238f8, 0x22392a],
            control_calls=[0x2238fc, 0x22392e], control_helper=0x4056,
            direct_vdec_descriptor_arguments=[0x203a0, 0x203a3],
            other_descriptor_source_table=0x227e9c,
            descriptor_source_table_vendor_index_proven=False,
            enable_paths_are_separate_from_initializer_calls=True,
            static_nonzero_control_is_not_runtime_irq_enable=True,
            full_selector_and_final_irq_mask_state_proven=False,
            reconfigure_or_enable_is_not_dma_drain=True),
        pre_post_wait_wla_control=dict(pre_wait_helper=0xc40a,
            post_wait_helper=0xc31c,
            pre_wait_argument_formula='(selector - 1) != 0',
            other_argument='context word at +8',
            source_path_address=0x1a449,
            source_path=cstring(tcm, 0x1a449),
            log_formats=[dict(address=a, text=cstring(tcm, a))
                for a in (0x1a305, 0x1a420, 0x1a46e)],
            control_object=0x24bdc,
            paired_enter_helper=0x160ec, paired_leave_helper=0x161ae,
            pre_enter_call=0xc41c, post_enter_call=0xc32e,
            pre_leave_tail=0xc512, post_leave_tail=0xc406,
            enter_lock_call=0x16134, enter_lock_helper=0x16052,
            leave_unlock_call=0x16196, leave_unlock_helper=0x1613c,
            unlock_zero_store=0x16168, unlock_word_offset=0,
            leave_restore_tail=0x161f4, leave_restore_helper=0x55ba,
            paired_hart_accounting_and_lock_semantics_proven=False,
            reload_reference_cell=0x24df4,
            pre_decrement_store=0xc49c, post_increment_store=0xc3fa,
            post_saturation_compare_literal=0x7fffffff,
            complete_reference_counter_vendor_selection_proven=False,
            history_index_base=0x24df8, history_records_base=0x24e00,
            history_category_shift=6, history_record_shift=3,
            history_index_mask=7,
            pre_history_word0_store=0xc448, pre_history_word0_value=0,
            post_history_word0_store=0xc35c, post_history_word0_value=1,
            history_word1_is_other_argument=True,
            history_category_index_vendor_semantics_proven=False,
            register_base_literal=0x2120a000, register_offset=108,
            pre_register_clear_bit0_store=0xc4e2,
            pre_register_set_bit0_store=0xc504,
            post_register_clear_bit0_store=0xc3c0,
            post_register_set_bit0_store=0xc3e2,
            register_sequences_save_and_restore_mstatus=True,
            register_vendor_fence_and_hardware_effect_proven=False,
            scope='WLA history/reload control around a VDEC semaphore wait',
            lock_word_clear_is_not_ring_producer_exclusion=True,
            reload_counter_is_not_pending_irq_or_buffer_retirement=True,
            wla_register_toggle_is_not_dma_drain=True),
        instruction_evidence=[dict(segment=segment, name=name,
            link_address=start, end_exclusive=end,
            payload_offset=start - (0x200000 if segment == 'DRAM' else 0),
            raw_hex=span(dram if segment == 'DRAM' else tcm,
                start - (0x200000 if segment == 'DRAM' else 0), end-start).hex())
            for segment, name, start, end in spans])
    return result


def firmware(blob):
    """MTK extended 512-byte headers; preserve every byte including padding."""
    at, pieces = 0, []
    service_payloads = {}
    while at + 4 <= len(blob) and struct.unpack_from('<I', blob, at)[0] == 0x58881688:
        if len(pieces) >= 32:
            raise ValueError('too many firmware segments')
        header = span(blob, at, 512)
        magic, size = struct.unpack_from('<II', header)
        ext, hsize = struct.unpack_from('<II', header, 48)
        if (magic != 0x58881688 or ext != 0x58891689 or hsize != 512 or
                struct.unpack_from('<I', header, 56)[0] != 1):
            raise ValueError('unsupported MTK container header')
        # The observed format uses 32-bit sizes and 16-byte alignment.
        if struct.unpack_from('<I', header, 72)[0] != 0:
            raise ValueError('unsupported extended payload size')
        align = struct.unpack_from('<I', header, 68)[0]
        if align != 16 or not size:
            raise ValueError('unsupported MTK payload alignment/size')
        name = cstring(header[8:40], 0)
        data = span(blob, at + hsize, size)
        end = at + hsize + size
        stop = (end + 15) & ~15
        padding = span(blob, end, stop - end)
        pieces.append(dict(index=len(pieces), name=name, offset=at,
                           header_bytes=hsize, header_words=list(struct.unpack('<20I', header[:80])),
                           payload_offset=at + hsize,
                           payload_bytes=size, padding_bytes=len(padding),
                           payload_sha256=hashlib.sha256(data).hexdigest(),
                           stored_segment_sha256=hashlib.sha256(blob[at:stop]).hexdigest(),
                           local_includes_header_and_padding=True,
                           local=f'segment-{len(pieces):02d}.bin'))
        if name == 'tinysys-vcp-RV55_A':
            pieces[-1]['wire_evidence'] = rv55_wire_evidence(data)
        if name in ('tinysys-vcp-RV55_A', 'tinysys-vcp-RV55_A_dram', 'tinysys-mmup-RV33_A'):
            if name in service_payloads:
                raise ValueError('ambiguous duplicate core payload')
            service_payloads[name] = data
        at = stop
    if not pieces:
        raise ValueError('missing MTK firmware segments')
    tail = blob[at:]
    init = rv55_init_evidence(service_payloads.get('tinysys-vcp-RV55_A_dram'))
    return dict(segments=pieces, tail=dict(offset=at, bytes=len(tail),
                all_zero=not any(tail), sha256=hashlib.sha256(tail).hexdigest()),
                global_boundary_evidence=global_boundary_evidence(
                    service_payloads.get('tinysys-vcp-RV55_A'),
                    service_payloads.get('tinysys-mmup-RV33_A')),
                service_shm_evidence=rv55_shm_evidence(
                    service_payloads.get('tinysys-vcp-RV55_A'),
                    service_payloads.get('tinysys-vcp-RV55_A_dram')),
                service_init_evidence=init,
                service_dispatch_evidence=rv55_service_dispatch_evidence(
                    service_payloads.get('tinysys-vcp-RV55_A'),
                    service_payloads.get('tinysys-vcp-RV55_A_dram'), init),
                service_start_evidence=rv55_start_evidence(
                    service_payloads.get('tinysys-vcp-RV55_A_dram'), init),
                service_vp_evidence=rv55_vp_evidence(
                    service_payloads.get('tinysys-vcp-RV55_A_dram'), init),
                service_mem_wait_evidence=rv55_mem_wait_evidence(
                    service_payloads.get('tinysys-vcp-RV55_A_dram'), init),
                service_clock_evidence=rv55_clock_evidence(
                    service_payloads.get('tinysys-vcp-RV55_A_dram'), init),
                service_clock_install_evidence=rv55_clock_install_evidence(
                    service_payloads.get('tinysys-vcp-RV55_A'),
                    service_payloads.get('tinysys-vcp-RV55_A_dram'), init),
                service_vdec_irq_evidence=rv55_vdec_irq_evidence(
                    service_payloads.get('tinysys-vcp-RV55_A'),
                    service_payloads.get('tinysys-vcp-RV55_A_dram'), init),
                service_output_evidence=rv55_output_evidence(
                    service_payloads.get('tinysys-vcp-RV55_A_dram'), init),
                signature_verified=False, executable_or_loadable_proven=False)


class ELF:
    def __init__(self, blob):
        self.blob = blob
        head = span(blob, 0, 64)
        if head[:7] != b'\x7fELF\x02\x01\x01':
            raise ValueError('expected ELF64 little-endian module')
        fields = struct.unpack_from('<HHIQQQIHHHHHH', head, 16)
        typ, machine, _, _, _, shoff, _, ehsize, _, _, shsize, count, names = fields
        if (typ, machine, ehsize, shsize) != (1, 183, 64, 64):
            raise ValueError('expected relocatable AArch64 module')
        if not 0 < count <= 512 or not 0 < names < count:
            raise ValueError('unsupported ELF section numbering')
        raw = span(blob, shoff, count * 64)
        rows = [struct.unpack_from('<IIQQQQIIQQ', raw, i * 64) for i in range(count)]
        string_row = rows[names]
        strings = span(blob, string_row[4], string_row[5])
        self.sections = []
        for row in rows:
            n, t, flags, addr, off, size, link, info, align, entry = row
            if t != 8:
                span(blob, off, size)
            self.sections.append(dict(name=cstring(strings, n), type=t,
                offset=off, bytes=size, link=link, info=info, entry=entry))
        self.symbols = []
        for sec in self.sections:
            if sec['type'] != 2:
                continue
            if sec['entry'] != 24 or sec['bytes'] % 24 or not 0 <= sec['link'] < count:
                raise ValueError('invalid ELF symbol table')
            strings = self.data(self.sections[sec['link']])
            data = self.data(sec)
            for at in range(0, len(data), 24):
                name, info, other, index, value, size = struct.unpack_from('<IBBHQQ', data, at)
                self.symbols.append(dict(name=cstring(strings, name), section=index,
                                         value=value, bytes=size, type=info & 15))

    def data(self, sec):
        if sec['type'] == 8:
            raise ValueError('NOBITS is not captured runtime memory')
        return span(self.blob, sec['offset'], sec['bytes'])

    def named_data(self, name):
        found = [s for s in self.symbols if s['name'] == name]
        if len(found) != 1:
            raise ValueError(f'missing/ambiguous ELF symbol {name}')
        sym = found[0]
        index = sym['section']
        if not 0 < index < len(self.sections) or sym['type'] != 1:
            raise ValueError('expected defined ELF object')
        sec = self.sections[index]
        data = span(self.data(sec), sym['value'], sym['bytes'])
        for rel in self.sections:
            if rel['type'] not in (4, 9) or rel['info'] != index:
                continue
            step = 24 if rel['type'] == 4 else 16
            if rel['entry'] != step or rel['bytes'] % step:
                raise ValueError('invalid ELF relocation table')
            relocation_data = self.data(rel)
            for at in range(0, rel['bytes'], step):
                target = struct.unpack_from('<Q', relocation_data, at)[0]
                if sym['value'] <= target < sym['value'] + sym['bytes']:
                    raise ValueError('cannot interpret unresolved relocated object')
        return data, dict(section=sec['name'], offset=sym['value'], bytes=sym['bytes'])


def decoder_common_evidence(elf, build_id):
    """File-specific common access profile, NOT a captured live common block.

    Gate on the entire captured KO and its build-id, then require unique .text
    and exact defined function anchors. Raw bytes (including unresolved call
    relocations) go only to the private report; offsets are section-relative,
    never loaded VAs. Firmware residency/pairing is a separate prerequisite.
    """
    result = dict(profile_match=False, file_field_abi_proven=False,
                  runtime_common_captured=False, runtime_mapping_proven=False,
                  module_firmware_pairing_proven=False)
    if (build_id != 'a5f63a89f1658568bb7ffc2ae837e96df2361ea1' or
            hashlib.sha256(elf.blob).hexdigest() !=
            '4de1ff25c4b16936bc487f6176d9feda24b4b9c85254f4921d4f8f330eb2bae0'):
        return result
    sections = [(i, s) for i, s in enumerate(elf.sections) if s['name'] == '.text']
    if len(sections) != 1 or sections[0][1]['type'] != 1:
        raise ValueError('captured decoder requires unique PROGBITS .text')
    index, sec = sections[0]
    anchors = (
        ('mtk_vcodec_dec_probe', 0xfd88, 0x1398),
        ('smmu_v3_enabled', 0x112ac, 0x58),
        ('mtk_vdec_translation_fault_callback', 0x1456c, 0x484),
        ('vcp_dec_ipi_handler', 0x21654, 0x17cc),
        ('handle_vdec_mem_alloc', 0x22e20, 0x2c0),
        ('vdec_vcp_deinit', 0x26e88, 0x1a0),
    )
    functions = {}
    for name, start, size in anchors:
        matches = [s for s in elf.symbols if s['name'] == name]
        if (len(matches) != 1 or matches[0]['section'] != index or
                matches[0]['type'] != 2 or matches[0]['value'] != start or
                matches[0]['bytes'] != size):
            raise ValueError(f'captured decoder function anchor mismatch: {name}')
        functions[name] = dict(section='.text', offset=start, bytes=size)
    spans = (
        ('probe_flag_base_constant', 'mtk_vcodec_dec_probe', 0x10e34, 0x10e3c),
        ('probe_flag_base_pointer', 'mtk_vcodec_dec_probe', 0x10f6c, 0x10f74),
        ('probe_flag_assignment', 'mtk_vcodec_dec_probe', 0x10fdc, 0x10ff0),
        ('smmu_compatibility_test', 'smmu_v3_enabled', 0x112ac, 0x11304),
        ('fault_stores', 'mtk_vdec_translation_fault_callback', 0x1476c, 0x14794),
        ('shm_type8_and_common_write', 'vcp_dec_ipi_handler', 0x21888, 0x21944),
        ('vp_consumers', 'vcp_dec_ipi_handler', 0x22224, 0x2226c),
        ('vp_publish_after_prepare', 'vcp_dec_ipi_handler', 0x22a0c, 0x22a54),
        ('shm_reserve0_assignment', 'handle_vdec_mem_alloc', 0x22e50, 0x22ea4),
        ('vp_teardown', 'vdec_vcp_deinit', 0x26f74, 0x26f90),
    )
    evidence, text = [], elf.data(sec)
    for name, function, start, end in spans:
        anchor = functions[function]
        if not anchor['offset'] <= start < end <= anchor['offset'] + anchor['bytes']:
            raise ValueError('common evidence outside function anchor')
        evidence.append(dict(name=name, function=function, section='.text',
            start=start, end_exclusive=end, file_offset=sec['offset'] + start,
            raw_hex=span(text, start, end - start).hex()))
    result.update(profile_match=True, profile='MT6993 captured decoder common ABI 1',
        wire_common_abi=1, file_field_abi_proven=True, accessed_prefix_bytes=80,
        function_anchors=functions, instruction_evidence=evidence,
        fields=[dict(name='fault_hw_id', offset=0, width=4),
                dict(name='fault_port_index', offset=4, width=4, is_sid_proven=False),
                dict(name='fault_iova', offset=8, width=8),
                dict(name='has_fault', offset=16, width=4),
                dict(name='enable_smmu', offset=24, width=1),
                dict(name='vp_published', offset=25, width=1, count=2),
                dict(name='vp_iova', offset=32, width=8, shape=[2, 3], row_stride=24)],
        padding_ranges=[dict(offset=20, bytes=4), dict(offset=27, bytes=5)],
        smmu_flag_source=dict(device_offset=0x3c60, base_offset=0x3b5c,
            offset_from_base=0x104, compatible='arm,smmu-v3',
            tests_node_presence_not_hardware_translation=True),
        shm=dict(memory_type=8, reserve_index=0, named_pa_is_dma_iova=True),
        vp=dict(prepare_function='mtk_vcodec_vp_mode_buf_prepare',
            buffers_per_group=3, groups=2, publish_after_success=True,
            clears_flags_and_iovas_before_unprepare=True,
            dma_allocation_and_cache_ownership_proven=False))
    return result


def decoder_init_evidence(elf, common):
    """Use the common profile's whole-file identity, but validate new anchors."""
    result = dict(profile_match=False, runtime_init_proven=False,
                  complete_vsi_abi_proven=False, module_firmware_pairing_proven=False)
    if not common['profile_match']:
        return result
    sections = [(i, s) for i, s in enumerate(elf.sections) if s['name'] == '.text']
    if len(sections) != 1:
        raise ValueError('captured decoder INIT requires unique .text')
    index, sec = sections[0]
    functions = dict(common['function_anchors'])
    for name, start, size in (('vdec_vcp_init', 0x25150, 0x404),
                              ('vdec_vcp_ipi_send', 0x230e0, 0x97c)):
        matches = [s for s in elf.symbols if s['name'] == name]
        if (len(matches) != 1 or matches[0]['section'] != index or
                matches[0]['type'] != 2 or matches[0]['value'] != start or
                matches[0]['bytes'] != size):
            raise ValueError(f'captured decoder INIT anchor mismatch: {name}')
        functions[name] = dict(section='.text', offset=start, bytes=size)
    spans = (
        ('init_zero_prefix', 'vdec_vcp_init', 0x2518c, 0x25194),
        ('init_id_context_ap_token', 'vdec_vcp_init', 0x25258, 0x2526c),
        ('init_send_24_bytes', 'vdec_vcp_init', 0x25410, 0x2542c),
        ('send_copy_and_outer_header', 'vdec_vcp_ipi_send', 0x233a8, 0x2344c),
        ('incoming_payload_base_and_token', 'vcp_dec_ipi_handler', 0x21868, 0x21888),
        ('ack_status_before_dispatch', 'vcp_dec_ipi_handler', 0x21a88, 0x21ab4),
        ('init_ack_dispatch', 'vcp_dec_ipi_handler', 0x21d24, 0x21d50),
        ('init_ack_alias_to_vsi', 'vcp_dec_ipi_handler', 0x22454, 0x224b0),
        ('post_init_vsi_access', 'vdec_vcp_init', 0x2544c, 0x25474),
        ('codec_check_dispatch_and_source_mode', 'vcp_dec_ipi_handler', 0x219f8, 0x21a28),
        ('heif_fourcc_is_distinct', 'vcp_dec_ipi_handler', 0x21a60, 0x21a70),
        ('hevc_fourcc_to_codec2', 'vcp_dec_ipi_handler', 0x21a70, 0x21a88),
        ('heif_codec3', 'vcp_dec_ipi_handler', 0x22030, 0x22034),
        ('codec_and_svp_match_or_negative_reply', 'vcp_dec_ipi_handler', 0x22034, 0x22088),
        ('codec_success_reply', 'vcp_dec_ipi_handler', 0x22370, 0x22394),
    )
    evidence, text = [], elf.data(sec)
    for name, function, start, end in spans:
        anchor = functions[function]
        if not anchor['offset'] <= start < end <= anchor['offset'] + anchor['bytes']:
            raise ValueError('INIT evidence outside function anchor')
        evidence.append(dict(name=name, function=function, section='.text',
            start=start, end_exclusive=end, file_offset=sec['offset'] + start,
            raw_hex=span(text, start, end - start).hex()))
    result.update(profile_match=True, profile='MT6993 captured decoder INIT framing',
        init=0xa000, init_done=0xb000, init_payload_bytes=24,
        request_has_fourcc=False, service_id=1,
        request=dict(message_offset=0, context_offset=4, ap_token_offset=8,
            status_offset=16, reserved_offset=20),
        ack=dict(ap_token_offset=8, status_offset=16, firmware_vsi_alias_offset=24,
            context_equality_check_proven=False,
            stock_low28_subtraction_is_not_mapping_authorization=True),
        post_init_vsi_access=dict(offset=0x1228, bytes=4,
            minimum_accessed_span=0x122c, name_from_source='ipi_blocked',
            complete_vsi_layout_proven=False),
        codec_identification=dict(message=0xc008, reply=0xd008, payload_bytes=40,
            hevc_fourcc=0x43564548, hevc_codec_id=2, heif_codec_id=3,
            status_field_before_reply='candidate SVP mode, not an errno',
            accept_condition='candidate codec id equals source codec AND status equals source SVP',
            reply_status_success=0, reply_status_mismatch=-1,
            reply_context_from_host_instance=True,
            ap_token_reserved_data_payload_preserved=True,
            physical_handling_implemented=False),
        instruction_evidence=evidence)
    return result


def decoder_decode_evidence(elf, common):
    """File-bound decode accesses, not a complete VSI or runnable provider."""
    result = dict(profile_match=False, runtime_start_proven=False,
                  complete_vsi_abi_proven=False, runtime_mapping_proven=False,
                  hardware_completion_proven=False, linear_pixels_proven=False,
                  module_firmware_pairing_proven=False)
    if not common['profile_match']:
        return result
    sections = [(i, s) for i, s in enumerate(elf.sections) if s['name'] == '.text']
    if len(sections) != 1:
        raise ValueError('captured decoder decode requires unique .text')
    index, sec = sections[0]
    functions = dict(common['function_anchors'])
    for name, start, size in (('vdec_vcp_decode', 0x25558, 0x754),
                              ('vdec_vcp_init', 0x25150, 0x404)):
        matches = [s for s in elf.symbols if s['name'] == name]
        if (len(matches) != 1 or matches[0]['section'] != index or
                matches[0]['type'] != 2 or matches[0]['value'] != start or
                matches[0]['bytes'] != size):
            raise ValueError(f'captured decoder decode anchor mismatch: {name}')
        functions[name] = dict(section='.text', offset=start, bytes=size)
    spans = (
        ('start_zero_entire_40_byte_message', 'vdec_vcp_decode', 0x25590, 0x255a0),
        ('bs_token_dma_non_acp_and_input_mode', 'vdec_vcp_decode', 0x255ec, 0x25620),
        ('non_input_driven_clear_fb', 'vdec_vcp_decode', 0x2563c, 0x25654),
        ('plane_count_and_eight_dma_stores', 'vdec_vcp_decode', 0x25654, 0x256fc),
        ('fb_token_and_index', 'vdec_vcp_decode', 0x256fc, 0x25710),
        ('general_buffer_fd_size_dma', 'vdec_vcp_decode', 0x25714, 0x2574c),
        ('general_buffer_absent_fd_minus_one_size_zero', 'vdec_vcp_decode', 0x25784, 0x257b4),
        ('timestamp_latency_and_start_fields', 'vdec_vcp_decode', 0x257d8, 0x25830),
        ('start_send_40_bytes', 'vdec_vcp_decode', 0x25870, 0x25890),
        ('changed_info_and_error_map', 'vdec_vcp_decode', 0x258ac, 0x258cc),
        ('lat_error_code', 'vdec_vcp_decode', 0x258cc, 0x258ec),
        ('async_mode_and_core_error_code', 'vdec_vcp_decode', 0x25908, 0x2591c),
        ('need_sequence_resolution_unsupported_branches', 'vdec_vcp_decode', 0x25938, 0x25948),
        ('successful_return_mode_consumers', 'vdec_vcp_decode', 0x25994, 0x259f4),
        ('post_init_blocked_store', 'vdec_vcp_init', 0x2544c, 0x25474),
        ('b001_dispatch_only_wakes_waiter', 'vcp_dec_ipi_handler', 0x21d24, 0x21d50),
        ('b001_payload_base_and_ap_token', 'vcp_dec_ipi_handler', 0x21868, 0x21888),
        ('b001_ap_token_matches_live_context', 'vcp_dec_ipi_handler', 0x21974, 0x21998),
        ('b001_status_before_dispatch', 'vcp_dec_ipi_handler', 0x21a88, 0x21ab4),
        ('ack_waiter_wakeup', 'vcp_dec_ipi_handler', 0x229bc, 0x229e0),
    )
    evidence, text = [], elf.data(sec)
    for name, function, start, end in spans:
        anchor = functions[function]
        if not anchor['offset'] <= start < end <= anchor['offset'] + anchor['bytes']:
            raise ValueError('decode evidence outside function anchor')
        evidence.append(dict(name=name, function=function, section='.text',
            start=start, end_exclusive=end, file_offset=sec['offset'] + start,
            raw_hex=span(text, start, end - start).hex()))
    result.update(profile_match=True, profile='MT6993 captured decoder decode subset ABI 1',
        wire_decode_subset_abi=1, file_accessed_fields_proven=True,
        start=dict(message=0xa001, payload_bytes=40, service_id=1,
            context_offset=4, firmware_instance_alias_offset=8,
            instance_field_is_not_ap_token=True, bytes_used_offset=16,
            buffer_capacity_offset=20, flags_offset=24,
            zero_data_word_offsets=[28, 32, 36], has_status_field=False,
            physical_transport_implemented=False),
        start_done=dict(message=0xb001, ap_token_offset=8, status_offset=16,
            instance_field_is_not_request_firmware_alias=True,
            context_echo_proven=False, same_instance_picture_sequence_proven=False,
            matched_reply_is_not_prior_submission_retirement=True,
            waiter_wakeup_is_not_pixels=True,
            hardware_completion_and_frame_ownership_proven=False),
        fields=[dict(name=name, offset=offset, width=width, count=count)
            for name, offset, width, count in (
                ('changed_info', 0xe7c, 4, 1), ('bs_iova', 0xe80, 8, 1),
                ('bs_non_acp_iova', 0xe90, 8, 1), ('fb_iova', 0xe98, 8, 8),
                ('bs_token', 0xf18, 8, 1), ('fb_token', 0xf20, 8, 1),
                ('plane_count', 0xf28, 4, 1), ('fb_index', 0xf2c, 4, 1),
                ('error_map', 0xf30, 4, 1), ('error_code_core_lat', 0xf34, 4, 2),
                ('timestamp', 0xf48, 8, 1), ('input_driven', 0x1000, 1, 1),
                ('output_async', 0x1001, 1, 1), ('low_power_mode', 0x1003, 1, 1),
                ('dynamic_low_latency', 0x1004, 1, 1), ('general_iova', 0x1208, 8, 1),
                ('general_fd_signed', 0x1210, 4, 1), ('general_bytes', 0x1214, 4, 1),
                ('ipi_blocked', 0x1228, 4, 1))],
        minimum_accessed_span=0x122c, plane_array_capacity=8,
        plane_capacity_is_not_negotiated_format=True,
        raw_modes_are_not_restricted_to_booleans=True,
        input_output_tokens_never_dereferenced=True,
        unused_plane_entries_may_be_stale=True,
        snapshot_has_no_ring_or_pixel_format_or_frame_dimensions=True,
        function_anchors=functions, instruction_evidence=evidence)
    return result


def decoder_bs_route_evidence(elf, common):
    """File-bound BS route selection, not two aliases of an owned allocation.

    The main queue may use device1, device2 or ACP device7. The separately
    attached non-ACP mapping is conditional and otherwise explicitly zero;
    do not copy the main IOVA into that field or infer runtime ACP settings.
    """
    result = dict(profile_match=False, runtime_main_route_proven=False,
        runtime_acp_parameters_proven=False, runtime_non_acp_mapping_proven=False,
        native_route_ownership_proven=False, module_firmware_pairing_proven=False)
    if not common['profile_match']:
        return result
    anchors = (('mtk_vcodec_dec_queue_init', 0x8bf8, 0x588),
               ('vb2ops_vdec_buf_prepare', 0xc48c, 0x82c),
               ('vdec_vcp_decode', 0x25558, 0x754))
    spans = (
        ('main_acp_support_enable_and_secure_gates', 'mtk_vcodec_dec_queue_init', 0x8d08, 0x8d2c),
        ('main_non_acp_route_and_alternative_gate', 'mtk_vcodec_dec_queue_init', 0x8d2c, 0x8d50),
        ('main_alternative_device2', 'mtk_vcodec_dec_queue_init', 0x8db0, 0x8dc0),
        ('main_acp_device7_and_null_check', 'mtk_vcodec_dec_queue_init', 0x8e10, 0x8e2c),
        ('optional_non_acp_output_queue_and_four_gates', 'vb2ops_vdec_buf_prepare', 0xc6a0, 0xc6e4),
        ('optional_non_acp_actual_attach_map', 'vb2ops_vdec_buf_prepare', 0xc6e4, 0xc70c),
        ('optional_non_acp_absent_zeroes_three_fields', 'vb2ops_vdec_buf_prepare', 0xc70c, 0xc718),
        ('start_copies_distinct_bs_fields_to_vsi', 'vdec_vcp_decode', 0x255ec, 0x25620),
    )
    functions, instructions = captured_function_spans(elf, anchors, spans)
    relocations = [captured_rela(elf, '.text', offset,
        (kind, name, None, 0, 0, 0)) for offset, kind, name in (
            (0x8d14, 275, 'mtk_vdec_acp_enable'),
            (0x8d18, 278, 'mtk_vdec_acp_enable'),
            (0x8d44, 283, 'vcp_get_io_device_ex'),
            (0x8db4, 283, 'vcp_get_io_device_ex'),
            (0x8e14, 283, 'vcp_get_io_device_ex'),
            (0x8e20, 283, 'vcp_get_io_device_ex'),
            (0xc6b4, 283, 'vcp_get_io_device_ex'),
            (0xc6c4, 275, 'mtk_vdec_acp_enable'),
            (0xc6c8, 278, 'mtk_vdec_acp_enable'),
            (0xc6d4, 275, 'mtk_vdec_acp_debug'),
            (0xc6d8, 285, 'mtk_vdec_acp_debug'),
            (0xc704, 283, 'mtk_vcodec_dma_attach_map'))]
    result.update(profile_match=True, profile='captured MT6993 BS route producer ABI 1',
        function_anchors=functions, instruction_evidence=instructions,
        relocation_evidence=relocations,
        main_queue=dict(device_ids=[1, 2, 7], device_numbers_are_not_sids=True,
            acp_device_id=7, acp_gates=dict(device_support_nonzero=True,
                imported_enable_equals=1, secure_mode_byte_equals=0),
            ordinary_device_id=1, alternative_device_id=2,
            ordinary_route_is_not_unconditional=True),
        optional_non_acp=dict(device_id=1, queue_type=10,
            gates=dict(device_support_nonzero=True, imported_enable_equals=1,
                imported_debug_nonzero=True, io_device_nonnull=True),
            separate_dma_attach_map=True, direction_argument=0,
            vb_buffer_iova_offset=0x4a8, vb_buffer_attachment_offset=0x8e0,
            vb_buffer_sg_table_offset=0x8e8, absent_values=[0, 0, 0],
            mapping_failure_result_not_checked_by_this_caller=True,
            main_and_non_acp_iovas_are_not_proven_equal=True),
        vsi=dict(main_bs_iova_offset=0xe80, non_acp_bs_iova_offset=0xe90,
            distinct_fields_copied_before_start=True,
            zero_non_acp_is_an_observed_absent_mapping_branch=True,
            zero_non_acp_does_not_prove_main_route_owned_or_acp_disabled=True))
    return result


def decoder_vp_evidence(elf, common):
    """The matched decoder dispatches c009 and publishes common, not ACK status."""
    result = dict(profile_match=False, runtime_vp_proven=False,
                  dma_mapping_and_cache_ownership_proven=False,
                  module_firmware_pairing_proven=False)
    if not common['profile_match']:
        return result
    sec = next(s for s in elf.sections if s['name'] == '.text')
    text, anchor = elf.data(sec), common['function_anchors']['vcp_dec_ipi_handler']
    spans = (
        ('c009_and_parameter_id0', 0x22210, 0x22224),
        ('bitdepth_smmu_multi_publication_gates', 0x22224, 0x2226c),
        ('d009_preserves_context_and_status', 0x2227c, 0x222a0),
        ('prepare_success_stores_iovas_then_flag', 0x22a0c, 0x22a58),
    )
    evidence = []
    for name, start, end in spans:
        if not anchor['offset'] <= start < end <= anchor['offset'] + anchor['bytes']:
            raise ValueError('VP evidence outside decoder function')
        evidence.append(dict(name=name, function='vcp_dec_ipi_handler', section='.text',
            start=start, end_exclusive=end, file_offset=sec['offset'] + start,
            raw_hex=span(text, start, end - start).hex()))
    result.update(profile_match=True, profile='MT6993 captured decoder VP publication',
        message=0xc009, reply=0xd009, payload_bytes=40, parameter_id=0,
        bitdepth_offset=0xf80, bitdepth_width=4,
        group_index='bitdepth == 8 ? 0 : 1',
        device_multiplier_offset=0x3c98, buffer_count=3,
        common_publication_flag_offsets=[25, 26], common_iova_offset=32,
        common_iova_tuple_stride=24, publication_after_prepare_nonnegative=True,
        ack_context_preserved=True, ack_status_not_allocation_result=True,
        ack_status0_not_publication_or_cache_proof=True,
        instruction_evidence=evidence)
    return result


def decoder_mem_wait_evidence(elf, common):
    """Current dispatch/lifetime ABI, never authorization to allocate or ACK."""
    result = dict(profile_match=False, runtime_allocation_proven=False,
                  runtime_irq_completion_proven=False,
                  dma_mapping_and_cache_ownership_proven=False)
    if not common['profile_match']:
        return result
    sections = [(i, s) for i, s in enumerate(elf.sections) if s['name'] == '.text']
    index, sec = sections[0]
    functions = dict(common['function_anchors'])
    name = 'get_dev_by_mem_type'
    matches = [s for s in elf.symbols if s['name'] == name]
    if (len(matches) != 1 or matches[0]['section'] != index or
            matches[0]['type'] != 2 or matches[0]['value'] != 0x24d48 or
            matches[0]['bytes'] != 0x13c):
        raise ValueError('captured memory device selector anchor mismatch')
    functions[name] = dict(section='.text', offset=0x24d48, bytes=0x13c)
    spans = (
        ('status_zero_vs_nonzero_dispatch', 'vcp_dec_ipi_handler', 0x21a88, 0x21ab4),
        ('zero_selector_context_and_reverse_dispatch', 'vcp_dec_ipi_handler', 0x21b00, 0x21b60),
        ('zero_selector_trace_read_or_constant0', 'vcp_dec_ipi_handler', 0x21b60, 0x21b98),
        ('nonzero_selector_c007_dispatch', 'vcp_dec_ipi_handler', 0x21d74, 0x21d80),
        ('nonzero_selector_wait500_d007_status_host_context', 'vcp_dec_ipi_handler', 0x21f58, 0x21fd4),
        ('c005_allocate_d005_send64', 'vcp_dec_ipi_handler', 0x22520, 0x22544),
        ('c006_exact32_byte_mem_match', 'vcp_dec_ipi_handler', 0x22654, 0x226a8),
        ('c006_unlink_before_teardown', 'vcp_dec_ipi_handler', 0x226a8, 0x226ec),
        ('c006_free_result_d006_send64', 'vcp_dec_ipi_handler', 0x226fc, 0x22750),
        ('zero_selector_wait500_d007_status', 'vcp_dec_ipi_handler', 0x22770, 0x22794),
        ('type8_service_vs_instance', 'handle_vdec_mem_alloc', 0x22e50, 0x22e68),
        ('instance_select_device_allocate_store_status', 'handle_vdec_mem_alloc', 0x22f30, 0x22f6c),
        ('success_record_alloc_failure_returns_without_rollback', 'handle_vdec_mem_alloc', 0x22f7c, 0x22fa8),
        ('record_mem_attach_sgt_under_lock', 'handle_vdec_mem_alloc', 0x22fa8, 0x23008),
        ('allocation_failure_zero_iova_and_dma_alias', 'handle_vdec_mem_alloc', 0x23094, 0x2309c),
        ('secure_flag_and_type_remapping', 'get_dev_by_mem_type', 0x24d58, 0x24d98),
        ('clear_type_device_selection', 'get_dev_by_mem_type', 0x24d98, 0x24de8),
        ('ube_vcp_io4_then5_then_codec_fallback', 'get_dev_by_mem_type', 0x24e00, 0x24e44),
    )
    text, evidence = elf.data(sec), []
    for label, function, start, end in spans:
        anchor = functions[function]
        if not anchor['offset'] <= start < end <= anchor['offset'] + anchor['bytes']:
            raise ValueError('memory/wait evidence outside decoder function')
        evidence.append(dict(name=label, function=function, section='.text',
            start=start, end_exclusive=end, file_offset=sec['offset'] + start,
            raw_hex=span(text, start, end - start).hex()))
    result.update(profile_match=True, profile='MT6993 captured instance mem/wait ABI 1',
        wire_memory_abi=1,
        memory=dict(request=0xc005, reply=0xd005, free_request=0xc006, free_reply=0xd006,
            bytes=64, object_offset=24, object_bytes=32,
            fields=dict(type=24, requested_length=28, iova=32, dma_alias_not_cpu_pa=40,
                allocation_object_token_not_cpu_va=48),
            opaque_reserved_offsets=[20, 56, 60], reply_context='host context',
            allocation_status0_does_not_prove_tracking_record=True,
            allocation_record_failure_has_no_visible_rollback=True,
            free_compares_all32_object_bytes=True, free_unlinks_before_teardown=True,
            provider_must_keep_uncertain_release_record=True),
        devices=dict(secure_flag_offset_in_ctx=0x189,
            clear_hw_types=[1, 2], clear_hw_target='instance+0x70 -> ctx+8 -> device+0x3e40',
            clear_ube_type=3, clear_ube_vcp_io_id_fallback=[4, 5],
            clear_ube_last_fallback='same codec DMA device',
            io_device_ids_are_not_sids=True, secure_types_not_supported_by_wire=True),
        wait=dict(request=0xc007, reply=0xd007, bytes=40,
            status_is_hardware_selector=True, host_timeout_ms=500,
            command_argument=1, replies_with_actual_wait_result=True,
            reply_context='host context',
            zero_selector_constant0_branch_is_not_trace_bug=True,
            nonzero_selector_kept_across_trace_off_path=True,
            no_irq_owner_or_pixels_proof=True),
        function_anchors=functions, instruction_evidence=evidence)
    return result


def captured_function_spans(elf, anchors, spans):
    """Exact section/value/type/size, not name uniqueness across local variants.

    Call only AFTER the full-file/build-id gate. A decoder KO contains two
    local vdec_get_fb/vdec_get_bs implementations; only the anchored VCP ones
    are relevant. Section offsets and unresolved relocations are not live VA.
    """
    index, sec = next((i, s) for i, s in enumerate(elf.sections) if s['name'] == '.text')
    functions = {}
    for name, start, size in anchors:
        matches = [s for s in elf.symbols if s['name'] == name and
                   s['section'] == index and s['type'] == 2 and
                   s['value'] == start and s['bytes'] == size]
        if len(matches) != 1:
            raise ValueError(f'captured section/value function anchor mismatch: {name}')
        functions[name] = dict(section='.text', offset=start, bytes=size)
    text, evidence = elf.data(sec), []
    for label, name, start, end in spans:
        f = functions[name]
        if not f['offset'] <= start < end <= f['offset'] + f['bytes']:
            raise ValueError(f'captured instruction span outside function: {name}')
        evidence.append(dict(name=label, function=name, section='.text', start=start,
            end_exclusive=end, file_offset=sec['offset'] + start,
            raw_hex=span(text, start, end - start).hex()))
    return functions, evidence


def captured_rela(elf, section, offset, expected):
    """Resolve ONE relocation using ITS linked symtab, never a flattened index.

    expected = (type, symbol name, section name or None, symbol type/value,
    addend). Call only after a full-file gate. This is link-time evidence, not
    a relocated runtime pointer or permission to dereference any address.
    """
    sections = [(i, s) for i, s in enumerate(elf.sections) if s['name'] == section]
    if len(sections) != 1:
        raise ValueError('captured relocation requires unique target section')
    index, target = sections[0]
    if not 0 <= offset < target['bytes']:
        raise ValueError('captured relocation outside target section')
    found = []
    for rel in elf.sections:
        if rel['type'] not in (4, 9) or rel['info'] != index:
            continue
        step = 24 if rel['type'] == 4 else 16
        if rel['entry'] != step or rel['bytes'] % step:
            raise ValueError('invalid captured relocation table')
        data = elf.data(rel)
        for at in range(0, len(data), step):
            if struct.unpack_from('<Q', data, at)[0] != offset:
                continue
            if rel['type'] != 4 or not 0 < rel['link'] < len(elf.sections):
                raise ValueError('captured relocation requires RELA and linked symtab')
            _, info, addend = struct.unpack_from('<QQq', data, at)
            table = elf.sections[rel['link']]
            if (table['type'] != 2 or table['entry'] != 24 or table['bytes'] % 24 or
                    not 0 < table['link'] < len(elf.sections)):
                raise ValueError('invalid relocation-linked symbol table')
            symbol_index = info >> 32
            row = span(elf.data(table), symbol_index * 24, 24)
            name, si, _, sh, value, size = struct.unpack('<IBBHQQ', row)
            name = cstring(elf.data(elf.sections[table['link']]), name)
            symbol_section = None if sh == 0 else (
                elf.sections[sh]['name'] if sh < len(elf.sections) else 'special')
            actual = (info & 0xffffffff, name, symbol_section, si & 15, value, addend)
            if actual != expected:
                raise ValueError(f'captured relocation mismatch at {section}+{offset:x}')
            found.append(dict(section=section, offset=offset, relocation_section=rel['name'],
                relocation_type=actual[0], symbol_index=symbol_index,
                linked_symbol_table=table['name'], symbol_name=name,
                symbol_section=symbol_section, symbol_type=si & 15,
                symbol_value=value, symbol_bytes=size, addend=addend,
                raw_hex=span(data, at, 24).hex(), runtime_pointer_proven=False))
    if len(found) != 1:
        raise ValueError('missing/ambiguous captured relocation')
    return found[0]


def vcp_bootstrap_evidence(elf, build_id):
    """Captured global heap/GPR/reset/shutdown ABI, not Android preservation."""
    result = dict(profile_match=False, runtime_boot_gprs_captured=False,
        runtime_ready_proven=False, runtime_reset_proven=False,
        runtime_preserve_proven=False, runtime_mapping_proven=False,
        allocation_ownership_proven=False, module_firmware_pairing_proven=False)
    if (build_id != '40d64438b091c055bb2176abb1e97a393549a8d0' or
            hashlib.sha256(elf.blob).hexdigest() !=
            '2fc6ae2f71e245272eb1b5218bb5ec0e87e3cbb8e059ee75d3a8fd4cf2869a5f'):
        return result
    anchors = (('reset_vcp', 0x240c, 0x3b0),
               ('vcp_device_shutdown', 0x61b0, 0x1d8),
               ('vcp_reserve_memory_ioremap', 0x6a24, 0x680),
               ('vcp_logger_init', 0xaa10, 0x184))
    spans = (
        ('mmup_global_shared_size_logger_gprs', 'reset_vcp', 0x24a8, 0x24f8),
        ('mmup_reset_release_cmd6_a2_1', 'reset_vcp', 0x2510, 0x2540),
        ('vcp_global_shared_size_logger_plus_stride_gprs', 'reset_vcp', 0x2610, 0x2668),
        ('vcp_reset_release_cmd3_a2_1', 'reset_vcp', 0x2680, 0x26b0),
        ('shutdown_flag_and_suspend_early_exit', 'vcp_device_shutdown', 0x61e4, 0x6230),
        ('shutdown_clear_ready_u64', 'vcp_device_shutdown', 0x6230, 0x6250),
        ('shutdown_vcp_notifier_event1', 'vcp_device_shutdown', 0x6284, 0x6298),
        ('shutdown_mmup_notifier_event1', 'vcp_device_shutdown', 0x62e0, 0x62f4),
        ('shutdown_cfg_ap_requests_and_conditional_reboot_wait', 'vcp_device_shutdown', 0x6324, 0x6364),
        ('sum_mblocks_and_uncached_heap_alloc', 'vcp_reserve_memory_ioremap', 0x6d68, 0x6e08),
        ('vmap_attach_map_and_first_sg_dma_address', 'vcp_reserve_memory_ioremap', 0x6e28, 0x6e84),
        ('global_shared_and_logger_record_sources', 'vcp_reserve_memory_ioremap', 0x6f4c, 0x6f80),
        ('logger_cold_header_and_zero_indices', 'vcp_logger_init', 0xaad4, 0xab14),
        ('logger_capacity_comparison', 'vcp_logger_init', 0xaac0, 0xaac8),
    )
    functions, evidence = captured_function_spans(elf, anchors, spans)
    rodata = next(s for s in elf.sections if s['name'] == '.rodata')
    heap_name = cstring(elf.data(rodata), 0x34c)
    if heap_name != 'mtk_mm-uncached':
        raise ValueError('captured bootstrap heap mismatch')
    # Calls are unresolved in the KO; explicitly bind decisive external symbols.
    relocations = [captured_rela(elf, '.text', offset, (283, name, None, 0, 0, 0))
        for offset, name in ((0x253c, '__arm_smccc_smc'), (0x26ac, '__arm_smccc_smc'),
                            (0x6294, 'blocking_notifier_call_chain'),
                            (0x62f0, 'blocking_notifier_call_chain'),
                            (0x6dec, 'dma_heap_find'), (0x6e04, 'dma_heap_buffer_alloc'),
                            (0x6e34, 'dma_buf_vmap_unlocked'), (0x6e48, 'dma_buf_attach'),
                            (0x6e64, 'dma_buf_map_attachment_unlocked'))]
    # Resolve the DATA reference too, not just external calls. .data+0x508
    # is record[2]+8 because the array starts at 0x4c0, NOT record[1].
    # Keeping an unchecked prose index here previously mislabelled VENC as
    # the logger, even though the extracted instruction bytes were correct.
    raw_mblocks, mblocks = elf.named_data('vcp_reserve_mblock')
    if mblocks != dict(section='.data', offset=0x4c0, bytes=12 * 32):
        raise ValueError('captured bootstrap mblock object mismatch')
    table_refs = [captured_rela(elf, '.text', offset, (kind, '', '.data', 3, 0, addend))
        for offset, kind, addend in ((0x6cf4, 275, 0x4c0), (0x6cf8, 277, 0x4c0),
                                    (0x6f50, 275, 0x508), (0x6f54, 277, 0x508))]
    logger_dma = table_refs[-1]['symbol_value'] + table_refs[-1]['addend']
    logger_bytes = logger_dma + 0x10  # ldr w9,[x9,#0x10] at 0x6f6c
    logger_index, member = divmod(logger_dma - mblocks['offset'], 32)
    if member != 8 or not 0 <= logger_index < len(raw_mblocks) // 32:
        raise ValueError('captured logger reference is not an mblock DMA field')
    relocations += table_refs
    result.update(profile_match=True, profile='captured MT6993 global VCP bootstrap ABI 1',
        function_anchors=functions, instruction_evidence=evidence,
        relocation_evidence=relocations,
        boot_gprs=dict(cfg_offsets=[0x44, 0x48, 0x4c], bytes_per_bank=12,
            mmup_cfg_vcpreg_offset=0x28, vcp_cfg_vcpreg_offset=0x18,
            fields=['packed_global_shared_iova', 'global_shared_bytes', 'packed_logger_iova'],
            packed_iova='(low32 & ~15) | ((high32 & 15))',
            unpacked_iova='(packed & ~15) | ((packed & 15) << 32)',
            mmup_logger=f'record[{logger_index}].dma_address',
            vcp_logger=f'record[{logger_index}].dma_address + record[{logger_index}].bytes',
            second_logger_extent_proven=False, same_reset_epoch_required=True,
            byte_consistency_is_not_freshness_or_ready=True),
        allocation=dict(heap=heap_name, mblock_count=12, mblock_stride=32,
            logger_record_index=logger_index, logger_dma_data_offset=logger_dma,
            logger_bytes_data_offset=logger_bytes, mblock_evidence=mblocks,
            logger_index_derivation='(relocated_data_offset - array_offset - DMA_member8) / stride32',
            shared_base_source='first SG DMA address',
            address_is_not_cpu_pa=True, whole_iova_contiguity_proven=False,
            distinct_from_service_c005_type8_shm=True),
        logger_cold_header=dict(header_bytes=32, capacity_must_exceed=0x100020,
            le32_words=[0x504c5402, 24, 0, 24, 32, 0x100000, 0, 0],
            logger_ipi_registration_is_separate=True, runtime_ready_proven=False),
        reset_release=dict(smc_function_id=0xc200052c, mmup_cmd=6, vcp_cmd=3, a2=1,
            distinct_from_lk_signed_loader=True, commands_not_executed=True),
        shutdown=dict(normal_path_clears_ready_bytes=8, notifier_event=1,
            cfg_ap_offset=0x28, request_values=[0x4000, 0x400],
            second_request_requires_dual_core=True, suspend_early_exit_exists=True,
            physical_reset_completion_proven=False,
            android_ready_and_dma_must_not_be_assumed_reusable=True))
    return result


def smmu_shutdown_evidence(elf, build_id):
    """MM/APU policy distinction; static hooks do not prove shutdown ran."""
    result = dict(profile_match=False, runtime_shutdown_executed=False,
        runtime_disable_proven=False, runtime_preserve_proven=False,
        runtime_mapping_proven=False, context_ownership_proven=False)
    if (build_id != '3b2760f31edabaeb1b769ac3fcec55979f4cd616' or
            hashlib.sha256(elf.blob).hexdigest() !=
            '5626de268a9242170c9b3d1a9109c7af7db8e79ecaf418e9ed0e13f4ad79e20d'):
        return result
    anchors = (('arm_smmu_device_shutdown', 0x47a8, 0x10c),
               ('arm_smmu_device_disable', 0x8550, 0x58),
               ('mtk_smmu_v3_impl_init', 0xe410, 0xab0),
               ('mtk_smmu_skip_shutdown', 0x13578, 0x14))
    spans = (
        ('skip_shutdown_hook_and_bit0_branch', 'arm_smmu_device_shutdown', 0x47b8, 0x47f0),
        ('power_get_hook_guard_then_disable', 'arm_smmu_device_shutdown', 0x47f0, 0x4830),
        ('disable_sync_zero_cr0_cr0ack_arguments', 'arm_smmu_device_disable', 0x8560, 0x8578),
        ('install_impl_at_smmu_plus38', 'mtk_smmu_v3_impl_init', 0xe4c8, 0xe4f4),
        ('mm_compatible_dispatch', 'mtk_smmu_v3_impl_init', 0xe5b0, 0xe5c4),
        ('mm_platform_data_selection', 'mtk_smmu_v3_impl_init', 0xe680, 0xe68c),
        ('store_platform_data_at_wrapper_base', 'mtk_smmu_v3_impl_init', 0xe6ac, 0xe6cc),
        ('skip_shutdown_feature_byte_bit7', 'mtk_smmu_skip_shutdown', 0x13578, 0x1358c),
    )
    functions, evidence = captured_function_spans(elf, anchors, spans)
    rows = []
    for name, offset, features, kind in (('mm', 0x668, 0xd60, 0),
            ('apu', 0x674, 0x9e0, 1), ('soc', 0x680, 0x904, 2), ('gpu', 0x68c, 0x1920, 3)):
        raw, where = elf.named_data('mt6993_data_' + name)
        if (where != dict(section='.rodata', offset=offset, bytes=12) or
                raw != struct.pack('<3I', 2, features, kind)):
            raise ValueError('captured SMMU platform data mismatch')
        rows.append(dict(name=name, evidence=where, raw_hex=raw.hex(), raw_words=[2, features, kind],
            feature_word=features, skip_shutdown_bit=7, skip_shutdown=bool(features & 0x80)))
    impl = [s for s in elf.symbols if s['name'] == 'mtk_smmu_impl' and s['type'] == 1 and
            s['value'] == 0x478 and s['bytes'] == 0x160 and
            0 < s['section'] < len(elf.sections) and
            elf.sections[s['section']]['name'] == '.rodata']
    if len(impl) != 1:
        raise ValueError('captured SMMU impl anchor mismatch')
    rodata = elf.sections[impl[0]['section']]
    compatible = cstring(elf.data(rodata), 0x4494)
    if compatible != 'mediatek,mt6993-mm-smmu':
        raise ValueError('captured MM SMMU compatible mismatch')
    relocations = [captured_rela(elf, '.rodata', 0x478 + field,
                               (257, '', '.text', 3, 0, address))
                  for field, address in ((0xc8, 0x13578), (0x30, 0xf394), (0x38, 0xfda8))]
    relocations += [captured_rela(elf, '.text', offset, (283, name, '.text', 2, value, 0))
        for offset, name, value in ((0x482c, 'arm_smmu_device_disable', 0x8550),
                                   (0x8570, 'arm_smmu_write_reg_sync', 0x8248))]
    # Both call targets are defined in this KO; do not treat them as imports.
    result.update(profile_match=True, profile='captured MT6993 MM SMMU shutdown policy 1',
        function_anchors=functions, instruction_evidence=evidence,
        platform_data=rows, relocation_evidence=relocations,
        impl=dict(section='.rodata', offset=0x478, bytes=0x160,
            skip_shutdown_hook_offset=0xc8, smmu_wrapper_offset=0x1440,
            smmu_impl_offset=0x38, feature_byte_offset=4),
        mm_compatible=compatible, mm_skip_shutdown=False, apu_skip_shutdown=True,
        disable_path_requires_power_get_success=True,
        power_get_hook=dict(offset=0x30, section='.text', address=0xf394,
            name='mtk_smmu_power_get', runtime_result_not_observed=True),
        disable_sync=dict(value=0, cr0_offset=0x20, ack_offset=0x24,
            runtime_registers_not_captured=True),
        mm_mapping_must_not_be_assumed_preserved=True,
        apu_skip_policy_does_not_authorize_mm=True)
    return result


def smmu_normal_bootstrap_evidence(elf, shutdown):
    """Captured normal host publication and its conditional EL2 share callee.

    Unpatched module text contains an early BUG branch and a NOP, not the
    loaded CPU-capability decision. Resolve BOTH alternatives through their
    linked RELA symtabs; never execute the SMC/HVC or invent an NS grant.
    """
    result = dict(profile_match=False, runtime_controller_owned_proven=False,
        runtime_alternatives_applied_proven=False, runtime_protected_mode_proven=False,
        runtime_hyp_registration_proven=False, runtime_ns_authority_proven=False)
    if not shutdown['profile_match']:
        return result
    anchors = (('arm_smmu_init_structures', 0x48b4, 0x1c0),
               ('arm_smmu_device_reset', 0x4c00, 0x6a8),
               ('arm_smmu_write_strtab', 0x89e0, 0x7c),
               ('mtk_smmu_share_mem_to_hyp', 0xe34c, 0xc0))
    spans = (
        ('private_structure_initialization', 'arm_smmu_init_structures', 0x48b4, 0x4a74),
        ('stream_table_write_then_share_type1', 'arm_smmu_device_reset', 0x4ce0, 0x4d1c),
        ('cmdq_base_prod_cons_then_share_type0', 'arm_smmu_device_reset', 0x4d1c, 0x4d84),
        ('cmdq_enable_cr0_8_and_cr0ack', 'arm_smmu_device_reset', 0x4d84, 0x4d9c),
        ('unpatched_capability_gates', 'mtk_smmu_share_mem_to_hyp', 0xe360, 0xe38c),
        ('dynamic_hvc_id_smc', 'mtk_smmu_share_mem_to_hyp', 0xe38c, 0xe3bc),
        ('zero_hvc_id_logs_and_returns', 'mtk_smmu_share_mem_to_hyp', 0xe3bc, 0xe3d8),
        ('registered_hvc_call_and_result_check', 'mtk_smmu_share_mem_to_hyp', 0xe3d8, 0xe408),
    )
    functions, instructions = captured_function_spans(elf, anchors, spans)
    alt = [s for s in elf.sections if s['name'] == '.altinstructions']
    if len(alt) != 1:
        raise ValueError('missing/ambiguous captured alternatives section')
    entries = []
    for offset, expected in ((0x1ec, (0x8001, 4, 0)), (0x1f8, (55, 4, 4))):
        raw = span(elf.data(alt[0]), offset, 12)
        orig, replacement, capability, original_bytes, replacement_bytes = struct.unpack('<iiHBB', raw)
        if (orig, replacement) != (0, 0) or (capability, original_bytes, replacement_bytes) != expected:
            raise ValueError('captured alternative entry mismatch')
        entries.append(dict(section='.altinstructions', offset=offset,
            capability_id=capability & 0x7fff, callback=bool(capability & 0x8000),
            original_bytes=original_bytes, replacement_bytes=replacement_bytes,
            raw_hex=raw.hex(), runtime_selection_proven=False))
    relocations = [
        captured_rela(elf, '.altinstructions', 0x1ec, (261, '', '.text', 3, 0, 0xe360)),
        captured_rela(elf, '.altinstructions', 0x1f0, (261, 'alt_cb_patch_nops', None, 0, 0, 0)),
        captured_rela(elf, '.altinstructions', 0x1f8, (261, '', '.text', 3, 0, 0xe368)),
        captured_rela(elf, '.altinstructions', 0x1fc, (261, '', '.text', 3, 0, 0x144c8)),
        captured_rela(elf, '.rodata', 0x568, (257, 'mtk_smmu_share_mem_to_hyp', '.text', 2, 0xe34c, 0)),
        captured_rela(elf, '.text', 0x4ce4, (283, 'arm_smmu_write_strtab', '.text', 2, 0x89e0, 0)),
        captured_rela(elf, '.text', 0x4d94, (283, 'arm_smmu_write_reg_sync', '.text', 2, 0x8248, 0)),
    ]
    text = elf.data(next(s for s in elf.sections if s['name'] == '.text'))
    word = struct.unpack('<I', span(text, 0x144c8, 4))[0]
    immediate = word & 0x3ffffff
    if immediate & (1 << 25):
        immediate -= 1 << 26
    if word >> 26 != 5 or 0x144c8 + immediate * 4 != 0xe384:
        raise ValueError('captured protected-mode alternative branch mismatch')
    result.update(profile_match=True, profile='captured MT6993 normal SMMU bootstrap/share ABI 1',
        function_anchors=functions, instruction_evidence=instructions,
        relocation_evidence=relocations, alternatives=entries,
        alternative_branch=dict(section='.text', offset=0x144c8,
            target_offset=0xe384, raw_hex=span(text, 0x144c8, 4).hex()),
        source_capability_names=dict(capability1='ARM64_ALWAYS_SYSTEM',
            capability55='ARM64_KVM_PROTECTED_MODE', names_are_source_correlated_not_runtime_state=True),
        normal_publication=dict(impl_mem_share_offset=0xf0,
            stream_table_share_type=1, command_queue_share_type=0,
            cmdq_register_offsets=dict(base=0x90, producer=0x98, consumer=0x9c),
            cmdq_enable=8, cr0_offset=0x20, cr0ack_offset=0x24,
            publication_precedes_share=True, does_not_authorize_reset_of_foreign_streams=True),
        hyp_share=dict(smc_id=0xbb00ffcd, zero_argument_registers=[1, 2, 3, 4, 5, 6],
            smc_a0_not_checked_by_callee=True, hvc_id_from_smc_a1=True,
            zero_hvc_id_logs_without_share=True, hvc_function_prefix=0xc6000000,
            hvc_id_low_bits=16, hvc_x1='platform smmu type', hvc_x2='memory share type',
            hvc_nonzero_result_reaches_bug=True, smmu_type_is_not_sid=True,
            memory_share_is_not_ns_lease_or_dma_permission=True,
            callee_does_not_register_hyp_handler_or_create_ns_controller=True))
    return result


def vcp_ready_evidence(elf, bootstrap):
    """READY channel framing and deferred state publication, not live events."""
    result = dict(profile_match=False, runtime_ready_proven=False,
        runtime_event_received=False, current_reset_epoch_proven=False,
        firmware_sender_proven=False, module_firmware_pairing_proven=False)
    if not bootstrap['profile_match']:
        return result
    anchors = (('vcp_device_probe', 0x4d40, 0x13c0), ('mmup_init', 0xd180, 0x80),
               ('vcp_A_ready_ipi_handler', 0x1618, 0x150),
               ('vcp_A_notify_ws', 0x70a8, 0x11c))
    spans = (
        ('expected_sram_size_from_dt', 'vcp_device_probe', 0x546c, 0x54a4),
        ('dual_core_mmup_init', 'vcp_device_probe', 0x5dfc, 0x5e24),
        ('two_notify_work_callbacks', 'vcp_device_probe', 0x5e6c, 0x5ec8),
        ('vcp_ready_channel5_registration', 'vcp_device_probe', 0x5ecc, 0x5eec),
        ('mmup_ready_channel26_registration', 'mmup_init', 0xd1b8, 0xd1dc),
        ('payload_word_and_vcp_already_ready_check', 'vcp_A_ready_ipi_handler', 0x1628, 0x1648),
        ('vcp_work_index0', 'vcp_A_ready_ipi_handler', 0x1654, 0x1680),
        ('mmup_ready_index_selection', 'vcp_A_ready_ipi_handler', 0x1680, 0x16a4),
        ('mmup_work_index1_and_queue', 'vcp_A_ready_ipi_handler', 0x16b0, 0x16e4),
        ('sram_size_check_after_work_queueing', 'vcp_A_ready_ipi_handler', 0x16e4, 0x171c),
        ('deferred_ready_store_under_mutex', 'vcp_A_notify_ws', 0x70b8, 0x70ec),
        ('notifier_event0_selected_by_index', 'vcp_A_notify_ws', 0x713c, 0x7160),
    )
    functions, evidence = captured_function_spans(elf, anchors, spans)
    rodata = next(s for s in elf.sections if s['name'] == '.rodata')
    if cstring(elf.data(rodata), 0x3065) != 'vcp-sram-size':
        raise ValueError('captured READY SRAM-size property mismatch')
    relocations = [captured_rela(elf, '.text', offset, (283, name, None, 0, 0, 0))
        for offset, name in ((0x5ee8, 'mtk_ipi_register'), (0xd1d8, 'mtk_ipi_register'),
                            (0x16e0, 'queue_work_on'), (0x715c, 'blocking_notifier_call_chain'))]
    result.update(profile_match=True, profile='captured MT6993 dual READY receive ABI 1',
        function_anchors=functions, instruction_evidence=evidence,
        relocation_evidence=relocations, payload_bytes=4,
        payload_field='vcp-sram-size, not firmware payload length',
        expected_dt_property='vcp-sram-size', expected_vcpreg_offset=0x88,
        controllers=[dict(name='vcp', channel=5, dual_core_ready_index=0),
                     dict(name='mmup', channel=26, dual_core_ready_index=1)],
        single_core_mmup_index_falls_back_to0=True,
        notification_event=0, shutdown_event=1,
        stock_queues_work_before_size_validation=True,
        size_mismatch_traps_but_work_may_already_be_queued=True,
        deferred_worker_sets_ready_without_epoch_check=True,
        ready_word_has_no_epoch_nonce_or_signature=True,
        xnu_must_validate_before_state_publication=True,
        registered_handler_is_not_event_receipt=True)
    return result


def smmu_power_sync_evidence(elf, shutdown):
    """Power hook conventions and MM's timeout suppression; never MMIO."""
    result = dict(profile_match=False, runtime_power_ownership_proven=False,
        runtime_ack_captured=False, runtime_mapping_proven=False,
        fallback_pm_provider_implementation_proven=False)
    if not shutdown['profile_match']:
        return result
    anchors = (('mtk_smmu_power_get', 0xf394, 0xfc), ('mtk_smmu_power_put', 0xfda8, 0xe0),
               ('arm_smmu_write_reg_sync', 0x8248, 0x168),
               ('mtk_smmu_skip_sync_timeout', 0x13590, 0x18),
               ('mkt_get_smmu_data', 0xf1b8, 0x24),
               ('mtk_smmu_v3_impl_init', 0xe410, 0xab0))
    spans = (
        ('power_get_bit2_and_optional_callback_positive_success', 'mtk_smmu_power_get', 0xf3a4, 0xf3f4),
        ('power_get_fallback_pm_and_wakeup_rollback', 'mtk_smmu_power_get', 0xf404, 0xf448),
        ('power_get_callback_failure_minus1', 'mtk_smmu_power_get', 0xf488, 0xf490),
        ('power_put_bit2_and_optional_callback_nonnegative_success', 'mtk_smmu_power_put', 0xfdb8, 0xfe04),
        ('power_put_fallback_pm_and_wakeup_release', 'mtk_smmu_power_put', 0xfe14, 0xfe40),
        ('sync_write_then_ack_equality_and_timeout', 'arm_smmu_write_reg_sync', 0x8260, 0x82fc),
        ('sync_success_zero', 'arm_smmu_write_reg_sync', 0x82fc, 0x8318),
        ('timeout_dump_and_skip_hook', 'arm_smmu_write_reg_sync', 0x8318, 0x837c),
        ('unsuppressed_timeout_minus110', 'arm_smmu_write_reg_sync', 0x837c, 0x8384),
        ('suppressed_timeout_log_then_success_zero', 'arm_smmu_write_reg_sync', 0x8384, 0x83b0),
        ('skip_sync_timeout_only_type0_mm', 'mtk_smmu_skip_sync_timeout', 0x13590, 0x135a8),
        ('type0_to3_registered_wrapper_lookup', 'mkt_get_smmu_data', 0xf1b8, 0xf1dc),
        ('register_wrapper_and_global_smmu_ops', 'mtk_smmu_v3_impl_init', 0xecb0, 0xece4),
    )
    functions, evidence = captured_function_spans(elf, anchors, spans)
    relocations = [captured_rela(elf, '.rodata', offset, (257, '', '.text', 3, 0, value))
        for offset, value in ((0x548, 0x13590), (0x5d8, 0xf1b8),
                              (0x5f0, 0xf394), (0x5f8, 0xfda8))]
    relocations += [captured_rela(elf, '.text', offset, (283, name, None, 0, 0, 0))
        for offset, name in ((0xf41c, 'mtk_smmu_pm_get'), (0xfe18, 'mtk_smmu_pm_put'),
                            (0xecd8, 'mtk_smmu_set_ops'))]
    result.update(profile_match=True, profile='captured MM SMMU power and ACK policy 1',
        function_anchors=functions, instruction_evidence=evidence, relocation_evidence=relocations,
        optional_pm_ops=dict(wrapper_offset=8, get_offset=0, put_offset=8,
            get_success='strictly positive callback result, normalized to0',
            put_success='nonnegative callback result, normalized to0',
            missing_callback_uses_fallback=True, callback_failure_result=-1),
        fallback_pm=dict(type_field_offset=8, mm_type=0,
            get_success='exact0 returned by mtk_smmu_pm_get',
            put_result_preserved=True, functions_are_not_mtk_smmu_rpm_get_put=True,
            wakeup_flag_smmu_offset=0x4f8, wakeup_object_smmu_offset=0x518),
        feature_bit2_bypasses_power_hooks=True, mm_feature_bit2=False,
        global_rpm_ops=dict(section='.rodata', offset=0x5d8, bytes=0x30,
            lookup_offset=0, get_offset=0x18, put_offset=0x20,
            returns_registered_wrapper_not_runtime_smmu_va=True),
        sync=dict(timeout_ns=1000000000, timeout_errno=-110,
            timeout_skip_hook_impl_offset=0xd0, mm_skip_sync_timeout=True,
            other_platform_types_skip_sync_timeout=False,
            mm_timeout_can_return0_with_ack_mismatch=True,
            return0_is_not_ack_or_dma_permission=True,
            xnu_must_compare_fresh_cr0_and_cr0ack=True))
    return result


def iommu_util_rpm_evidence(elf, build_id):
    """Bound the small RPM forwarding module; callbacks are not PM ownership."""
    result = dict(profile_match=False, runtime_ops_registration_proven=False,
        runtime_power_ownership_proven=False, runtime_mapping_proven=False)
    if (build_id != 'e2d26ff7bcbc4c9116eb417ebff57e917a6be29d' or
            hashlib.sha256(elf.blob).hexdigest() !=
            '9695245fef4772cc1cf6079a996be8f527c965399f0120541541160a9cbde127'):
        return result
    anchors = (('mtk_smmu_set_ops', 0xcc, 0x20), ('mtk_smmu_rpm_get', 0xf0, 0x98),
               ('mtk_smmu_rpm_put', 0x18c, 0x98))
    spans = (
        ('first_ops_pointer_wins', 'mtk_smmu_set_ops', 0xcc, 0xec),
        ('rpm_get_lookup_type_callback', 'mtk_smmu_rpm_get', 0x100, 0x13c),
        ('rpm_get_wrapper_plus1440_and_ops18', 'mtk_smmu_rpm_get', 0x13c, 0x174),
        ('rpm_get_missing_ops_minus1', 'mtk_smmu_rpm_get', 0x174, 0x178),
        ('rpm_put_lookup_type_callback', 'mtk_smmu_rpm_put', 0x19c, 0x1d8),
        ('rpm_put_wrapper_plus1440_and_ops20', 'mtk_smmu_rpm_put', 0x1d8, 0x210),
        ('rpm_put_missing_ops_minus1', 'mtk_smmu_rpm_put', 0x210, 0x214),
    )
    functions, evidence = captured_function_spans(elf, anchors, spans)
    result.update(profile_match=True, profile='captured mtk_iommu_util RPM forwarding ABI 1',
        function_anchors=functions, instruction_evidence=evidence,
        global_ops_storage=dict(section='.bss', offset=8, runtime_value_not_captured=True),
        registration_first_nonzero_pointer_wins=True,
        lookup_callback_offset=0, wrapper_smmu_offset=0x1440,
        get_callback_offset=0x18, put_callback_offset=0x20,
        absent_registration_lookup_or_callback_returns=-1,
        callback_result_returned_unchanged=True,
        not_fallback_mtk_smmu_pm_get_put=True,
        loaded_note_does_not_prove_current_ops_pointer=True)
    return result


def decoder_output_evidence(elf, common):
    """Nonempty a005 layout from the paired decoder file, NOT a provider."""
    result = dict(profile_match=False, runtime_output_registration_proven=False,
                  runtime_output_ownership_proven=False, runtime_pixels_proven=False,
                  module_firmware_pairing_proven=False)
    if not common['profile_match']:
        return result
    anchors = (
        ('vdec_vcp_set_param', 0x265fc, 0x888),
        ('vdec_vcp_ipi_send', 0x230e0, 0x97c),
    )
    spans = (
        ('output_once_initialization_and_a005_prefix', 'vdec_vcp_set_param', 0x267f0, 0x26834),
        ('output_buffer_selected_token', 'vdec_vcp_set_param', 0x2685c, 0x26870),
        ('c_plane_conditional_update_no_else_clear', 'vdec_vcp_set_param', 0x26888, 0x2689c),
        ('output_y_index_general_length_and_iova', 'vdec_vcp_set_param', 0x2689c, 0x268c8),
        ('general_absent_sentinel_no_size_clear', 'vdec_vcp_set_param', 0x268fc, 0x2690c),
        ('output_token_y_c_general_size32_index_send64', 'vdec_vcp_set_param', 0x2695c, 0x26988),
        ('output_loop_reuses_stack_fields', 'vdec_vcp_set_param', 0x2698c, 0x2699c),
        ('sender_once_zero72_and_nonzero_alias_gate', 'vdec_vcp_ipi_send', 0x23124, 0x2314c),
        ('a005_separate_device_mutex_selection', 'vdec_vcp_ipi_send', 0x23160, 0x23188),
        ('a005_outer_service2_separate_reply_state', 'vdec_vcp_ipi_send', 0x233e0, 0x23424),
        ('ordinary_service_not_output_service2', 'vdec_vcp_ipi_send', 0x23424, 0x23450),
    )
    functions, evidence = captured_function_spans(elf, anchors, spans)
    result.update(profile_match=True, profile='MT6993 captured a005 output ABI 1',
        wire_output_abi=1, message=0xa005, reply=0xb005, service=2, payload_bytes=64,
        transport_channels=dict(out=0, incoming=1),
        fields=[dict(name='message', offset=0, width=4),
                dict(name='host_context', offset=4, width=4),
                dict(name='firmware_instance_alias_not_ap_token', offset=8, width=8),
                dict(name='parameter_id_zero', offset=16, width=4),
                dict(name='reserved_zero', offset=20, width=4),
                dict(name='output_buffer_token', offset=24, width=8),
                dict(name='y_iova', offset=32, width=8),
                dict(name='c_iova', offset=40, width=8),
                dict(name='general_iova', offset=48, width=8),
                dict(name='general_size_s32', offset=56, width=4),
                dict(name='buffer_index_not_ring_index', offset=60, width=4)],
        no_general_iova_sentinel=0xffffffffffffffff,
        stock_initialization_once_not_per_output=True,
        stock_missing_c_can_retain_previous_c=True,
        stock_missing_general_can_retain_previous_size=True,
        stock_general_u64_length_truncated_to_wire_s32=True,
        separate_a005_mutex_offsets=[0x2538, 0x2568],
        separate_a005_reply_state_offset=0x64,
        record_generation_mapping_cache_and_lifetime_must_be_provider_owned=True,
        no_flush_or_eos_or_linear_format_inferred=True,
        function_anchors=functions, instruction_evidence=evidence)
    return result


def decoder_clock_ring_evidence(elf, common):
    result = dict(profile_match=False, runtime_clock_and_irq_ownership_proven=False,
                  runtime_ring_completion_proven=False, runtime_pixels_proven=False)
    if not common['profile_match']:
        return result
    anchors = (
        ('vcp_dec_ipi_handler', 0x21654, 0x17cc),
        ('vdec_decode_prepare', 0x16590, 0x134),
        ('vdec_decode_unprepare', 0x166c8, 0x1c4),
        ('vdec_get_fb_list', 0x23a5c, 0x198),
        ('vdec_vcp_get_param', 0x25cb0, 0x948),
        ('vdec_get_bs', 0x27220, 0x164),
        ('vdec_get_fb', 0x27384, 0x330),
    )
    spans = (
        ('clock_status0_context_host_and_dispatch', 'vcp_dec_ipi_handler', 0x21b00, 0x21b2c),
        ('c001_core_payload64_ao_clock_count', 'vcp_dec_ipi_handler', 0x22330, 0x22370),
        ('c002_core_payload64_ao_inverse', 'vcp_dec_ipi_handler', 0x224c4, 0x224f8),
        ('core_full_unprepare_then_prepare_branches', 'vcp_dec_ipi_handler', 0x227d4, 0x22834),
        ('c003_lat_prepare_active_d003', 'vcp_dec_ipi_handler', 0x225dc, 0x22610),
        ('c004_lat_inactive_unprepare_d004', 'vcp_dec_ipi_handler', 0x21f28, 0x21f58),
        ('clock_shared_send40', 'vcp_dec_ipi_handler', 0x22070, 0x22088),
        ('prepare_lock_owner_clock_conditional_ap_irq', 'vdec_decode_prepare', 0x165ac, 0x16634),
        ('prepare_dvfs_pmqos_after_clock', 'vdec_decode_prepare', 0x16634, 0x16664),
        ('unprepare_pending_hardware_does_not_teardown', 'vdec_decode_unprepare', 0x16704, 0x16728),
        ('unprepare_qos_conditional_irq_clock_owner_unlock', 'vdec_decode_unprepare', 0x16728, 0x16804),
        ('shared_transfer_read_write_empty', 'vdec_get_fb_list', 0x23a98, 0x23aa8),
        ('shared_transfer_host_backpressure_copy24_consume', 'vdec_get_fb_list', 0x23b08, 0x23ba8),
        ('async_e000_error_display_free_transfer_no_ack', 'vcp_dec_ipi_handler', 0x22394, 0x22454),
        ('b005_status0_resource_signal_only', 'vcp_dec_ipi_handler', 0x222a4, 0x222b0),
        ('b005_resource_signal_status0_branch', 'vcp_dec_ipi_handler', 0x22544, 0x22554),
        ('b005_nonzero_status_signal_without_failure_store', 'vcp_dec_ipi_handler', 0x21d80, 0x21d9c),
        ('b005_shared_wake_not_irq_queue', 'vcp_dec_ipi_handler', 0x21bb8, 0x21bcc),
        ('free_bs_count_index8_and_owned_token_search', 'vdec_get_bs', 0x27238, 0x272a8),
        ('free_bs_consume_read_count', 'vdec_get_bs', 0x272f0, 0x27310),
        ('fb_count_index24_and_owned_token_search', 'vdec_get_fb', 0x273f0, 0x2744c),
        ('fb_raw_timestamp_field_frame_type_flags', 'vdec_get_fb', 0x274f4, 0x275c4),
        ('fb_consume_read_count', 'vdec_get_fb', 0x275d4, 0x275f0),
        ('get_param_bs_from_shared_vsi', 'vdec_vcp_get_param', 0x2630c, 0x26320),
        ('get_param_async_display_from_locked_host_ring', 'vdec_vcp_get_param', 0x26320, 0x26364),
        ('get_param_async_free_from_locked_host_ring', 'vdec_vcp_get_param', 0x25e24, 0x25e64),
    )
    functions, evidence = captured_function_spans(elf, anchors, spans)
    result.update(profile_match=True, profile='MT6993 captured clock and shared ring ABI 1',
        clock=dict(requests=[0xc001, 0xc002, 0xc003, 0xc004], bytes=40,
            reply_context='host context', status0_only=True, payload_offset=32,
            core_payload_nonzero_is_ao_clock_only=True, lat_payload_not_used_by_host=True,
            data_low_byte_is_dvfs_data=True, active_offsets=[0x26d8, 0x26dc],
            ao_count_offset=0x2458, full_prepare_order=['lock', 'publish owner', 'clock on',
                'conditional AP IRQ enable', 'DVFS/PMQoS begin'],
            vcp_mode_skips_ap_enable_irq=True, vcp_irq_owner_not_established=True,
            pending_unprepare_can_return_without_teardown=True,
            stock_status0_reply_not_operation_success_proof=True),
        rings=dict(wire_abi=1, capacity=64, shared_offsets=dict(free_bs=0, free_fb=0x210,
            display_fb=0x820), fb_stride=24, bs_stride=8,
            fb_metadata_offsets=[0x600, 0x604, 0x608],
            bs_metadata_offsets=[0x200, 0x204, 0x208],
            fb_entry_offsets=dict(token=0, timestamp=8, field=16, frame_type=18, raw_flags=20),
            shared_fb_transfer_empty='read == write',
            shared_fb_consumed_count='(write - read) & 63',
            shared_bs_empty='count == 0', shared_bs_consumer_count='count - 1',
            shared_bs_count_is_cross_endpoint_rmw=True,
            host_transfer_backpressure_at_count=64, shared_fb_count64_not_host_full_ring=True,
            stock_invalid_index64_not_copied_by_wire=True,
            token_requires_live_instance_record=True, async_message=0xe000,
            async_has_no_ack=True, b005_only_resource_queue_wakeup=True,
            b005_nonzero_status_does_not_store_failure_in_this_branch=True,
            no_dimensions_pixel_format_or_linearization_proven=True),
        function_anchors=functions, instruction_evidence=evidence)
    return result


def common_irq_owner_evidence(elf, vp):
    result = dict(profile_match=False, runtime_irq_owner_proven=False,
                  runtime_irq_completion_proven=False, runtime_pixels_proven=False)
    if not vp['profile_match']:
        return result
    anchors = (
        ('wake_up_dec_ctx', 0xa60, 0x4c),
        ('mtk_vcodec_dec_irq_handler', 0xab0, 0x304),
        ('mtk_vcodec_lat_dec_irq_handler', 0xeb4, 0x304),
        ('mtk_vcodec_dec_irq_setup', 0x16e4, 0x2c0),
        ('mtk_vcodec_set_curr_ctx', 0x30a8, 0x84),
        ('mtk_vcodec_get_curr_ctx', 0x3130, 0x7c),
    )
    spans = (
        ('owner_publish_under_irqsave_lock', 'mtk_vcodec_set_curr_ctx', 0x30c8, 0x30f0),
        ('owner_fetch_under_irqsave_lock', 'mtk_vcodec_get_curr_ctx', 0x3148, 0x3170),
        ('core_irq_select_owner0', 'mtk_vcodec_dec_irq_handler', 0xab0, 0xb20),
        ('core_secure_mode5_can_skip_mmio', 'mtk_vcodec_dec_irq_handler', 0xb98, 0xbc4),
        ('core_status_bit4_done_bit16_busy_bit4', 'mtk_vcodec_dec_irq_handler', 0xbc4, 0xbf8),
        ('core_ack_then_condition_queue', 'mtk_vcodec_dec_irq_handler', 0xbf8, 0xcb4),
        ('lat_irq_select_owner1', 'mtk_vcodec_lat_dec_irq_handler', 0xed0, 0xf34),
        ('lat_secure5_and_status_bit4_done_bit16', 'mtk_vcodec_lat_dec_irq_handler', 0xf34, 0xf88),
        ('lat_ack_then_condition_queue', 'mtk_vcodec_lat_dec_irq_handler', 0xf88, 0xff8),
        ('software_wake_can_set_same_condition', 'wake_up_dec_ctx', 0xa74, 0xa9c),
        ('irq_setup_platform_irq_and_initial_disable', 'mtk_vcodec_dec_irq_setup', 0x1754, 0x178c),
        ('irq_setup_request_threaded_no_thread_fn', 'mtk_vcodec_dec_irq_setup', 0x1830, 0x1870),
    )
    functions, evidence = captured_function_spans(elf, anchors, spans)
    result.update(profile_match=True, profile='MT6993 captured common IRQ owner instructions',
        owner=dict(lock_offset=0x128, device_owner_offset=0x2288, owner_stride=8,
            core_owner_slot=0, lat_owner_slot=1, owner_bound=4,
            core_condition_slots=[0, 2], lat_condition_slots=[1, 3]),
        irq=dict(condition_offset=0xce8, condition_stride=4, queue_offset=0xd00,
            queue_stride=24, status_saved_offset=0xd78,
            total_status_bit4_blocks_wake=True, selected_status_offset=0xa4,
            selected_done_bit=16, core_busy_offset=0x19c, core_busy_bit=4,
            secure_mode5_can_wake_without_mmio=True,
            software_wake_can_set_same_condition=True,
            condition1_is_not_physical_irq_evidence=True,
            setup_disables_registered_irq=True, no_register_addresses_inferred=True,
            no_mmio_or_irq_ack_executed=True),
        function_anchors=functions, instruction_evidence=evidence)
    return result


def common_mem_wait_evidence(elf, vp):
    """Current common DMA alias/extra extent and terminal wait wire statuses."""
    result = dict(profile_match=False, runtime_mapping_proven=False,
                  runtime_irq_completion_proven=False,
                  dma_mapping_and_cache_ownership_proven=False)
    if not vp['profile_match']:
        return result
    index, sec = next((i, s) for i, s in enumerate(elf.sections) if s['name'] == '.text')
    functions = dict(vp['function_anchors'])
    for name, start, size in (('mtk_vcodec_wait_for_done_ctx', 0x5c8, 0x234),
                              ('mtk_vcodec_dma_attach_map', 0x53a4, 0x1bc),
                              ('mtk_vcodec_free_mem', 0x598c, 0x164)):
        matches = [s for s in elf.symbols if s['name'] == name]
        if (len(matches) != 1 or matches[0]['section'] != index or
                matches[0]['type'] != 2 or matches[0]['value'] != start or
                matches[0]['bytes'] != size):
            raise ValueError(f'captured common mem/wait anchor mismatch: {name}')
        functions[name] = dict(section='.text', offset=start, bytes=size)
    spans = (
        ('wait_ctx_and_hw_less5', 'mtk_vcodec_wait_for_done_ctx', 0x5fc, 0x60c),
        ('wait_selected_condition_and_queue', 'mtk_vcodec_wait_for_done_ctx', 0x618, 0x66c),
        ('wait_result_and_clear_condition', 'mtk_vcodec_wait_for_done_ctx', 0x6c8, 0x6e0),
        ('wait_timeout_status_minus1', 'mtk_vcodec_wait_for_done_ctx', 0x7ac, 0x7b4),
        ('wait_interrupted_status_minus2', 'mtk_vcodec_wait_for_done_ctx', 0x7e8, 0x7f8),
        ('allocator_request_length_guard_and_zero_addresses', 'mtk_vcodec_alloc_mem', 0x5660, 0x5680),
        ('allocator_clear_heap_selection', 'mtk_vcodec_alloc_mem', 0x5680, 0x56d4),
        ('allocator_sw_extra128_not_wire_length', 'mtk_vcodec_alloc_mem', 0x5730, 0x5764),
        ('allocator_bidirectional_map_iova_dma_alias_token', 'mtk_vcodec_alloc_mem', 0x579c, 0x57dc),
        ('attach_map_first_sg_dma_address', 'mtk_vcodec_dma_attach_map', 0x5420, 0x5478),
        ('free_valid_token_type', 'mtk_vcodec_free_mem', 0x59ac, 0x59cc),
        ('free_unmap_detach_allocator_token', 'mtk_vcodec_free_mem', 0x59e4, 0x5a1c),
    )
    text, evidence = elf.data(sec), []
    for label, function, start, end in spans:
        anchor = functions[function]
        if not anchor['offset'] <= start < end <= anchor['offset'] + anchor['bytes']:
            raise ValueError('mem/wait evidence outside common function')
        evidence.append(dict(name=label, function=function, section='.text',
            start=start, end_exclusive=end, file_offset=sec['offset'] + start,
            raw_hex=span(text, start, end - start).hex()))
    result.update(profile_match=True, profile='MT6993 captured common mem/wait operations',
        allocator=dict(request_bound=0x20000000, clear_types=[0, 1, 2, 3],
            sw_extra_formula='round_up(request_length,128)+128',
            requested_length_field_not_allocation_size=True,
            iova_equals_dma_alias=True, pa_field_is_not_cpu_physical_address=True,
            va_field_is_allocator_object_token=True, map_direction=0,
            map_direction_meaning='DMA_BIDIRECTIONAL, not XNU cache permission',
            first_sg_dma_address_is_not_full_extent_contiguity_proof=True,
            cpu_vmap_not_performed_by_this_allocator=True),
        wait=dict(condition_offset=0xce8, selected_condition_stride=4,
            queue_offset=0xd00, selected_queue_stride=24,
            hardware_argument_bound=5, wire_supported_subset=[0, 1],
            condition_is_nonzero_not_command_comparison=True,
            statuses=dict(success=0, timeout_or_invalid=-1, interrupted=-2),
            terminal_wait_clears_selected_condition_and_int_type=True,
            condition_consumption_not_pixel_completion=True),
        function_anchors=functions, instruction_evidence=evidence)
    return result


def common_vp_evidence(elf, build_id):
    """Captured common KO's allocation, pattern and cache call instructions."""
    result = dict(profile_match=False, runtime_vp_proven=False,
                  dma_mapping_and_cache_ownership_proven=False,
                  module_firmware_pairing_proven=False)
    if (build_id != 'aca809858dda2fc48a73dfa661324e402d0361a9' or
            hashlib.sha256(elf.blob).hexdigest() !=
            'de863471e77b05cdec514c7c523c12a785fc1c81e603bdf222a6054f987b7de5'):
        return result
    sections = [(i, s) for i, s in enumerate(elf.sections) if s['name'] == '.text']
    if len(sections) != 1 or sections[0][1]['type'] != 1:
        raise ValueError('captured common VP requires unique PROGBITS .text')
    index, sec = sections[0]
    functions = {}
    for name, start, size in (('mtk_vcodec_vp_mode_buf_prepare', 0x5af4, 0x3b0),
                              ('mtk_vcodec_alloc_mem', 0x5614, 0x374)):
        matches = [s for s in elf.symbols if s['name'] == name]
        if (len(matches) != 1 or matches[0]['section'] != index or
                matches[0]['type'] != 2 or matches[0]['value'] != start or
                matches[0]['bytes'] != size):
            raise ValueError(f'captured common VP function mismatch: {name}')
        functions[name] = dict(section='.text', offset=start, bytes=size)
    patterns = []
    for name, offset, sample in (('hyfbc_10bit_black_pattern_y', 0x25, 0x10),
                                 ('hyfbc_10bit_black_pattern_c', 0xe5, 0x80)):
        raw, where = elf.named_data(name)
        expected = bytes(16) + bytes([sample]) * 64 + bytes(16) + bytes([sample]) * 64 + bytes(32)
        if where != dict(section='.data', offset=offset, bytes=192) or raw != expected:
            raise ValueError('captured common VP pattern mismatch')
        patterns.append(dict(name=name, evidence=where, raw_hex=raw.hex()))
    spans = (
        ('depth8_or10_only', 'mtk_vcodec_vp_mode_buf_prepare', 0x5b40, 0x5b50),
        ('dma_device_and_mutex', 'mtk_vcodec_vp_mode_buf_prepare', 0x5b50, 0x5b74),
        ('pattern_selection', 'mtk_vcodec_vp_mode_buf_prepare', 0x5bc0, 0x5c48),
        ('length_type2_allocation', 'mtk_vcodec_vp_mode_buf_prepare', 0x5c48, 0x5c80),
        ('vmap_and_cpu_access_begin', 'mtk_vcodec_vp_mode_buf_prepare', 0x5c84, 0x5cb0),
        ('floor_repetitions_no_tail_write', 'mtk_vcodec_vp_mode_buf_prepare', 0x5cb0, 0x5cec),
        ('cpu_access_end_vunmap_next_buffer', 'mtk_vcodec_vp_mode_buf_prepare', 0x5b98, 0x5bc0),
        ('cache_failures_log_then_continue', 'mtk_vcodec_vp_mode_buf_prepare', 0x5cf0, 0x5d40),
        ('prepare_success', 'mtk_vcodec_vp_mode_buf_prepare', 0x5d40, 0x5d54),
        ('allocation_failure_free_three_buffers', 'mtk_vcodec_vp_mode_buf_prepare', 0x5dec, 0x5ea0),
        ('allocator_length_guard', 'mtk_vcodec_alloc_mem', 0x5660, 0x5680),
        ('allocator_records_iova_not_cpu_pointer', 'mtk_vcodec_alloc_mem', 0x57a0, 0x57d8),
    )
    evidence, text = [], elf.data(sec)
    for name, function, start, end in spans:
        anchor = functions[function]
        if not anchor['offset'] <= start < end <= anchor['offset'] + anchor['bytes']:
            raise ValueError('VP evidence outside common function')
        evidence.append(dict(name=name, function=function, section='.text',
            start=start, end_exclusive=end, file_offset=sec['offset'] + start,
            raw_hex=span(text, start, end - start).hex()))
    result.update(profile_match=True, profile='MT6993 captured common VP pattern ABI 1',
        wire_vp_abi=1, file_pattern_and_length_proven=True,
        bitdepths=[8, 10], buffer_count=3, memory_type=2,
        memory_type_is_not_cpu_cache_policy=True,
        allocator_max_buffer_bytes=0x20000000, max_is_not_hardware_ram_capacity=True,
        buffer_bytes_formula='192 * multiplier + 16',
        written_bytes_formula='floor(buffer_bytes / pattern_bytes) * pattern_bytes',
        patterns=patterns, short_patterns=dict(y8='10101010', c8='80808080',
            length8='bf1fbf1f', length10='cf27cf27'),
        multiplier1=dict(buffer_bytes=208, written8=[208, 208, 208],
            written10=[192, 192, 208], untouched10=[16, 16, 0]),
        untouched_tail_contents_proven=False,
        auxiliary_patterns_are_not_decoded_pixels_or_linear_p010=True,
        cache_call_failure_does_not_stop_stock_publication=True,
        function_anchors=functions, instruction_evidence=evidence)
    return result


def module(blob):
    elf = ELF(blob)
    result = dict(build_id=None, modinfo={}, debug_sections=[], symbols={})
    for sec in elf.sections:
        if sec['name'] == '.modinfo':
            for line in elf.data(sec).split(b'\0'):
                if b'=' in line:
                    k, v = line.decode('utf-8').split('=', 1)
                    result['modinfo'].setdefault(k, []).append(v)
        if sec['name'] == '.note.gnu.build-id':
            if result['build_id'] is not None:
                raise ValueError('duplicate module build-id section')
            result['build_id'] = gnu_build_id(elf.data(sec))
        if sec['name'].startswith('.debug') or sec['name'] == '.BTF':
            result['debug_sections'].append(sec['name'])
    targets = ('reset_vcp', 'vcp_A_ready_ipi_handler', 'vcp_reserve_memory_ioremap',
               'mbox_setup_pin_table', 'arm_smmu_of_xlate', 'vdec_vcp_init')
    for sym in elf.symbols:
        if sym['name'] in targets and 0 < sym['section'] < len(elf.sections):
            result['symbols'].setdefault(sym['name'], []).append(dict(
                section=elf.sections[sym['section']]['name'],
                offset=sym['value'], bytes=sym['bytes']))
    if any(s['name'] == 'vcp_reserve_mblock_remap' for s in elf.symbols):
        raw, where = elf.named_data('vcp_reserve_mblock_remap')
        if len(raw) != 40:
            raise ValueError('unsupported rv_v2 remap table')
        result['remap'] = dict(evidence=where, indices=list(struct.unpack('<10I', raw)))
        raw, where = elf.named_data('vcp_reserve_mblock')
        if len(raw) != 384:
            raise ValueError('unsupported rv_v2 mblock table')
        result['mblocks'] = dict(evidence=where, records=[])
        for i in range(12):
            num, dma, cpu, size = struct.unpack_from('<I4xQQI4x', raw, i * 32)
            result['mblocks']['records'].append(dict(index=i, num=num,
                initial_dma_address=dma, initial_cpu_address=cpu, initial_bytes=size))
    result['loaded_pairing_proven'] = False
    if result['modinfo'].get('name') == ['mtk_vcodec_dec']:
        result['decoder_common_evidence'] = decoder_common_evidence(elf, result['build_id'])
        result['decoder_init_evidence'] = decoder_init_evidence(
            elf, result['decoder_common_evidence'])
        result['decoder_decode_evidence'] = decoder_decode_evidence(
            elf, result['decoder_common_evidence'])
        result['decoder_bs_route_evidence'] = decoder_bs_route_evidence(
            elf, result['decoder_common_evidence'])
        result['decoder_vp_evidence'] = decoder_vp_evidence(
            elf, result['decoder_common_evidence'])
        result['decoder_mem_wait_evidence'] = decoder_mem_wait_evidence(
            elf, result['decoder_common_evidence'])
        result['decoder_clock_ring_evidence'] = decoder_clock_ring_evidence(
            elf, result['decoder_common_evidence'])
        result['decoder_output_evidence'] = decoder_output_evidence(
            elf, result['decoder_common_evidence'])
    if result['modinfo'].get('name') == ['mtk_vcodec_common']:
        result['common_vp_evidence'] = common_vp_evidence(elf, result['build_id'])
        result['common_mem_wait_evidence'] = common_mem_wait_evidence(
            elf, result['common_vp_evidence'])
        result['common_irq_owner_evidence'] = common_irq_owner_evidence(
            elf, result['common_vp_evidence'])
    if result['modinfo'].get('name') == ['vcp_v2']:
        result['vcp_bootstrap_evidence'] = vcp_bootstrap_evidence(elf, result['build_id'])
        result['vcp_ready_evidence'] = vcp_ready_evidence(elf, result['vcp_bootstrap_evidence'])
    if result['modinfo'].get('name') == ['arm_smmu_v3']:
        result['smmu_shutdown_evidence'] = smmu_shutdown_evidence(elf, result['build_id'])
        result['smmu_normal_bootstrap_evidence'] = smmu_normal_bootstrap_evidence(
            elf, result['smmu_shutdown_evidence'])
        result['smmu_power_sync_evidence'] = smmu_power_sync_evidence(
            elf, result['smmu_shutdown_evidence'])
    if result['modinfo'].get('name') == ['mtk_iommu_util']:
        result['iommu_util_rpm_evidence'] = iommu_util_rpm_evidence(elf, result['build_id'])
    return result


def cells(blob):
    if len(blob) % 4:
        raise ValueError('FDT cell property not divisible by four')
    return list(struct.unpack(f'>{len(blob) // 4}I', blob))


def fdt(blob):
    # Older vendor DTB has an explicit 64-byte wrapper, not a scan for magic.
    wrapper = 0 if blob[:4] == b'\xd0\x0d\xfe\xed' else 64
    data = blob[wrapper:]
    magic, total, os_, ot, or_, ver, compat, cpu, nt, ns = struct.unpack('>10I', span(data, 0, 40))
    if magic != 0xd00dfeed or ver < 17 or compat > 17 or total < 40:
        raise ValueError('unsupported FDT header')
    data = span(data, 0, total)
    body, strings = span(data, os_, ns), span(data, ot, nt)
    if os_ < 40 or ot < 40 or (os_ < ot + nt and ot < os_ + ns):
        raise ValueError('overlapping FDT blocks')
    nodes, stack, at = {}, [], 0
    while at < len(body):
        token = struct.unpack('>I', span(body, at, 4))[0]
        at += 4
        if token == 1:
            name = cstring(body, at)
            at = (at + len(name.encode()) + 1 + 3) & ~3
            if len(nodes) >= 16384 or len(stack) >= 128 or '/' in name:
                raise ValueError('invalid/oversized FDT tree')
            if not stack and (nodes or name):
                raise ValueError('invalid FDT root')
            path = (stack[-1].rstrip('/') + '/' + name) if stack else '/'
            if path in nodes:
                raise ValueError('duplicate FDT node')
            nodes[path] = {}
            stack.append(path)
        elif token == 2:
            if not stack:
                raise ValueError('unbalanced FDT node')
            stack.pop()
        elif token == 3:
            size, off = struct.unpack('>II', span(body, at, 8))
            at += 8
            if not stack:
                raise ValueError('FDT property outside node')
            name = cstring(strings, off)
            if name in nodes[stack[-1]]:
                raise ValueError('duplicate FDT property')
            nodes[stack[-1]][name] = span(body, at, size)
            at = (at + size + 3) & ~3
        elif token == 4:
            pass
        elif token == 9:
            if stack or not nodes:
                raise ValueError('unbalanced FDT end')
            return nodes, wrapper
        else:
            raise ValueError('unknown FDT token')
    raise ValueError('missing FDT end')


def mailbox(props):
    count = cells(props['mbox-count'])
    if len(count) != 1 or not 1 <= count[0] <= 3:
        raise ValueError('unsupported rv_v2 mailbox count')
    result = dict(model='rv_v2 64-word mailboxes, 64-bit slot alignment', send=[], recv=[])
    for direction, stride in (('send', 3), ('recv', 4)):
        data = cells(props[direction + '-table'])
        if not data or len(data) % stride or len(data) > stride * 64:
            raise ValueError('invalid mailbox table')
        previous, ids = -1, set()
        for at in range(0, len(data), stride):
            channel, box, words = data[at:at + 3]
            if box >= count[0] or box < previous or channel in ids or not 1 <= words <= 64:
                raise ValueError('invalid/unsorted mailbox entry')
            previous = box
            ids.add(channel)
            row = dict(channel=channel, mbox=box, words=words, bytes=words * 4)
            if stride == 4:
                if data[at + 3] not in (0, 1):
                    raise ValueError('unsupported mailbox recv option')
                row['recv_opt'] = data[at + 3]
            result[direction].append(row)
    result['used_words'] = []
    for box in range(count[0]):
        pin = 0
        # The stock ABI shares one address/bit-index space: send, then recv.
        for row in result['send'] + result['recv']:
            if row['mbox'] != box:
                continue
            row.update(pin_index=pin, offset_words=pin * 2, offset_bytes=pin * 8)
            pin += (row['words'] + 1) // 2
        if pin > 32:
            raise ValueError('mailbox table exceeds 64-word SRAM')
        result['used_words'].append(pin * 2)
    return result


def topology(blob):
    nodes, wrapper = fdt(blob)
    handles = {}
    for path, props in nodes.items():
        if 'phandle' in props:
            values = cells(props['phandle'])
            if len(values) != 1 or values[0] in handles:
                raise ValueError('invalid/duplicate FDT phandle')
            handles[values[0]] = path
    selected = {p for p, v in nodes.items() if any(c in v.get('compatible', b'').split(b'\0')
        for c in (b'mediatek,vcp', b'mediatek,mmup', b'mediatek,mt6993-vcodec-dec',
                  b'mediatek,mt6993-mm-smmu')) or p.endswith('/vdec@36000000')
        or 'me_vcp_reserved' in p}
    # Follow only dependencies of these nodes, not unrelated device inventory.
    refs = ('mtk,smmu-shared', 'mtk,iommu-group', 'iommus', 'mtk,trans-granule')
    todo = list(selected)
    while todo:
        path = todo.pop()
        for name in refs:
            value = cells(nodes[path].get(name, b''))
            if value and value[0] in handles and handles[value[0]] not in selected:
                selected.add(handles[value[0]])
                todo.append(handles[value[0]])
    result = dict(wrapper_bytes=wrapper, runtime_mapping_proven=False, nodes=[])
    text_props = ('compatible', 'status', 'reg-names', 'interrupt-names', 'label', 'group-name')
    for path in sorted(selected):
        row = dict(path=path, properties={}, references={})
        props = nodes[path]
        for name, value in props.items():
            if name in text_props:
                row['properties'][name] = value.rstrip(b'\0').decode('utf-8').split('\0')
            elif name in refs or name in ('reg', 'interrupts', 'ssid-enable', 'dma-mask-bit',
                    'dma-range', 'dma-ranges', 'mtk,iommu-dma-range', 'gran4k-only',
                    'mtk,gran4k-only', 'iova-region', 'reserved-region', 'vcp-mem-tbl', 'mbox-count',
                    'vcp-secure-dump', 'vcp-secure-dump-size', 'vcp-secure-dump-offset',
                    'vcp-sram-size', 'vcp-dram-size', 'core-nums',
                    'vp-mode-buf-size-multi'):
                row['properties'][name] = cells(value)
            elif name == 'no-map':
                row['properties'][name] = True
            if name in refs:
                values = cells(value)
                if values:
                    row['references'][name] = handles.get(values[0])
        if 'mtk,iommu-dma-range' in props:
            ranges = cells(props['mtk,iommu-dma-range'])
            if not ranges or len(ranges) % 4:
                raise ValueError('unsupported MTK IOVA window encoding')
            row['iova_windows'] = []
            for at in range(0, len(ranges), 4):
                start = (ranges[at] << 32) | ranges[at + 1]
                size = (ranges[at + 2] << 32) | ranges[at + 3]
                if not size or start + size > 1 << 64:
                    raise ValueError('invalid MTK IOVA window')
                row['iova_windows'].append(dict(start=start, end=start + size))
        if 'iommus' in props:
            values = cells(props['iommus'])
            controller = handles.get(values[0]) if values else None
            if controller and cells(nodes[controller].get('#iommu-cells', b'')) == [1] and len(values) == 2:
                row['sid'] = values[1]
                row['sid_model'] = 'single fwspec ID; not an SSID'
        if 'reg' in props:
            parent = path.rsplit('/', 1)[0] or '/'
            parent_props = nodes.get(parent, {})
            # Do not guess the shape of reg from its total length.
            if (cells(parent_props.get('#address-cells', b'')),
                    cells(parent_props.get('#size-cells', b''))) == ([2], [2]):
                values = cells(props['reg'])
                names = props.get('reg-names', b'').rstrip(b'\0').split(b'\0')
                if len(values) % 4 or (props.get('reg-names') and len(names) != len(values) // 4):
                    raise ValueError('invalid named MMIO ranges')
                row['register_ranges'] = [dict(
                    name=names[i // 4].decode() if props.get('reg-names') else None,
                    start=(values[i] << 32) | values[i + 1],
                    bytes=(values[i + 2] << 32) | values[i + 3])
                    for i in range(0, len(values), 4)]
        if 'send-table' in props and 'recv-table' in props:
            row['mailbox'] = mailbox(props)
        result['nodes'].append(row)
    if not any('mailbox' in r for r in result['nodes']):
        raise ValueError('captured DT has no VCP/MMUP tables')
    return result


def shared_layout(mod, dt):
    """Model offsets only; no physical allocation or SMMU mapping implied."""
    table = next(r['properties']['vcp-mem-tbl'] for r in dt['nodes']
                 if 'vcp-mem-tbl' in r['properties'])
    if len(table) % 2:
        raise ValueError('invalid shared memory size table')
    sizes, records = {}, mod['mblocks']['records']
    for at in range(0, len(table), 2):
        index, size = table[at:at + 2]
        if index >= len(records) or index in sizes:
            raise ValueError('invalid/duplicate shared memory table index')
        sizes[index] = size
    if sizes.get(10, 0):
        raise ValueError('secure dump layout requires separate reserved-memory handling')
    used, out = 0, []
    for index in mod['remap']['indices']:
        if index >= len(records) or any(r['index'] == index for r in out):
            raise ValueError('invalid shared memory remap index')
        size = sizes.get(index, records[index]['initial_bytes'])
        out.append(dict(index=index, num=records[index]['num'], offset=used, bytes=size))
        used += size
    result = dict(blocks=out, bytes=used, offsets_only=True,
                  note='array index, not num, selects size/address in the stock instructions')
    bootstrap = mod.get('vcp_bootstrap_evidence', {})
    if bootstrap.get('profile_match'):
        index = bootstrap['allocation']['logger_record_index']
        logger = next((r for r in out if r['index'] == index), None)
        if logger is None or not logger['bytes']:
            raise ValueError('captured logger record absent from shared layout')
        vcp_offset = logger['offset'] + logger['bytes']
        result['boot_logger'] = dict(record_index=index, mmup_offset=logger['offset'],
            stride_bytes=logger['bytes'], vcp_gpr_offset=vcp_offset,
            bytes_remaining_after_vcp_gpr=used - vcp_offset,
            second_logger_extent_proven=False,
            vcp_gpr_is_not_a_second_allocation=True,
            note='GPR points immediately after the logger record. Remaining bytes belong '
                 'to later mblocks, not automatically to a second logger. Do not invent '
                 'its extent, divide the stride or alias/overwrite the next block.')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lk', type=Path, help='explicit captured MTK LK partition file; <=64 MiB')
    parser.add_argument('--firmware', type=Path)
    parser.add_argument('--module', type=Path, action='append', default=[])
    parser.add_argument('--loaded-module-note', nargs=2, action='append', default=[],
                        metavar=('MODULE_NAME', 'NOTE_PATH'),
                        help='explicit captured /sys/module/<name>/notes/.note.gnu.build-id; '
                             'compares build-id only, never runtime firmware/SHM state')
    parser.add_argument('--dtb', type=Path)
    parser.add_argument('--provenance', required=True, help='capture/OTA origin; not inferred from names')
    parser.add_argument('--out', type=Path, required=True, help='NEW private directory, never git')
    args = parser.parse_args()
    if not (args.lk or args.firmware or args.module or args.dtb) or len(args.module) > 16:
        parser.error('supply explicit artifacts (at most 16 modules)')
    if len(args.loaded_module_note) > 16:
        parser.error('at most 16 captured loaded-module notes')
    report = dict(schema=1, provenance=args.provenance, runtime_ready=False,
                  module_firmware_pairing_proven=False, modules=[])
    fw = None
    if args.lk:
        lk = read_input(args.lk, MAX_LK_INPUT)
        report['lk'] = identity(args.lk, lk) | lk_bootstrap_evidence(lk)
    if args.firmware:
        fw = read_input(args.firmware)
        report['firmware'] = identity(args.firmware, fw) | firmware(fw)
    for path in args.module:
        blob = read_input(path)
        report['modules'].append(identity(path, blob) | module(blob))
    used_names = set()
    for name, path in args.loaded_module_note:
        if (not 0 < len(name) <= 64 or name in used_names or
                any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_'
                    for c in name)):
            raise ValueError('invalid/duplicate captured module name')
        used_names.add(name)
        matches = [m for m in report['modules'] if m['modinfo'].get('name') == [name]]
        if len(matches) != 1:
            raise ValueError('captured note requires one module with matching .modinfo name')
        note = captured_loaded_note(Path(path))
        matches[0]['captured_loaded_build_id'] = note | dict(module_name=name,
            matches_file_build_id=(matches[0]['build_id'] is not None and
                                   note['build_id'] == matches[0]['build_id']))
    if args.dtb:
        blob = read_input(args.dtb)
        report['topology'] = identity(args.dtb, blob) | topology(blob)
    remaps = [m for m in report['modules'] if 'remap' in m]
    if len(remaps) == 1 and 'topology' in report:
        report['shared_layout'] = shared_layout(remaps[0], report['topology'])
    # Parse everything before creating outputs; refuse every existing directory.
    args.out.mkdir(mode=0o700, parents=False, exist_ok=False)
    if fw is not None:
        # Includes signed headers and certificates, so extraction cannot silently
        # turn the original container into a purported unsigned loader input.
        for part in report['firmware']['segments']:
            at = part['offset']
            size = part['header_bytes'] + part['payload_bytes'] + part['padding_bytes']
            with (args.out / part['local']).open('xb') as f:
                f.write(span(fw, at, size))
        with (args.out / 'trailer.bin').open('xb') as f:
            f.write(fw[report['firmware']['tail']['offset']:])
    with (args.out / 'artifacts.json').open('x', encoding='utf-8') as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
        f.write('\n')
    print(json.dumps(dict(output=str(args.out / 'artifacts.json'), runtime_ready=False,
                         modules=len(report['modules'])), ensure_ascii=False))


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, struct.error, UnicodeError, StopIteration) as exc:
        raise SystemExit(f'vcp_artifacts: {exc}') from exc
