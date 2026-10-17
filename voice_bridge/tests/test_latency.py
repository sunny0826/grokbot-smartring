from concurrent.futures import ThreadPoolExecutor
import hashlib
import http.client
import json
import socket
import struct
import threading
import time
import zlib

import pytest

from conftest import DEVICE, RID, TOKEN, private_write, wav
from test_bridge import request
from voice_bridge.store import RequestError, Store
from voice_bridge.trigger import TriggerConfig, TriggerSender


def test_response_endpoint_baseline(receiver, storage):
    storage.put(RID, DEVICE, wav())
    assert request(receiver, 'GET', suffix='/response')[0] == 200


def test_explicit_keep_alive_baseline(receiver, storage):
    storage.put(RID, DEVICE, wav())
    server, context, _ = receiver
    conn = http.client.HTTPSConnection('localhost', server.server_port, context=context)
    try:
        conn.request('GET', '/v1/recordings/' + RID, headers={
            'Authorization': 'Bearer ' + TOKEN, 'X-Device-ID': DEVICE, 'Connection': 'keep-alive'})
        response = conn.getresponse()
        response.read()
        assert response.getheader('Connection') == 'keep-alive'
    finally:
        conn.close()


def test_transcript_experiment_baseline(storage, trigger_endpoint):
    storage.put(RID, DEVICE, wav())
    storage.update(RID, 'ready', transcript='合成短句')
    url, requests, _, context = trigger_endpoint
    sender = TriggerSender(TriggerConfig(url, 'synthetic-test-key'), 'test-mailbox',
                           context=context, transcript_store=storage)
    assert sender(RID).state == 'accepted'
    assert requests[0][2]['transcript'] == '合成短句'

def connection(receiver):
    server, context, _ = receiver
    return http.client.HTTPSConnection('localhost', server.server_port, context=context, timeout=3)


def fetch(conn, suffix='', *, device=DEVICE, token=TOKEN, after=None, keep=True):
    fields = {'Authorization': 'Bearer ' + token, 'X-Device-ID': device}
    if keep:
        fields['Connection'] = 'keep-alive'
    if after is not None:
        fields['X-Voice-After'] = after
    conn.request('GET', '/v1/recordings/' + RID + suffix, headers=fields)
    response = conn.getresponse()
    return response.status, dict(response.getheaders()), response.read()


def waiting(storage, count=1):
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        with storage.lock:
            if len(storage.response_waiters) == count:
                return
        time.sleep(0.005)
    pytest.fail('response waiter did not register')


def test_response_timeout_and_state_changed_before_wait(receiver, storage):
    storage.put(RID, DEVICE, wav())
    conn = connection(receiver)
    try:
        started = time.monotonic()
        status, _, data = fetch(conn, '/response', after='stored')
        assert status == 200 and json.loads(data)['status'] == 'stored'
        assert 0.9 <= time.monotonic() - started < 2
        storage.claim()
        started = time.monotonic()
        assert json.loads(fetch(conn, '/response', after='stored')[2])['status'] == 'transcribing'
        assert time.monotonic() - started < 0.5
    finally:
        conn.close()


def test_response_wakes_after_commit_without_holding_store_lock(receiver, storage):
    storage.put(RID, DEVICE, wav())
    conn = connection(receiver)
    try:
        with ThreadPoolExecutor(1) as pool:
            future = pool.submit(fetch, conn, '/response', after='stored')
            waiting(storage)
            started = time.monotonic()
            storage.claim()
            assert json.loads(future.result(timeout=0.7)[2])['status'] == 'transcribing'
            assert time.monotonic() - started < 0.7
    finally:
        conn.close()


def test_response_same_device_wait_limit_is_immediate(receiver, storage):
    storage.put(RID, DEVICE, wav())
    first, second = connection(receiver), connection(receiver)
    try:
        with ThreadPoolExecutor(1) as pool:
            future = pool.submit(fetch, first, '/response', after='stored')
            waiting(storage)
            started = time.monotonic()
            assert json.loads(fetch(second, '/response', after='stored')[2])['status'] == 'stored'
            assert time.monotonic() - started < 0.5
            storage.claim()
            future.result(timeout=0.7)
    finally:
        first.close()
        second.close()


def test_response_global_wait_limit(storage):
    for i in range(5):
        storage.put(f'{i:032x}', f'ring-{i}', wav())
    with ThreadPoolExecutor(4) as pool:
        futures = [pool.submit(storage.response, f'{i:032x}', f'ring-{i}', 'stored') for i in range(4)]
        waiting(storage, 4)
        started = time.monotonic()
        assert storage.response(f'{4:032x}', 'ring-4', 'stored')[0]['status'] == 'stored'
        assert time.monotonic() - started < 0.1
        storage.stop_responses()
        for future in futures:
            with pytest.raises(RequestError, match='service_unavailable'):
                future.result(timeout=0.5)


@pytest.mark.parametrize('after', ['', 'replied', 'failed', 'Stored', 'ready,claimed'])
def test_response_invalid_after(receiver, storage, after):
    storage.put(RID, DEVICE, wav())
    assert request(receiver, 'GET', suffix='/response', headers={'X-Voice-After': after})[0] == 400


def test_response_duplicate_header_auth_and_ownership(receiver, storage):
    storage.put(RID, DEVICE, wav())
    conn = connection(receiver)
    try:
        conn.putrequest('GET', '/v1/recordings/' + RID + '/response')
        for name, value in [('Authorization', 'Bearer ' + TOKEN), ('X-Device-ID', DEVICE),
                            ('X-Voice-After', 'stored'), ('X-Voice-After', 'stored')]:
            conn.putheader(name, value)
        conn.endheaders()
        response = conn.getresponse()
        response.read()
        assert response.status == 400 and response.getheader('Connection') == 'close'
    finally:
        conn.close()
    assert request(receiver, 'GET', suffix='/response', headers={'Authorization': 'Bearer bad'})[0] == 401
    assert request(receiver, 'GET', suffix='/response', headers={
        'Authorization': 'Bearer ' + TOKEN + 'other', 'X-Device-ID': 'other-ring'})[0] == 404


def reply(storage):
    storage.put(RID, DEVICE, wav())
    storage.update(RID, 'ready', transcript='合成输入')
    storage.claim_request(RID, 'f' * 32)
    return storage.reply_request(RID, 'f' * 32, '合成回复')


def test_response_page_crc_read_snapshot_and_lost_reply_ack(receiver, storage):
    reply(storage)  # Discard the first reply acknowledgement, as after a connection loss.
    assert storage.reply_request(RID, 'f' * 32, '合成回复')['status'] == 'replied'
    conn = connection(receiver)
    try:
        status, headers, data = fetch(conn, '/response', after='claimed')
        assert status == 200 and headers['X-Voice-Read'] == '0'
        assert headers['X-Voice-Request-ID'] == RID
        magic, width, height, index, count, crc = struct.unpack('<4sHHHHI', data[:16])
        assert (magic, width, height, index, count) == (b'VPG1', 272, 168, 0, 1)
        assert zlib.crc32(data[16:]) == crc
        assert data == storage.page(RID, DEVICE, 0)
        assert not storage.get(RID, DEVICE)['read']
        storage.mark_read(RID, DEVICE)
        _, headers, same = fetch(conn, '/response')
        assert same == data and headers['X-Voice-Read'] == '1'
        _, headers, _ = fetch(conn)
        assert 'X-Voice-Read' not in headers and 'X-Voice-Request-ID' not in headers
        with pytest.raises(RequestError, match='reply_conflict'):
            storage.reply_request(RID, 'f' * 32, '不同回复')
    finally:
        conn.close()


def test_response_read_snapshot_holds_lock_through_page(storage, monkeypatch):
    reply(storage)
    entered = threading.Event()
    original = storage.page
    def page(*args):
        entered.set()
        time.sleep(0.05)
        return original(*args)
    monkeypatch.setattr(storage, 'page', page)
    with ThreadPoolExecutor(2) as pool:
        response = pool.submit(storage.response, RID, DEVICE)
        assert entered.wait(1)
        read = pool.submit(storage.mark_read, RID, DEVICE)
        assert response.result(timeout=1)[1]['X-Voice-Read'] == '0'
        assert read.result(timeout=1)['read']


def test_persistent_second_request_reauthenticates(receiver, storage):
    storage.put(RID, DEVICE, wav())
    conn = connection(receiver)
    try:
        assert fetch(conn)[1]['Connection'] == 'keep-alive'
        original = conn.sock
        private_write(receiver[2], {'devices': {DEVICE: {'token': TOKEN, 'revoked': True}}})
        status, headers, _ = fetch(conn)
        assert status == 401 and headers['Connection'] == 'close'
        assert original is not None and conn.sock is None
    finally:
        conn.close()


def test_connection_reuse_requires_explicit_header_and_error_closes(receiver, storage):
    storage.put(RID, DEVICE, wav())
    conn = connection(receiver)
    try:
        assert fetch(conn, keep=False)[1]['Connection'] == 'close'
        assert fetch(conn)[1]['Connection'] == 'keep-alive'
        original = conn.sock
        assert fetch(conn)[1]['Connection'] == 'keep-alive'
        assert conn.sock is original
        assert fetch(conn, '/response', after='bad')[1]['Connection'] == 'close'
        assert conn.sock is None
    finally:
        conn.close()


def test_persistent_per_device_limit_and_release(receiver, storage):
    storage.put(RID, DEVICE, wav())
    conns = [connection(receiver) for _ in range(3)]
    try:
        assert [fetch(c)[1]['Connection'] for c in conns] == ['keep-alive', 'keep-alive', 'close']
        conns[0].close()
        deadline = time.monotonic() + 1
        while time.monotonic() < deadline:
            with receiver[0].connections.lock:
                if sum(s.device is not None for s in receiver[0].connections.sockets.values()) == 1:
                    break
            time.sleep(.005)
        assert fetch(conns[2])[1]['Connection'] == 'keep-alive'
    finally:
        for conn in conns:
            conn.close()


def test_persistent_global_limit_leaves_short_request_capacity(receiver, storage):
    server, _, credentials = receiver
    devices = {f'ring-{i}': {'token': TOKEN} for i in range(5)}
    private_write(credentials, {'devices': devices})
    for i in range(5):
        storage.put(f'{i:032x}', f'ring-{i}', wav())
    conns = [connection(receiver) for _ in range(9)]
    try:
        results = []
        for i, conn in enumerate(conns):
            device = f'ring-{i // 2}'
            conn.request('GET', f'/v1/recordings/{i // 2:032x}', headers={
                'Authorization': 'Bearer ' + TOKEN, 'X-Device-ID': device, 'Connection': 'keep-alive'})
            response = conn.getresponse()
            response.read()
            assert response.status == 200
            results.append(response.getheader('Connection'))
        assert results == ['keep-alive'] * 8 + ['close']
    finally:
        for conn in conns:
            conn.close()


@pytest.mark.parametrize('limit', ['idle', 'lifetime', 'requests'])
def test_persistent_connection_bounds(receiver, storage, limit):
    storage.put(RID, DEVICE, wav())
    pool = receiver[0].connections
    pool.idle_seconds = .1 if limit == 'idle' else 3
    pool.lifetime_seconds = .1 if limit == 'lifetime' else 30
    pool.max_requests = 2 if limit == 'requests' else 64
    conn = connection(receiver)
    try:
        assert fetch(conn)[1]['Connection'] == 'keep-alive'
        if limit == 'requests':
            assert fetch(conn)[1]['Connection'] == 'close'
        else:
            time.sleep(.2)
            assert conn.sock.recv(1) == b''
    finally:
        conn.close()


def test_shutdown_wakes_waiter_and_closes_idle_tls_and_raw_socket(receiver, storage):
    storage.put(RID, DEVICE, wav())
    idle, waiter = connection(receiver), connection(receiver)
    raw = socket.create_connection(('localhost', receiver[0].server_port), timeout=2)
    try:
        assert fetch(idle)[0] == 200
        with ThreadPoolExecutor(1) as pool:
            future = pool.submit(fetch, waiter, '/response', after='stored')
            waiting(storage)
            started = time.monotonic()
            receiver[0].server_close()
            assert time.monotonic() - started < 1
            status, headers, _ = future.result(timeout=1)
            assert status == 503 and headers['Connection'] == 'close'
        assert idle.sock.recv(1) == b''
        assert raw.recv(1) == b''
    finally:
        idle.close()
        waiter.close()
        raw.close()


def test_default_payload_unchanged_and_long_transcript_falls_back(storage, trigger_endpoint):
    url, requests, _, context = trigger_endpoint
    storage.put(RID, DEVICE, wav())
    storage.update(RID, 'ready', transcript='合成输入')
    config = TriggerConfig(url, 'synthetic-test-key')
    TriggerSender(config, 'test-mailbox', context=context)(RID)
    expected = {'event': 'voice.ready', 'version': 1, 'request_id': RID, 'mailbox_id': 'test-mailbox'}
    assert requests[-1][2] == expected
    storage.put('b' * 32, DEVICE, wav())
    storage.update('b' * 32, 'ready', transcript='字' * 801)
    TriggerSender(config, 'test-mailbox', context=context, transcript_store=storage)('b' * 32)
    assert requests[-1][2] == {**expected, 'request_id': 'b' * 32}


def test_transcript_metadata_is_durable_immutable_and_matches_claim(tmp_path, trigger_endpoint):
    directory = tmp_path / 'data'
    store = Store(directory)
    text = '😀' * 800
    store.put(RID, DEVICE, wav())
    store.update(RID, 'ready', transcript=text)
    metadata = store.trigger_transcript(RID)
    assert metadata == {'transcript': text, 'transcript_version': 1,
                        'transcript_sha256': hashlib.sha256(text.encode()).hexdigest()}
    with pytest.raises(RequestError, match='transcript_conflict'):
        store.update(RID, 'ready', transcript='替换')
    claim = store.claim_request(RID, 'c' * 32)
    store.close()
    store = Store(directory)
    try:
        duplicate = store.claim_request(RID, 'c' * 32)
        for field in metadata:
            assert duplicate[field] == claim[field] == metadata[field]
        url, requests, _, context = trigger_endpoint
        sender = TriggerSender(TriggerConfig(url, 'synthetic-test-key'), 'test-mailbox',
                               context=context, transcript_store=store)
        sender(RID)
        sender(RID)
        assert requests[-1][2] == requests[-2][2]
        assert 'claim_id' not in requests[-1][2]
    finally:
        store.close()


def test_cli_transcript_experiment_requires_trigger(monkeypatch):
    from voice_bridge.cli import main
    monkeypatch.setattr('sys.argv', ['voice-bridge', '--grok-trigger-transcript'])
    with pytest.raises(SystemExit) as error:
        main()
    assert error.value.code == 2


def test_store_close_wakes_wait_without_accessing_closed_database(tmp_path):
    store = Store(tmp_path / 'data')
    store.put(RID, DEVICE, wav())
    with ThreadPoolExecutor(1) as pool:
        future = pool.submit(store.response, RID, DEVICE, 'stored')
        waiting(store)
        store.close()
        with pytest.raises(RequestError, match='service_unavailable'):
            future.result(timeout=.5)


def test_legacy_transcript_and_claim_migrate_without_changing_receipt(tmp_path):
    store = Store(tmp_path / 'data')
    store.put(RID, DEVICE, wav())
    store.update(RID, 'ready', transcript='旧合成转写')
    claim = store.claim_request(RID, 'c' * 32)
    claim.pop('transcript_sha256')
    claim.pop('transcript_version')
    with store.db:
        store.db.execute('UPDATE jobs SET claim_result=?', (json.dumps(claim),))
        store.db.execute('ALTER TABLE jobs DROP COLUMN transcript_sha256')
        store.db.execute('ALTER TABLE jobs DROP COLUMN transcript_version')
    store.close()
    store = Store(tmp_path / 'data')
    try:
        duplicate = store.claim_request(RID, 'c' * 32)
        assert duplicate['transcript_version'] == 1
        assert duplicate['transcript_sha256'] == hashlib.sha256('旧合成转写'.encode()).hexdigest()
        assert duplicate['transcript'] == claim['transcript']
        assert duplicate['duplicate'] is True
        assert store.trigger_transcript(RID)['transcript_sha256'] == duplicate['transcript_sha256']
    finally:
        store.close()


def test_keep_alive_does_not_reuse_previous_exclusion_or_auth_headers(receiver, storage):
    from test_inbox_exclusions import replied
    replied(storage, RID)
    conn = connection(receiver)
    fields = {'Authorization': 'Bearer ' + TOKEN, 'X-Device-ID': DEVICE, 'Connection': 'keep-alive'}
    try:
        conn.request('GET', '/v1/inbox', headers={**fields, 'X-Voice-Read-Exclude': RID})
        response = conn.getresponse()
        assert json.loads(response.read())['status'] == 'empty'
        original = conn.sock
        conn.request('GET', '/v1/inbox', headers=fields)
        response = conn.getresponse()
        assert json.loads(response.read())['request_id'] == RID
        assert conn.sock is original
        conn.request('GET', '/v1/inbox', headers={'Connection': 'keep-alive'})
        response = conn.getresponse()
        response.read()
        assert response.status == 401 and response.getheader('Connection') == 'close'
    finally:
        conn.close()


def test_rollback_does_not_publish_uncommitted_status(storage):
    storage.put(RID, DEVICE, wav())
    with ThreadPoolExecutor(1) as pool:
        future = pool.submit(storage.response, RID, DEVICE, 'stored')
        waiting(storage)
        with pytest.raises(ValueError):
            with storage.transaction():
                storage.db.execute("UPDATE jobs SET status='ready' WHERE request_id=?", (RID,))
                raise ValueError('rollback synthetic write')
        storage.claim()
        assert future.result(timeout=.7)[0]['status'] == 'transcribing'


def test_successful_upload_can_reuse_connection_only_after_complete_body(receiver, storage):
    conn = connection(receiver)
    try:
        conn.request('PUT', '/v1/recordings/' + RID, wav(), {
            'Authorization': 'Bearer ' + TOKEN, 'X-Device-ID': DEVICE,
            'Content-Type': 'audio/wav', 'Connection': 'keep-alive'})
        response = conn.getresponse()
        response.read()
        assert response.status == 201 and response.getheader('Connection') == 'keep-alive'
        original = conn.sock
        assert fetch(conn)[0] == 200 and conn.sock is original
        conn.request('GET', '/v1/recordings/' + RID, b'unexpected body', {
            'Authorization': 'Bearer ' + TOKEN, 'X-Device-ID': DEVICE, 'Connection': 'keep-alive'})
        response = conn.getresponse()
        response.read()
        assert response.status == 400 and response.getheader('Connection') == 'close'
    finally:
        conn.close()


def test_waiting_claimed_response_returns_committed_first_page(receiver, storage):
    storage.put(RID, DEVICE, wav())
    storage.update(RID, 'ready', transcript='等待期间生成的回复')
    storage.claim_request(RID, 'f' * 32)
    conn = connection(receiver)
    try:
        with ThreadPoolExecutor(1) as pool:
            future = pool.submit(fetch, conn, '/response', after='claimed')
            waiting(storage)
            storage.reply_request(RID, 'f' * 32, '等待完成')
            status, headers, page = future.result(timeout=.8)
            assert status == 200 and headers['Content-Type'] == 'application/octet-stream'
            assert headers['X-Voice-Request-ID'] == RID and headers['X-Voice-Read'] == '0'
            assert page == storage.page(RID, DEVICE, 0)
            assert not storage.get(RID, DEVICE)['read']
    finally:
        conn.close()
