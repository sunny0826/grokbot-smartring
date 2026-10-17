import asyncio
import io
import json
from pathlib import Path
import secrets

import httpx2
from mcp import ClientSession
from mcp.client.streamable_http import streamable_http_client
from mcp.types import Implementation
import pytest

from grok_gadgets_gateway.mcp_server import RateLimiter
from grok_gadgets_gateway.protocol import validate_request
from grok_gadgets_gateway.service import serve_gateway
from voice_bridge.enroll import enroll
from voice_bridge.mailbox import CAPABILITIES, MAILBOX_ID, Mailbox, encode
from voice_bridge.store import Store, RequestError
from conftest import DEVICE, RID, TOKEN, private_write, wav


async def call(session, name, args=None):
    result = await session.call_tool(name, args or {})
    assert not result.is_error, result
    return result.structured_content or json.loads(result.content[0].text)


async def command(session, capability, arguments):
    response = await call(session, 'gadgets_command', {
        'device_id': MAILBOX_ID, 'capability': capability, 'arguments': arguments})
    assert response['ok'], response
    receipt = response['command']
    for _ in range(100):
        if receipt['status'] not in ('accepted', 'dispatched'):
            break
        await asyncio.sleep(0.02)
        response = await call(session, 'gadgets_command_status', {'command_id': receipt['command_id']})
        receipt = response['command']
    assert receipt['status'] in ('executed', 'failed'), receipt
    return receipt['reported_state']['result']


def test_existing_mcp_tools_round_trip_and_gateway_restart(tmp_path):
    registry, tokenfile = tmp_path / 'credentials.json', tmp_path / 'mcp-token'
    private_write(registry, {'devices': {DEVICE: {'token': TOKEN}}})
    enroll(registry)
    token = secrets.token_urlsafe(32)
    tokenfile.write_text(token)
    tokenfile.chmod(0o600)
    store = Store(tmp_path / 'data')
    store.put(RID, DEVICE, wav())
    store.update(RID, 'ready', transcript='请回复一句简短中文，证明圆屏语音已经接入现有 MCP。')
    claim = secrets.token_hex(16)
    reply_text = '已收到你的语音。这条回复通过原有 MCP 连接返回圆屏。'

    async def exercise(restarted=False):
        async with serve_gateway(registry, tokenfile, device_port=0, port=0,
                                 log_stream=io.StringIO(),
                                 rate_limiter=RateLimiter(1000, 1000)) as gateway:
            mailbox = Mailbox(store, registry, port=gateway.device_port)
            mailbox.start()
            try:
                for _ in range(100):
                    if mailbox.connected.is_set():
                        break
                    await asyncio.sleep(0.02)
                assert mailbox.connected.is_set()
                async with (
                    httpx2.AsyncClient(headers={'Authorization': 'Bearer ' + token}, trust_env=False) as http,
                    streamable_http_client(gateway.url, http_client=http) as (read, write),
                    ClientSession(read, write, client_info=Implementation(name='voice-test', version='1')) as session,
                ):
                    await session.initialize()
                    names = {t.name for t in (await session.list_tools()).tools}
                    assert names == {'gadgets_list_devices', 'gadgets_get_state', 'gadgets_command',
                                     'gadgets_command_status', 'gadgets_read_events', 'gadgets_diagnostics'}
                    devices = (await call(session, 'gadgets_list_devices'))['devices']
                    mailbox_device = next(d for d in devices if d['device_id'] == MAILBOX_ID)
                    assert mailbox_device['available'] and not mailbox_device['simulated']
                    assert set(mailbox_device['command_capabilities']) == set(CAPABILITIES)
                    result = await command(session, 'voice.claim', {'request_id': RID, 'claim_id': claim})
                    assert result['request_id'] == RID and 'claim_id' not in result
                    current = await call(session, 'gadgets_get_state', {'device_id': MAILBOX_ID})
                    assert claim not in json.dumps(current)
                    assert result['status'] == ('replied' if restarted else 'claimed')
                    result = await command(session, 'voice.reply',
                                           {'request_id': RID, 'claim_id': claim, 'text': reply_text})
                    assert result['ok'] and result['status'] == 'replied'
                    replay = await command(session, 'voice.reply',
                                           {'request_id': RID, 'claim_id': claim, 'text': reply_text})
                    assert replay == result
                    conflict = await command(session, 'voice.reply',
                                             {'request_id': RID, 'claim_id': claim, 'text': '不同的回复'})
                    assert not conflict['ok'] and conflict['error'] == 'reply_conflict'
                    state = await command(session, 'voice.read', {'request_id': RID})
                    assert state['status'] == 'replied'
            finally:
                await asyncio.to_thread(mailbox.stop)

    try:
        asyncio.run(exercise())
        assert store.inbox(DEVICE)['request_id'] == RID
        assert store.page(RID, DEVICE, 0)[:4] == b'VPG1'
        store.close()
        store = Store(tmp_path / 'data')
        asyncio.run(exercise(True))
        assert not store.get(RID, DEVICE)['read']
        store.mark_read(RID, DEVICE)
        assert store.inbox(DEVICE)['status'] == 'empty'
    finally:
        store.close()


def test_full_wire_budgets_and_schema(tmp_path):
    registry = tmp_path / 'credentials.json'
    private_write(registry, {'devices': {MAILBOX_ID: {'token': TOKEN}}})
    store = Store(tmp_path / 'data')
    try:
        for i in range(5):
            rid = f'{i:032x}'
            store.put(rid, DEVICE, wav())
            store.update(rid, 'ready', transcript='😀' * 16000)
        mailbox = Mailbox(store, registry)
        hello = mailbox.hello()
        validate_request(hello)
        assert len(encode(hello)) <= 16384
        ack = mailbox.acknowledgement({'command_id': 'g' * 64, 'capability': 'voice.claim',
                                      'arguments': {'request_id': '0' * 32, 'claim_id': 'f' * 32}})
        validate_request(ack)
        assert len(encode(ack)) <= 16384
        assert len(ack['state']['result']['transcript']) == 800
        frame = {'ok': True, 'commands': [{'command_id': 'g' * 64, 'capability': 'voice.reply',
                  'arguments': {'request_id': '0' * 32, 'claim_id': 'f' * 32, 'text': '😀' * 128}}]}
        assert len(json.dumps(frame).encode()) + 1 <= 2048
    finally:
        store.close()


def test_enrollment_is_explicit_preserves_existing_records_and_rejects_conflicts(tmp_path):
    registry = tmp_path / 'credentials.json'
    original = {'devices': {DEVICE: {'token': TOKEN, 'display_name': 'real ring'}}, 'schema_version': 1}
    private_write(registry, original)
    assert enroll(registry)
    content = json.loads(registry.read_text())
    assert content['devices'][DEVICE] == original['devices'][DEVICE]
    assert content['schema_version'] == 1
    assert not enroll(registry)
    assert (registry.stat().st_mode & 0o777) == 0o600
    assert len(list(tmp_path.glob('*.before-voice-*'))) == 1
    with pytest.raises(ValueError, match='already_in_use'):
        enroll(registry, DEVICE)


def test_failed_claim_is_reported_with_its_request_id(storage, tmp_path):
    mailbox = Mailbox(storage, tmp_path / 'unused')
    ack = mailbox.acknowledgement({'command_id': 'test', 'capability': 'voice.claim',
                                  'arguments': {'request_id': RID, 'claim_id': 'f' * 32}})
    assert ack['status'] == 'failed'
    assert ack['state']['result'] == {'ok': False, 'request_id': RID, 'error': 'not_found'}
