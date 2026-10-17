import asyncio
from contextlib import asynccontextmanager
from dataclasses import replace

import pytest
from websockets.exceptions import ConnectionClosed

from conftest import DEVICE, TOKEN, private_write
from voice_bridge.device_relay import Limits, Relay
from test_device_relay import encode, hello, ws_connect


@asynccontextmanager
async def probe(tmp_path, *, limits=None, response=b'{"ok":true}\n'):
    registry = tmp_path / 'credentials.json'
    private_write(registry, {'devices': {DEVICE: {'token': TOKEN}}})
    frames = []
    accepted = []
    closed = asyncio.Event()
    tasks = set()

    async def backend(reader, writer):
        task = asyncio.current_task()
        tasks.add(task)
        accepted.append(writer)
        try:
            while data := await reader.readline():
                frames.append(data)
                if response is not None:
                    writer.write(response)
                    await writer.drain()
        finally:
            writer.close()
            await writer.wait_closed()
            tasks.discard(task)
            closed.set()

    async with await asyncio.start_server(backend, '127.0.0.1', 0) as upstream:
        relay = Relay(registry, gateway_port=upstream.sockets[0].getsockname()[1], limits=limits)
        async with relay.listen(0) as server:
            port = server.sockets[0].getsockname()[1]
            yield relay, server, f'ws://127.0.0.1:{port}/v1/control', frames, accepted, closed
        for writer in accepted:
            writer.close()
        await asyncio.gather(*tasks, return_exceptions=True)


def test_upgrade_pending_connection_limit_and_handshake_timeout(tmp_path):
    async def exercise():
        limits = replace(Limits(), connections=2, handshake=0.2)
        async with probe(tmp_path, limits=limits) as (relay, server, url, frames, accepted, closed):
            port = server.sockets[0].getsockname()[1]
            reader1, writer1 = await asyncio.open_connection('127.0.0.1', port)
            reader2, writer2 = await asyncio.open_connection('127.0.0.1', port)
            reader3, writer3 = await asyncio.open_connection('127.0.0.1', port)
            try:
                assert await asyncio.wait_for(reader3.read(), 0.1) == b''
                assert len(relay.connections) == 2
                assert not accepted
                assert await asyncio.wait_for(reader1.read(), 1) == b''
                assert await asyncio.wait_for(reader2.read(), 1) == b''
                async with ws_connect(url) as ws:
                    await ws.send(encode(hello()))
                    assert await ws.recv() == b'{"ok":true}\n'
            finally:
                for writer in (writer1, writer2, writer3):
                    writer.close()
                    await writer.wait_closed()
    asyncio.run(exercise())


def test_no_upstream_before_hello_and_bounded_hello_idle_timeouts(tmp_path):
    async def exercise():
        limits = replace(Limits(), hello=0.1, idle=0.15)
        async with probe(tmp_path, limits=limits) as (_, _, url, frames, accepted, closed):
            async with ws_connect(url) as ws:
                with pytest.raises(ConnectionClosed):
                    await asyncio.wait_for(ws.recv(), 1)
                assert not accepted
            async with ws_connect(url) as ws:
                await ws.send(encode(hello()))
                assert await ws.recv() == b'{"ok":true}\n'
                with pytest.raises(ConnectionClosed):
                    await asyncio.wait_for(ws.recv(), 1)
            await asyncio.wait_for(closed.wait(), 1)
            assert len(accepted) == 1 and frames == [encode(hello())]
    asyncio.run(exercise())


def test_rate_limit_and_websocket_close_do_not_poll_or_replay(tmp_path):
    async def exercise():
        limits = replace(Limits(), frames_per_second=2)
        async with probe(tmp_path, limits=limits) as (_, _, url, frames, accepted, closed):
            async with ws_connect(url) as ws:
                for message in [hello(), {'type': 'poll'}]:
                    await ws.send(encode(message))
                    assert await ws.recv() == b'{"ok":true}\n'
                await ws.send(encode({'type': 'poll'}))
                with pytest.raises(ConnectionClosed):
                    await asyncio.wait_for(ws.recv(), 1)
            await asyncio.wait_for(closed.wait(), 1)
            assert frames == [encode(hello()), encode({'type': 'poll'})]
            closed.clear()
            async with ws_connect(url) as ws:
                await ws.send(encode(hello()))
                await ws.recv()
            await asyncio.wait_for(closed.wait(), 1)
            assert len(accepted) == 2
            assert frames == [encode(hello()), encode({'type': 'poll'}), encode(hello())]
    asyncio.run(exercise())


@pytest.mark.parametrize('response', [b'x' * 2048 + b'\n', b'x' * 5000])
def test_oversized_upstream_frame_closes_both_sides(tmp_path, response):
    async def exercise():
        async with probe(tmp_path, response=response) as (_, _, url, _, _, closed):
            async with ws_connect(url) as ws:
                await ws.send(encode(hello()))
                with pytest.raises(ConnectionClosed):
                    await asyncio.wait_for(ws.recv(), 1)
            await asyncio.wait_for(closed.wait(), 1)
    asyncio.run(exercise())


def test_upstream_eof_while_waiting_for_device_closes_websocket(tmp_path):
    async def exercise():
        async with probe(tmp_path) as (_, _, url, _, accepted, closed):
            async with ws_connect(url) as ws:
                await ws.send(encode(hello()))
                await ws.recv()
                accepted[0].close()
                with pytest.raises(ConnectionClosed):
                    await asyncio.wait_for(ws.recv(), 1)
    asyncio.run(exercise())


def test_shutdown_releases_pending_handshakes_and_active_upstream(tmp_path):
    async def exercise():
        async with probe(tmp_path) as (relay, server, url, _, _, closed):
            async with ws_connect(url) as ws:
                await ws.send(encode(hello()))
                await ws.recv()
                reader, writer = await asyncio.open_connection('127.0.0.1', server.sockets[0].getsockname()[1])
                try:
                    server.close()
                    await asyncio.wait_for(server.wait_closed(), 4)
                    assert await reader.read() == b''
                    await asyncio.wait_for(closed.wait(), 1)
                    assert not relay.connections
                finally:
                    writer.close()
                    await writer.wait_closed()
    asyncio.run(exercise())


def test_cli_sigterm_closes_active_connection(tmp_path):
    import re
    import sys

    async def exercise():
        registry = tmp_path / 'credentials.json'
        private_write(registry, {'devices': {DEVICE: {'token': TOKEN}}})
        closed = asyncio.Event()

        async def backend(reader, writer):
            try:
                assert await reader.readline() == encode(hello())
                writer.write(b'{"ok":true}\n')
                await writer.drain()
                assert await reader.read() == b''
            finally:
                writer.close()
                await writer.wait_closed()
                closed.set()

        async with await asyncio.start_server(backend, '127.0.0.1', 0) as upstream:
            port = upstream.sockets[0].getsockname()[1]
            process = await asyncio.create_subprocess_exec(
                sys.executable, '-m', 'voice_bridge.device_relay', '--credentials', str(registry),
                '--port', '0', '--gateway-port', str(port),
                stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
            try:
                line = await asyncio.wait_for(process.stdout.readline(), 3)
                matched = re.search(rb'127\.0\.0\.1:(\d+);', line)
                assert matched, line
                url = f'ws://127.0.0.1:{int(matched[1])}/v1/control'
                async with ws_connect(url) as ws:
                    await ws.send(encode(hello()))
                    assert await ws.recv() == b'{"ok":true}\n'
                    process.terminate()
                    await asyncio.wait_for(ws.wait_closed(), 2)
                    await asyncio.wait_for(closed.wait(), 2)
                _, errors = await asyncio.wait_for(process.communicate(), 3)
                assert process.returncode == 0 and errors == b''
            finally:
                if process.returncode is None:
                    process.kill()
                    await process.wait()
    asyncio.run(exercise())
