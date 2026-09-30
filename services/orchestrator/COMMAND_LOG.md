# Command inventory — Windows setup, 2026-09-27

Commands ran in PowerShell from `C:\Users\Administrator\Documents\tokkio`
unless noted. This lists every shell invocation, including the failed launch.
For inline `python -c` maintenance/inspection commands, the operation is recorded
alongside the invocation instead of duplicating long source code. No secret
values appeared in command arguments or output. Source edits used `apply_patch`
except for the two explicitly listed inline configuration updates.

## Repository inspection

1. `Get-Location`; `rg --files` with filters for `AGENTS.md`, `README.md`,
   requirements, pyproject, env templates, compose, Python, YAML, TOML,
   PowerShell, shell, JavaScript, TypeScript and HTML files.
2. `git status --short`; `Get-ChildItem -Force | Select-Object Name,Mode`.
3. `Get-Content README.md,services/orchestrator/README.md,services/orchestrator/pyproject.toml,services/orchestrator/.env.example,services/orchestrator/app/settings.py`.
4. `Get-Content services/orchestrator/app/adapters/asr.py,services/orchestrator/app/adapters/llm.py,services/orchestrator/app/adapters/tts.py`.
5. `Get-Content tts/README.md,tts/pyproject.toml,unreal/README.md,infra/compose/README.md,infra/tokkio/README.md`;
   `rg --files -g AGENTS.md -g '!Irodori-TTS/**'`; `py -0p`; `Get-Content .gitignore`.
6. `Get-Content -Encoding utf8 services/orchestrator/app/adapters/tts.py,services/orchestrator/app/adapters/asr.py`.
7. `Get-Content -Encoding utf8 services/orchestrator/app/main.py,services/orchestrator/app/session.py,services/orchestrator/tools/demo_client.py`.
8. `Get-Content -Encoding utf8 services/orchestrator/app/adapters/llm.py,services/orchestrator/app/service_status.py,services/orchestrator/tests/test_settings.py,services/orchestrator/tests/test_session_flow.py,services/orchestrator/app/settings.py`.
9. `Get-Content -Encoding utf8 services/orchestrator/tools/demo_client.py,services/orchestrator/tools/init_storage.py,services/orchestrator/app/text.py`;
   `Get-Content -Encoding utf8 services/orchestrator/app/session.py | Select-Object -Skip 90 -First 240`;
   `rg -n 'voices|ja-JP|fallback' services/orchestrator infra/compose/check_nim_stack.py`;
   `py -3.11 -c` to inspect `.env` configuration with key/token/secret/prompt
   values replaced by `[configured]` before printing.

NVIDIA's official documentation was searched with the web tool to verify hosted
voice discovery and English voice naming. This was a read-only tool call,
not a shell command. The live list was then queried below.

## Setup, edits, and first launch

10. `py -3.11 -m venv services/orchestrator/.venv`.
11. `.\services\orchestrator\.venv\Scripts\python.exe -m pip install -e .\services\orchestrator`.
12. `py -3.11 -c` using `urllib.request`: read the key from `.env`, call the
    configured TTS URL's sibling `/list_voices` with bearer authentication,
    save the public voice list to `outputs/tts-voices.json`, and print the list.
13. `.\services\orchestrator\.venv\Scripts\python.exe -c` using `dotenv.set_key`:
    update only `ASSISTANT_LANGUAGE`, `ASR_LANGUAGE`, `TTS_LANGUAGE`, both
    legacy language aliases, `ACE_TTS_VOICE`, `ACE_SYSTEM_PROMPT`,
    `ACE_LOG_DIR`, and `ACE_AUDIO_DIR`. Existing credentials were preserved.
14. `Get-Content -Encoding utf8 services/orchestrator/tests/test_text.py,infra/compose/check_nim_stack.py | Select-Object -First 150`;
    `.\services\orchestrator\.venv\Scripts\python.exe -c` to print
    `inspect.signature(riva.client.ASRService.offline_recognize)`;
    `git ls-files services/orchestrator/.env`.
15. From **services/orchestrator**:
    `.\services\orchestrator\.venv\Scripts\python.exe -m uvicorn app.main:app --host 127.0.0.1 --port 8080`.
    This failed because that relative interpreter path repeated the service directory.
16. From repository root: `.\services\orchestrator\.venv\Scripts\python.exe -c`
    to print `inspect.getsource(riva.client.ASRService.offline_recognize)`.
    The installed client accepts `future=True`, not a direct `timeout` argument;
    the adapter uses a bounded future result and cancels it afterward.
17. From **services/orchestrator**:
    `.\.venv\Scripts\python.exe -m uvicorn app.main:app --host 127.0.0.1 --port 8080`.
    Started successfully; stopped through the tool session's Ctrl+C after edits.
18. `.\services\orchestrator\.venv\Scripts\python.exe -m unittest discover -s services/orchestrator/tests`;
    `.\services\orchestrator\.venv\Scripts\python.exe -m pip check`.
    Ten tests passed; dependency check passed.
19. `git rm --cached -- services/orchestrator/.env`.
    Removed tracking only; the local file remains.

## Real hosted verification

20. From **services/orchestrator**:
    `.\.venv\Scripts\python.exe -m uvicorn app.main:app --host 127.0.0.1 --port 8080`.
21. `.\services\orchestrator\.venv\Scripts\python.exe services/orchestrator/tools/init_storage.py`;
    `.\services\orchestrator\.venv\Scripts\python.exe -c` using `httpx` to
    query `/healthz` and `/status`, save `outputs/status.json`, and print results.
22. `.\services\orchestrator\.venv\Scripts\python.exe services/orchestrator/tools/demo_client.py --generate-input --require-real --output services/orchestrator/outputs/english-e2e.wav`.
    All real hosted stages passed.
23. `.\services\orchestrator\.venv\Scripts\python.exe -c` using `Path` string
    replacements to update `.env.example`: hosted ASR/TTS routing, empty keys,
    English language variables/voice, portable storage, blank automatic prompt,
    44.1 kHz output, and the locally configured hosted LLM model.
24. `.\services\orchestrator\.venv\Scripts\python.exe -c`: read the response WAV
    with `wave`, compute duration/RMS using Python 3.11 `audioop`, resample
    44.1 kHz PCM to 16 kHz, send it to `RivaAsrStream(Settings())`, save
    `outputs/output-transcript.json`, and assert the recognized text includes
    `Kagawa`. Passed. `audioop` emitted its Python 3.13 deprecation notice.
25. `git status --short`; `git diff --stat`;
    `Get-Content -Encoding utf8 services/orchestrator/.env.example`;
    `.\services\orchestrator\.venv\Scripts\python.exe -m unittest discover -s services/orchestrator/tests`;
    `.\services\orchestrator\.venv\Scripts\python.exe -m pip check`.
26. `.\services\orchestrator\.venv\Scripts\python.exe -m unittest discover -s services/orchestrator/tests`.
    Twelve tests passed after adding storage and ASR recovery coverage.
    The foreground service was stopped through the tool session's Ctrl+C.

## Final background launch and checks

27. Background launch from repository root:

```powershell
$backendRoot = (Resolve-Path services/orchestrator).Path
$backendPython = Join-Path $backendRoot '.venv/Scripts/python.exe'
$backendProcess = Start-Process -FilePath $backendPython -ArgumentList @('-m','uvicorn','app.main:app','--host','127.0.0.1','--port','8080') -WorkingDirectory $backendRoot -WindowStyle Hidden -RedirectStandardOutput (Join-Path $backendRoot 'runtime/logs/server.stdout.log') -RedirectStandardError (Join-Path $backendRoot 'runtime/logs/server.stderr.log') -PassThru
$backendProcess.Id | Set-Content -LiteralPath (Join-Path $backendRoot 'runtime/server.pid')
Write-Output "Backend PID: $($backendProcess.Id)"
```

The launcher PID was 1200. Its Python server child reported PID 11320.

28. `.\services\orchestrator\.venv\Scripts\python.exe services/orchestrator/tools/demo_client.py --wav services/orchestrator/outputs/demo-input.wav --require-real --output services/orchestrator/outputs/english-e2e-final.wav`.
    Transport completed, but the hosted LLM returned malformed text. See report.
29. `.\services\orchestrator\.venv\Scripts\python.exe -c` using `httpx`:
    save refreshed `/status` to `outputs/status.json`, print `/healthz` HTTP
    status and public language/mock configuration;
    `git diff --check`; `git status --short`.
    Endpoint and whitespace checks passed. Git warned about normal LF/CRLF conversion.
30. `Get-Content -Tail 25 services/orchestrator/runtime/logs/server.stderr.log`;
    `Get-Content -Tail 15 services/orchestrator/runtime/logs/server.stdout.log`;
    `Get-Process -Id 1200 | Select-Object Id,ProcessName`;
    `Get-Content services/orchestrator/outputs/english-e2e.json`.
31. `.\services\orchestrator\.venv\Scripts\python.exe services/orchestrator/tools/demo_client.py --wav services/orchestrator/outputs/demo-input.wav --require-real --output services/orchestrator/outputs/english-e2e-repeat.wav`.
    Final repeat passed with normal English output, about 8 seconds overall.

Long-running command sessions were polled with `write_stdin`; no additional
shell commands were issued by those polls. No GPU model downloads, Docker,
Kubernetes, Unreal, package installation outside the venv, commits, or pushes
were performed.
