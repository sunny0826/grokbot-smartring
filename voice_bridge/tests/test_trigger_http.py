import asyncio
import io
import json
import secrets

import httpx2
from mcp import ClientSession
from mcp.client.streamable_http import streamable_http_client
from mcp.types import Implementation
import pytest

from conftest import DEVICE, RID, TOKEN, private_write
from test_bridge import request
from test_gateway import call, command
from grok_gadgets_gateway.mcp_server import RateLimiter
from grok_gadgets_gateway.service import serve_gateway
from voice_bridge.enroll import enroll
from voice_bridge.mailbox import MAILBOX_ID, Mailbox
from voice_bridge.notifier import Notifier
from voice_bridge.trigger import TriggerConfig, TriggerSender
from voice_bridge.worker import Worker


def sender(endpoint):
    return TriggerSender(TriggerConfig.parse({"url": endpoint[0], "key": TOKEN}),
                         MAILBOX_ID, context=endpoint[3])


@pytest.mark.parametrize("status,expected,pause", [
    (200, "accepted", False), (202, "blocked", True), (301, "blocked", True),
    (401, "blocked", True), (403, "blocked", True), (404, "blocked", True),
    (429, "retry", False), (503, "retry", False),
])
def test_official_bot_status_contract_and_no_redirect(trigger_endpoint, status, expected, pause):
    url, requests, outcome, _ = trigger_endpoint
    outcome["status"] = status
    outcome["headers"] = {"Retry-After": "300", "Location": url + "/redirected"}
    result = sender(trigger_endpoint)(RID)
    assert result.state == expected and result.pause == pause
    assert len(requests) == 1
    path, headers, body = requests[0]
    assert path == "/wake"
    assert headers["Authorization"] == "Bearer " + TOKEN
    assert body == {"event": "voice.ready", "version": 1, "request_id": RID, "mailbox_id": MAILBOX_ID}
    if expected == "retry":
        assert result.retry_after == 300


def test_tls_verification_failure_is_unknown_and_does_not_transmit(trigger_endpoint):
    result = TriggerSender(TriggerConfig(trigger_endpoint[0], TOKEN), MAILBOX_ID)(RID)
    assert result.state == "uncertain"
    assert not trigger_endpoint[1]
    assert TOKEN not in repr(result)


def test_https_upload_auto_notification_original_mcp_reply_and_page(receiver, storage, trigger_endpoint, tmp_path):
    registry = receiver[2]
    enroll(registry)
    tokenfile = tmp_path / "mcp-token"
    token = secrets.token_urlsafe(32)
    tokenfile.write_text(token)
    tokenfile.chmod(0o600)
    storage.enable_activation()
    notifier = Notifier(storage, sender(trigger_endpoint), clock=lambda: 1000)

    async def exercise():
        async with serve_gateway(registry, tokenfile, device_port=0, port=0,
                                 log_stream=io.StringIO(), rate_limiter=RateLimiter(1000, 1000)) as gateway:
            mailbox = Mailbox(storage, registry, port=gateway.device_port)
            mailbox.start()
            try:
                for _ in range(100):
                    if mailbox.connected.is_set():
                        break
                    await asyncio.sleep(0.02)
                assert mailbox.connected.is_set()
                assert (await asyncio.to_thread(request, receiver))[0] == 201
                assert Worker(storage, lambda _: "请回复自动触发已接通").process_one()
                assert await asyncio.to_thread(notifier.process_one)
                payload = trigger_endpoint[1][0][2]
                assert payload["request_id"] == RID
                async with (
                    httpx2.AsyncClient(headers={"Authorization": "Bearer " + token}, trust_env=False) as http,
                    streamable_http_client(gateway.url, http_client=http) as (read, write),
                    ClientSession(read, write, client_info=Implementation(name="activation-test", version="1")) as session,
                ):
                    await session.initialize()
                    claim_id = secrets.token_hex(16)
                    result = await command(session, "voice.claim", {"request_id": payload["request_id"], "claim_id": claim_id})
                    assert result["ok"] and result["transcript"] == "请回复自动触发已接通"
                    duplicate = await command(session, "voice.claim", {"request_id": RID, "claim_id": secrets.token_hex(16)})
                    assert not duplicate["ok"] and duplicate["error"] == "claim_conflict"
                    reply = await command(session, "voice.reply", {"request_id": RID, "claim_id": claim_id,
                                                                  "text": "隔离测试：自动通知已接通。"})
                    assert reply["status"] == "replied"
                    assert await command(session, "voice.reply", {"request_id": RID, "claim_id": claim_id,
                                                                  "text": "隔离测试：自动通知已接通。"}) == reply
                assert storage.reserve_notification(10000) is None
                status, page = await asyncio.to_thread(request, receiver, "GET", suffix="/pages/0")
                assert status == 200 and page[:4] == b"VPG1" and len(page) == 91408
                assert not storage.get(RID, DEVICE)["read"]
                assert len(trigger_endpoint[1]) == 1
            finally:
                await asyncio.to_thread(mailbox.stop)
    asyncio.run(exercise())
