"""Single local transcription worker; ready requests wait in the MCP mailbox."""
import io
import threading


class WhisperTranscriber:
    def __init__(self, model="base"):
        self.model_name, self.model = model, None

    def load(self, *, allow_download=False):
        if self.model is None:
            from faster_whisper import WhisperModel
            self.model = WhisperModel(self.model_name, device="cpu", compute_type="int8",
                                      local_files_only=not allow_download)
        return self.model

    def __call__(self, audio):
        self.load()
        segments, _ = self.model.transcribe(io.BytesIO(audio), vad_filter=True)
        return "".join(segment.text for segment in segments).strip()


class Worker:
    def __init__(self, store, transcribe):
        self.store, self.transcribe = store, transcribe
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self.run, name="voice-worker", daemon=True)

    def start(self):
        self.thread.start()

    def stop(self):
        self.stop_event.set()
        self.thread.join()

    def run(self):
        while not self.stop_event.is_set():
            try:
                worked = self.process_one()
            except Exception:
                # Fail closed on a storage failure; restart recovers the persisted stage.
                self.stop_event.set()
                return
            if not worked:
                self.stop_event.wait(0.2)

    def process_one(self):
        row = self.store.claim()
        if row is None:
            return False
        request_id = row["request_id"]
        transcript = row["transcript"]
        if transcript is None:
            try:
                transcript = self.transcribe(row["audio"])
                if not isinstance(transcript, str) or not transcript.strip() or len(transcript) > 16000:
                    raise ValueError()
                transcript = transcript.strip()
            except Exception:
                self.store.update(request_id, "failed", error="transcription_failed", retryable=True)
                return True
        self.store.update(request_id, "ready", transcript=transcript)
        return True
