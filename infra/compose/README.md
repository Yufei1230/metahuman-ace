# Speech NIM Compose

`docker-compose.yml` pins ASR/TTS NIM to `GPU1`. Store large caches and logs outside the repository; customize the paths in `.env` for your host.

## Start

```bash
cp .env.example .env
docker compose --env-file .env -f docker-compose.yml up -d
```

## Verify

```bash
python3 check_nim_stack.py --asr-http-url http://127.0.0.1:9000 --tts-http-url http://127.0.0.1:9001
python3 check_nim_stack.py --tts-grpc 127.0.0.1:50052 --tts-text "Hello, this is a speech synthesis test."
```

To test ASR, provide a `16kHz mono PCM16 WAV` file.

```bash
python3 check_nim_stack.py --asr-grpc 127.0.0.1:50051 --asr-wav /path/to/input.wav
```

