import asyncio
from contextlib import asynccontextmanager
import io
import json
import secrets

import httpx2
from mcp import ClientSession
from mcp.client.streamable_http import streamable_http_client
from mcp.types import Implementation
import pytest
from websockets.asyncio.client import connect
from websockets.exceptions import ConnectionClosed, InvalidStatus

from grok_gadgets_gateway.mcp_server import RateLimiter
from grok_gadgets_gateway.service import serve_gateway
from voice_bridge.device_relay import Relay
from conftest import DEVICE, TOKEN, private_write
from test_gateway import call


def encode(message):
    return (json.dumps(message, separators=(',', ':')) + '\n').encode()


def hello(device=DEVICE, token=TOKEN):
    return {'type': 'hello', 'protocol_version': '0.1.0', 'token': token,
            'device': {'device_id': device, 'model': 'relay test', 'firmware_version': '1',
                       'boot_id': 'same-boot', 'simulated': False,
                       'capabilities': ['test.poke', 'state'], 'state': {},
                       'capability_schemas': {'test.poke': {'type': 'object',
                           'properties': {}, 'additionalProperties': False}}}}


def ws_connect(url, headers=None, **kwargs):
    if headers is None:
        headers = [('Authorization', 'Bearer ' + TOKEN), ('X-Device-ID', DEVICE)]
    return connect(url, additional_headers=headers, compression=None, proxy=None,
                   open_timeout=2, close_timeout=0.2, **kwargs)


async def exchange(ws, message):
    await ws.send(encode(message))
    data = await asyncio.wait_for(ws.recv(), 2)
    assert isinstance(data, bytes) and data.count(b'\n') == 1
    return json.loads(data)


@asynccontextmanager
async def gateway_at(tmp_path):
    registry, tokenfile = tmp_path / 'credentials.json', tmp_path / 'mcp-token'
    private_write(registry, {'devices': {DEVICE: {'token': TOKEN}}})
    token = secrets.token_urlsafe(32)
    tokenfile.write_text(token)
    tokenfile.chmod(0o600)
    async with serve_gateway(registry, tokenfile, device_port=0, port=0,
                             log_stream=io.StringIO(),
                             rate_limiter=RateLimiter(1000, 1000)) as gateway:
        yield registry, token, gateway


@asynccontextmanager
async def mcp_at(token, gateway):
    async with (
        httpx2.AsyncClient(headers={'Authorization': 'Bearer ' + token}, trust_env=False) as http,
        streamable_http_client(gateway.url, http_client=http) as (read, write),
        ClientSession(read, write, client_info=Implementation(name='relay-test', version='1')) as session,
    ):
        await session.initialize()
        yield session


async def available(session, expected):
    for _ in range(100):
        result = await call(session, 'gadgets_list_devices')
        device = next((d for d in result['devices'] if d['device_id'] == DEVICE), None)
        if device and device['available'] is expected:
            return device
        await asyncio.sleep(0.01)
    raise AssertionError(result)


def test_real_mcp_command_ack_disconnect_and_relay_restart(tmp_path):
    async def exercise():
        async with gateway_at(tmp_path) as (registry, token, gateway), mcp_at(token, gateway) as session:
            sessions = set()
            for _ in range(2):
                relay = Relay(registry, gateway_port=gateway.device_port)
                async with relay.listen(0) as server:
                    url = f'ws://127.0.0.1:{server.sockets[0].getsockname()[1]}/v1/control'
                    async with ws_connect(url) as ws:
                        response = await exchange(ws, hello())
                        assert response['ok']
                        sessions.add(response['session_id'])
                        device = await available(session, True)
                        assert not device['simulated']
                        result = await call(session, 'gadgets_command', {
                            'device_id': DEVICE, 'capability': 'test.poke', 'arguments': {}})
                        assert result['ok'], result
                        cid = result['command']['command_id']
                        response = await exchange(ws, {'type': 'poll'})
                        assert response['commands'][0]['command_id'] == cid
                        assert (await exchange(ws, {'type': 'ack', 'command_id': cid,
                            'status': 'executed', 'state': {'poke_count': 1}}))['ok']
                        result = await call(session, 'gadgets_command_status', {'command_id': cid})
                        assert result['command']['status'] == 'executed'
                        assert (await exchange(ws, {'type': 'poll'}))['commands'] == []
                        pending = await call(session, 'gadgets_command', {
                            'device_id': DEVICE, 'capability': 'test.poke', 'arguments': {}})
                        lost_id = pending['command']['command_id']
                        assert (await exchange(ws, {'type': 'poll'}))['commands'][0]['command_id'] == lost_id
                        ws.transport.abort()
                    await available(session, False)
                    lost = await call(session, 'gadgets_command_status', {'command_id': lost_id})
                    assert lost['command']['status'] == 'unconfirmed'
                    async with ws_connect(url) as ws:
                        assert (await exchange(ws, hello()))['ok']
                        assert (await exchange(ws, {'type': 'poll'}))['commands'] == []
                        server.close()
                        await asyncio.wait_for(ws.wait_closed(), 2)
                    await available(session, False)
                assert not relay.connections
            assert len(sessions) == 2
    asyncio.run(exercise())


def test_real_gateway_stopping_closes_websocket_and_can_restart(tmp_path):
    async def exercise():
        sessions = set()
        for _ in range(2):
            async with gateway_at(tmp_path) as (registry, token, gateway):
                relay = Relay(registry, gateway_port=gateway.device_port)
                listener = relay.listen(0)
                server = await listener.__aenter__()
                url = f'ws://127.0.0.1:{server.sockets[0].getsockname()[1]}/v1/control'
                ws = await ws_connect(url)
                sessions.add((await exchange(ws, hello()))['session_id'])
            try:
                await asyncio.wait_for(ws.wait_closed(), 2)
            finally:
                await ws.close()
                await listener.__aexit__(None, None, None)
        assert len(sessions) == 2
    asyncio.run(exercise())


def test_authentication_and_wire_rejections_against_real_gateway(tmp_path, caplog):
    async def exercise():
        async with gateway_at(tmp_path) as (registry, token, gateway), mcp_at(token, gateway) as session:
            relay = Relay(registry, gateway_port=gateway.device_port)
            async with relay.listen(0) as server:
                url = f'ws://127.0.0.1:{server.sockets[0].getsockname()[1]}/v1/control'
                headers = [('Authorization', 'Bearer ' + TOKEN), ('X-Device-ID', DEVICE)]
                for bad in [[], headers[:1], headers + headers[:1], headers + headers[1:],
                            [('Authorization', 'Bearer invalid'), ('X-Device-ID', DEVICE)]]:
                    with pytest.raises(InvalidStatus) as caught:
                        async with ws_connect(url, bad):
                            pass
                    assert caught.value.response.status_code == 401
                    assert caught.value.response.headers['Cache-Control'] == 'no-store'
                with pytest.raises(InvalidStatus) as caught:
                    async with ws_connect(url + '?token=forbidden'):
                        pass
                assert caught.value.response.status_code == 404
                private_write(registry, {'devices': {DEVICE: {'token': TOKEN, 'revoked': True}}})
                with pytest.raises(InvalidStatus) as caught:
                    async with ws_connect(url):
                        pass
                assert caught.value.response.status_code == 401
                private_write(registry, {'devices': {DEVICE: {'token': TOKEN}}})
                invalid = [encode(hello(device='other')), encode(hello(token='x' * 32)),
                           encode({'type': 'poll'}), encode(hello()) * 2, b'x' * 16385,
                           encode(hello()).decode(), b'{"type":"hello","type":"hello"}\n',
                           b'{}', b'[]\n']
                for payload in invalid:
                    async with ws_connect(url) as ws:
                        await ws.send(payload)
                        with pytest.raises(ConnectionClosed):
                            await asyncio.wait_for(ws.recv(), 2)
                assert (await call(session, 'gadgets_list_devices'))['devices'] == []
                async with ws_connect(url) as ws:
                    # Fragmented binary messages still represent exactly one LF-delimited frame.
                    data = encode(hello())
                    await ws.send([data[:25], data[25:]])
                    assert json.loads(await ws.recv())['ok']
                    await ws.send(data)
                    with pytest.raises(ConnectionClosed):
                        await asyncio.wait_for(ws.recv(), 2)
                await available(session, False)
                async with ws_connect(url) as ws:
                    assert (await exchange(ws, hello()))['ok']
                    private_write(registry, {'devices': {DEVICE: {'token': TOKEN, 'revoked': True}}})
                    response = await exchange(ws, {'type': 'poll'})
                    assert not response['ok'] and response['error']['code'] == 'revoked'
                    await asyncio.wait_for(ws.wait_closed(), 2)
                await available(session, False)
    caplog.set_level('DEBUG')
    asyncio.run(exercise())
    # The relay's library logger is isolated even when the application enables DEBUG.
    assert all(record.name != 'voice_device_relay.wire' for record in caplog.records)
