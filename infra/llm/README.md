# LLM Endpoint Check

Use `check_llm_endpoint.py` to validate an OpenAI-compatible NVIDIA endpoint.
Keep credentials in environment variables rather than tracked files.

```bash
export NVIDIA_API_KEY=<your-api-key>
python3 infra/llm/check_llm_endpoint.py \
  --base-url https://integrate.api.nvidia.com/v1 \
  --model nvidia/nemotron-3.5-lightning-30b-a3b
```

Run with `--help` for timeout, prompt, and generation options. The default
prompt and assistant instructions are English. This helper checks the LLM
endpoint; use the orchestrator's audio demo to verify a full conversation turn.
