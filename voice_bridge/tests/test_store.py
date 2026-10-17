from concurrent.futures import ThreadPoolExecutor
import json
import sqlite3
import threading

import pytest

from conftest import DEVICE, RID, TOKEN, wav
from test_bridge import request
from voice_bridge.assets import font_path
from voice_bridge.render import RenderPages
from voice_bridge.store import RequestError, Store
from voice_bridge.worker import Worker

CLAIM = 'a' * 32


def ready(store, request_id=RID, text='请打开书桌灯'):
    store.put(request_id, DEVICE, wav())
    Worker(store, lambda _: text).process_one()


def test_transcript_waits_for_claim(storage):
    ready(storage)
    assert storage.get(RID, DEVICE)['status'] == 'ready'
    assert not Worker(storage, lambda _: pytest.fail('already transcribed')).process_one()
    state = storage.mailbox_state()
    assert state['pending_count'] == 1 and state['unread_count'] == 0
    assert state['requests'][0]['preview'] == '请打开书桌灯'
    assert 'claim_id' not in json.dumps(state)
    with pytest.raises(RequestError, match='retry_not_allowed'):
        storage.retry(RID, DEVICE)


def test_local_transcription_failure_retry(receiver, storage):
    request(receiver)
    def fail(audio):
        raise RuntimeError('private details')
    worker = Worker(storage, fail)
    worker.process_one()
    assert request(receiver, 'GET')[1]['error'] == 'transcription_failed'
    assert request(receiver, 'POST', suffix='/retry')[0] == 200
    assert request(receiver, 'POST', suffix='/retry')[0] == 409
    worker.transcribe = lambda _: '转写完成'
    worker.process_one()
    assert request(receiver, 'GET')[1]['status'] == 'ready'


def test_paged_transcript_and_no_claim_leak(storage):
    text = '𠮷' * 1601
    ready(storage, text=text)
    first = storage.claim_request(RID, CLAIM)
    assert first['transcript'] == text[:800] and first['next_offset'] == 800
    assert first['claim_id'] == CLAIM and first['duplicate'] is False
    middle = storage.read_request(RID, 800)
    assert middle['transcript'] == text[800:1600] and middle['next_offset'] == 1600
    assert 'claim_id' not in middle and CLAIM not in json.dumps(storage.mailbox_state())
    assert storage.read_request(RID, 1600)['transcript'] == '𠮷'
    assert storage.read_request(RID, 1601)['next_offset'] is None
    for offset in (-1, True, '1', 1602):
        with pytest.raises(RequestError, match='invalid_offset'):
            storage.read_request(RID, offset)


def test_claim_race_and_cross_request_token(storage):
    ready(storage)
    def claim(i):
        try:
            return storage.claim_request(RID, f'{i:032x}')
        except RequestError:
            return None
    with ThreadPoolExecutor(max_workers=6) as executor:
        results = list(executor.map(claim, range(6)))
    winners = [r for r in results if r]
    assert len(winners) == 1
    winner = winners[0]
    assert storage.claim_request(RID, winner['claim_id']) == {**winner, 'duplicate': True}
    ready(storage, 'f' * 32)
    with pytest.raises(RequestError, match='claim_id_reused'):
        storage.claim_request('f' * 32, winner['claim_id'])


def test_reply_race_restart_ack_loss_and_immutable_pages(tmp_path):
    directory = tmp_path / 'durable'
    store = Store(directory)
    ready(store)
    claim = store.claim_request(RID, CLAIM)
    with ThreadPoolExecutor(max_workers=6) as executor:
        receipts = list(executor.map(lambda _: store.reply_request(RID, CLAIM, '灯已打开。'), range(6)))
    assert all(r == receipts[0] for r in receipts)
    first = store.page(RID, DEVICE, 0)
    assert store.db.execute('SELECT COUNT(*) FROM pages').fetchone()[0] == receipts[0]['page_count']
    store.close()
    store = Store(directory, renderer=lambda _: pytest.fail('retry must not rerender'))
    try:
        replay = store.claim_request(RID, CLAIM)
        assert replay['duplicate'] and replay['status'] == 'replied'
        assert replay['reply'] == '灯已打开。' and replay['claim_id'] == CLAIM
        assert replay['transcript'] == claim['transcript']
        assert store.read_request(RID)['reply'] == '灯已打开。'
        assert store.reply_request(RID, CLAIM, '灯已打开。') == receipts[0]
        assert store.page(RID, DEVICE, 0) == first
        assert store.mailbox_state()['unread_count'] == 1
        assert not store.get(RID, DEVICE)['read']
        store.mark_read(RID, DEVICE)
        assert store.mark_read(RID, DEVICE)['read']
        assert store.read_request(RID)['read']
        assert store.inbox(DEVICE)['status'] == 'empty'
        with pytest.raises(RequestError, match='reply_conflict'):
            store.reply_request(RID, CLAIM, '另一个回复')
        with pytest.raises(RequestError, match='claim_conflict'):
            store.reply_request(RID, 'b' * 32, '灯已打开。')
    finally:
        store.close()


def test_reply_failure_is_atomic_and_retryable(storage):
    ready(storage)
    storage.claim_request(RID, CLAIM)
    storage.renderer = lambda _: (_ for _ in ()).throw(OSError('broken font'))
    with pytest.raises(RequestError, match='render_unavailable'):
        storage.reply_request(RID, CLAIM, '回复')
    assert storage.get(RID, DEVICE)['status'] == 'claimed'
    assert storage.db.execute('SELECT COUNT(*) FROM pages').fetchone()[0] == 0
    storage.renderer = RenderPages(font_path())
    assert storage.reply_request(RID, CLAIM, '回复')['status'] == 'replied'


@pytest.mark.parametrize('text', ['', '   ', 'a' * 129, '\x00', '\ud800', '\t', 1, None])
def test_bad_reply_text(storage, text):
    ready(storage)
    storage.claim_request(RID, CLAIM)
    with pytest.raises(RequestError, match='invalid_reply_text'):
        storage.reply_request(RID, CLAIM, text)
    assert storage.get(RID, DEVICE)['status'] == 'claimed'


def test_tls_inbox_page_and_explicit_read_ownership(receiver, storage):
    assert request(receiver, 'GET', path='/v1/inbox')[1]['status'] == 'empty'
    assert request(receiver)[0] == 201
    Worker(storage, lambda _: '模拟任务：打开书桌灯').process_one()
    storage.claim_request(RID, CLAIM)
    receipt = storage.reply_request(RID, CLAIM, '已打开书桌灯。')
    assert request(receiver, 'GET')[1]['page_count'] == receipt['page_count']
    assert request(receiver, 'GET', path='/v1/inbox')[1]['request_id'] == RID
    status, page = request(receiver, 'GET', suffix='/pages/0')
    assert status == 200 and len(page) == 91392 + 16
    assert not request(receiver, 'GET')[1]['read']
    other = {'X-Device-ID': 'other-ring', 'Authorization': 'Bearer ' + TOKEN + 'other'}
    for method, suffix in [('GET', '/pages/0'), ('POST', '/read')]:
        assert request(receiver, method, suffix=suffix, headers=other)[0] == 404
    assert request(receiver, 'GET', path='/v1/inbox', headers=other)[1]['status'] == 'empty'
    for index in ('-1', '8', '999999', 'x'):
        assert request(receiver, 'GET', suffix='/pages/' + index)[0] == 404
    assert request(receiver, 'POST', suffix='/read')[1]['read']
    assert request(receiver, 'POST', suffix='/read')[1]['read']
    assert request(receiver, 'GET', path='/v1/inbox')[1]['status'] == 'empty'


def test_legacy_schema_migration_and_worker_recovery(tmp_path):
    directory = tmp_path / 'legacy'
    directory.mkdir(mode=0o700)
    db = sqlite3.connect(directory / 'jobs.sqlite3')
    db.execute('''CREATE TABLE jobs (request_id TEXT PRIMARY KEY, device_id TEXT NOT NULL,
        digest TEXT NOT NULL,audio BLOB NOT NULL,status TEXT NOT NULL,transcript TEXT,
        error TEXT,retryable INTEGER NOT NULL DEFAULT 0)''')
    statuses = ['transcribing', 'needs_configuration', 'dispatching', 'submitted', 'unconfirmed']
    for i, status in enumerate(statuses):
        db.execute('INSERT INTO jobs VALUES(?,?,?,?,?,?,?,?)',
                   (f'{i:032x}', DEVICE, 'old', wav(), status, '旧任务', 'old_error', 0))
    db.commit()
    db.close()
    store = Store(directory)
    try:
        assert [store.get(f'{i:032x}', DEVICE)['status'] for i in range(5)] == [
            'stored', 'ready', 'needs_review', 'needs_review', 'needs_review']
        with pytest.raises(ValueError, match='storage_already_in_use'):
            Store(directory)
        assert Worker(store, lambda _: pytest.fail('reuse saved transcription')).process_one()
        assert not Worker(store, lambda _: pytest.fail('never execute history')).process_one()
    finally:
        store.close()


def test_slow_render_is_not_published_before_all_pages(storage):
    ready(storage)
    storage.claim_request(RID, CLAIM)
    started, release = threading.Event(), threading.Event()
    renderer = RenderPages(font_path())
    def slow(text):
        started.set()
        assert release.wait(3)
        return renderer(text)
    storage.renderer = slow
    with ThreadPoolExecutor(max_workers=2) as executor:
        reply = executor.submit(storage.reply_request, RID, CLAIM, '你' * 128)
        assert started.wait(3)
        read = executor.submit(storage.get, RID, DEVICE)
        assert not read.done()
        release.set()
        assert reply.result()['page_count'] == 3
        assert read.result()['status'] == 'replied'


def test_page_capacity_failure_preserves_claim(tmp_path):
    store = Store(tmp_path / 'quota', max_bytes=len(wav()) + 100)
    try:
        ready(store)
        store.claim_request(RID, CLAIM)
        with pytest.raises(RequestError, match='storage_full'):
            store.reply_request(RID, CLAIM, '答复')
        assert store.get(RID, DEVICE)['status'] == 'claimed'
        assert store.db.execute('SELECT COUNT(*) FROM pages').fetchone()[0] == 0
    finally:
        store.close()


def test_sql_failure_rolls_back_pages_and_reply(storage):
    ready(storage)
    storage.claim_request(RID, CLAIM)
    storage.db.execute('''CREATE TRIGGER fail_reply BEFORE UPDATE OF reply ON jobs
        BEGIN SELECT RAISE(ABORT, 'simulated disk failure'); END''')
    with pytest.raises(sqlite3.IntegrityError):
        storage.reply_request(RID, CLAIM, '答复')
    assert storage.get(RID, DEVICE)['status'] == 'claimed'
    assert storage.db.execute('SELECT COUNT(*) FROM pages').fetchone()[0] == 0
    storage.db.execute('DROP TRIGGER fail_reply')
    assert storage.reply_request(RID, CLAIM, '答复')['status'] == 'replied'


def test_claimed_tasks_cannot_hide_new_ready_requests(storage):
    for i in range(6):
        request_id = f'{i:032x}'
        ready(storage, request_id)
        if i < 5:
            storage.claim_request(request_id, f'{i + 100:032x}')
    state = storage.mailbox_state()
    assert len(state['requests']) == 5
    assert state['requests'][0]['request_id'] == f'{5:032x}'


def test_concurrent_retry_only_one_transcription(storage):
    ready(storage)
    storage.update(RID, 'failed', error='transcription_failed', retryable=True)
    def retry(_):
        try:
            storage.retry(RID, DEVICE)
            return True
        except RequestError:
            return False
    with ThreadPoolExecutor(max_workers=6) as executor:
        assert sum(executor.map(retry, range(6))) == 1
    worker = Worker(storage, lambda _: pytest.fail('reuse durable transcription'))
    with ThreadPoolExecutor(max_workers=6) as executor:
        assert sum(executor.map(lambda _: worker.process_one(), range(6))) == 1
    assert storage.get(RID, DEVICE)['status'] == 'ready'
