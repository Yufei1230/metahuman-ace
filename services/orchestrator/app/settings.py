from __future__ import annotations

from pathlib import Path

from pydantic import AliasChoices, Field, model_validator
from pydantic_settings import BaseSettings, SettingsConfigDict


DEFAULT_ASR_MODEL = "nvidia/nemotron-3.5-asr-streaming-0.6b"
DEFAULT_NIM_LLM_BASE_URL = "https://integrate.api.nvidia.com/v1"
DEFAULT_NIM_LLM_MODEL = "nvidia/nemotron-3.5-lightning-30b-a3b"
DEFAULT_SYSTEM_PROMPT = (
    "You are Kagawa, a conversational virtual assistant. Respond in English by default. "
    "Speak naturally and concisely in one to three sentences. "
    "Do not include markdown, lists, emoji, or internal reasoning. "
    "Ask for clarification when information is missing rather than inventing facts."
)


class Settings(BaseSettings):
    model_config = SettingsConfigDict(
        env_file=Path(__file__).resolve().parents[1] / ".env",
        env_file_encoding="utf-8", extra="ignore", populate_by_name=True,
    )

    assistant_language: str = Field(default="en-US", validation_alias=AliasChoices("ASSISTANT_LANGUAGE", "ACE_ASSISTANT_LANGUAGE"))

    host: str = Field(default="0.0.0.0", alias="ACE_HOST")
    port: int = Field(default=8080, alias="ACE_PORT")

    log_dir: Path = Field(default=Path(__file__).resolve().parents[1] / "runtime/logs", alias="ACE_LOG_DIR")
    audio_dir: Path = Field(default=Path(__file__).resolve().parents[1] / "runtime/audio", alias="ACE_AUDIO_DIR")

    asr_server: str = Field(default="127.0.0.1:50051", alias="ACE_ASR_SERVER")
    asr_http_url: str = Field(default="http://127.0.0.1:9000", alias="ACE_ASR_HTTP_URL")
    asr_language_code: str = Field(default="", validation_alias=AliasChoices("ASR_LANGUAGE", "ACE_ASR_LANGUAGE_CODE"))
    asr_model: str = Field(default=DEFAULT_ASR_MODEL, alias="ACE_ASR_MODEL")
    asr_sample_rate_hz: int = Field(default=16000, alias="ACE_ASR_SAMPLE_RATE_HZ")
    asr_frame_ms: int = Field(default=20, alias="ACE_ASR_FRAME_MS")

    asr_use_hosted_api: bool = Field(default=False, alias="ACE_ASR_USE_HOSTED_API")
    asr_function_id: str = Field(default="", alias="ACE_ASR_FUNCTION_ID")
    asr_api_key: str = Field(default="", alias="ACE_ASR_API_KEY")
    asr_use_ssl: bool = Field(default=False, alias="ACE_ASR_USE_SSL")

    tts_server: str = Field(default="127.0.0.1:50052", alias="ACE_TTS_SERVER")
    tts_http_url: str = Field(default="http://127.0.0.1:9001", alias="ACE_TTS_HTTP_URL")
    tts_language_code: str = Field(default="", validation_alias=AliasChoices("TTS_LANGUAGE", "ACE_TTS_LANGUAGE_CODE"))
    tts_voice: str = Field(
        default="",
        alias="ACE_TTS_VOICE",
    )
    tts_sample_rate_hz: int = Field(default=44100, alias="ACE_TTS_SAMPLE_RATE_HZ")
    tts_encoding: str = Field(default="LINEAR_PCM", alias="ACE_TTS_ENCODING")
    tts_api_url: str = Field(
    default="https://877104f7-e885-42b9-8de8-f6e4c6303969.invocation.api.nvcf.nvidia.com/v1/audio/synthesize",
    alias="ACE_TTS_API_URL",
    )
    tts_use_hosted_api: bool = Field(default=False, alias="ACE_TTS_USE_HOSTED_API")

    nim_llm_base_url: str = Field(default=DEFAULT_NIM_LLM_BASE_URL, alias="ACE_NIM_LLM_BASE_URL")
    nim_api_key: str = Field(default="", alias="ACE_NIM_API_KEY")
    nim_llm_model: str = Field(default=DEFAULT_NIM_LLM_MODEL, alias="ACE_NIM_LLM_MODEL")
    skip_llm_model_validation: bool = Field(default=False, alias="ACE_SKIP_LLM_MODEL_VALIDATION")
    validate_externals_on_startup: bool = Field(default=False, alias="ACE_VALIDATE_EXTERNALS_ON_STARTUP")

    vad_aggressiveness: int = Field(default=2, alias="ACE_VAD_AGGRESSIVENESS")
    eos_silence_ms: int = Field(default=500, alias="ACE_EOS_SILENCE_MS")
    save_debug_audio: bool = Field(default=True, alias="ACE_SAVE_DEBUG_AUDIO")

    mock_asr: bool = Field(default=False, alias="ACE_MOCK_ASR")
    mock_tts: bool = Field(default=False, alias="ACE_MOCK_TTS")
    mock_llm: bool = Field(default=False, alias="ACE_MOCK_LLM")

    system_prompt: str = Field(default="", alias="ACE_SYSTEM_PROMPT")

    @model_validator(mode="after")
    def language_defaults(self) -> "Settings":
        root = Path(__file__).resolve().parents[1]
        if not self.log_dir.is_absolute():
            self.log_dir = root / self.log_dir
        if not self.audio_dir.is_absolute():
            self.audio_dir = root / self.audio_dir
        self.asr_language_code = self.asr_language_code or self.assistant_language
        self.tts_language_code = self.tts_language_code or self.assistant_language
        if not self.tts_voice:
            voices = {"en-US": "Magpie-Multilingual.EN-US.Aria"}
            if self.tts_language_code not in voices:
                raise ValueError("Set ACE_TTS_VOICE for this TTS language using the server voice list")
            self.tts_voice = voices[self.tts_language_code]
        if not self.system_prompt:
            self.system_prompt = DEFAULT_SYSTEM_PROMPT
            if self.assistant_language != "en-US":
                self.system_prompt = self.system_prompt.replace("English", self.assistant_language)
        self.asr_api_key = self.asr_api_key or self.nim_api_key
        return self

    @property
    def asr_frame_bytes(self) -> int:
        samples_per_frame = self.asr_sample_rate_hz * self.asr_frame_ms // 1000
        return samples_per_frame * 2

    def ensure_runtime_dirs(self) -> None:
        self.log_dir.mkdir(parents=True, exist_ok=True)
        self.audio_dir.mkdir(parents=True, exist_ok=True)
