"""Resume eligible ready requests without blocking transcription or MCP commands."""
import random
import sys
import threading
import time


class Notifier:
    def __init__(self, store, send, *, clock=time.time):
        self.store, self.send, self.clock = store, send, clock
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self.run, name="grok-notifier", daemon=True)

    def start(self):
        self.thread.start()

    def stop(self):
        self.stop_event.set()
        if self.thread.is_alive():
            self.thread.join()

    def process_one(self):
        item = self.store.reserve_notification(self.clock())
        if item is None:
            return False
        result = self.send(item["request_id"])
        grace = 120 if result.state in ("accepted", "uncertain") else 30
        delay = max(result.retry_after, min(900, grace * 2 ** (item["attempt"] - 1)))
        self.store.finish_notification(item["request_id"], item["attempt"], result.state,
                                       self.clock() + delay + random.uniform(0, 5),
                                       result.error, pause=result.pause)
        if result.pause:
            print(f"Grok activation paused: {result.error}; fix configuration and restart with --retry-activation.",
                  file=sys.stderr, flush=True)
        return True

    def run(self):
        while not self.stop_event.is_set():
            try:
                self.process_one()
            except Exception:
                self.store.activation_error = "notification_worker_unavailable"
                print("Grok activation paused: notification_worker_unavailable", file=sys.stderr, flush=True)
                return
            self.stop_event.wait(0.2)
