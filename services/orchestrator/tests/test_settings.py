from __future__ import annotations

import os
import unittest
from unittest.mock import patch

from app.settings import Settings


EXPECTED_ASR_MODEL = "nvidia/nemotron-3.5-asr-streaming-0.6b"
EXPECTED_NIM_LLM_BASE_URL = "https://integrate.api.nvidia.com/v1"
EXPECTED_NIM_LLM_MODEL = "nvidia/nemotron-3.5-lightning-30b-a3b"


class SettingsTests(unittest.TestCase):
    def test_relative_storage_is_anchored_to_service_directory(self) -> None:
        from pathlib import Path
        with patch.dict(os.environ, {"ACE_LOG_DIR": "runtime/logs", "ACE_AUDIO_DIR": "runtime/audio"}, clear=True):
            settings = Settings(_env_file=None)
        self.assertTrue(settings.log_dir.is_absolute())
        self.assertEqual(settings.log_dir.parent.parent, Path(__file__).resolve().parents[1])
        self.assertEqual(settings.audio_dir.parent.parent, Path(__file__).resolve().parents[1])

    def test_default_asr_model_uses_nemotron_streaming(self) -> None:
        with patch.dict(os.environ, {}, clear=True):
            settings = Settings(_env_file=None)

        self.assertEqual(settings.asr_model, EXPECTED_ASR_MODEL)

    def test_default_llm_uses_hosted_nim(self) -> None:
        with patch.dict(os.environ, {}, clear=True):
            settings = Settings(_env_file=None)

        self.assertEqual(settings.nim_llm_base_url, EXPECTED_NIM_LLM_BASE_URL)
        self.assertEqual(settings.nim_llm_model, EXPECTED_NIM_LLM_MODEL)

    def test_default_language_is_english(self) -> None:
        with patch.dict(os.environ, {}, clear=True):
            settings = Settings(_env_file=None)

        self.assertEqual(settings.assistant_language, "en-US")
        self.assertEqual(settings.asr_language_code, "en-US")
        self.assertEqual(settings.tts_language_code, "en-US")
        self.assertEqual(settings.tts_voice, "Magpie-Multilingual.EN-US.Aria")
        self.assertIn("English", settings.system_prompt)

    def test_japanese_configuration_preserves_prompt_and_voice(self) -> None:
        with patch.dict(os.environ, {"ASSISTANT_LANGUAGE": "ja-JP"}, clear=True):
            settings = Settings(_env_file=None)

        self.assertEqual(settings.asr_language_code, "ja-JP")
        self.assertEqual(settings.tts_language_code, "ja-JP")
        self.assertEqual(settings.tts_voice, "Magpie-Multilingual.JA-JP.Louise")

        self.assertIn("標準語", settings.system_prompt)
        self.assertIn("香川", settings.system_prompt)
        self.assertIn("40から120文字", settings.system_prompt)
        self.assertIn("対話型バーチャルアシスタント", settings.system_prompt)
        self.assertNotIn("大阪弁", settings.system_prompt)
        self.assertNotIn("大藪", settings.system_prompt)
        self.assertNotIn("/no_think", settings.system_prompt)

    def test_language_overrides_and_legacy_aliases(self) -> None:
        with patch.dict(os.environ, {"ACE_ASR_LANGUAGE_CODE": "multi", "TTS_LANGUAGE": "ja-JP", "ACE_SYSTEM_PROMPT": "Custom prompt", "ACE_TTS_VOICE": "custom-voice"}, clear=True):
            settings = Settings(_env_file=None)
        self.assertEqual(settings.asr_language_code, "multi")
        self.assertEqual(settings.tts_language_code, "ja-JP")
        self.assertEqual(settings.system_prompt, "Custom prompt")
        self.assertEqual(settings.tts_voice, "custom-voice")


if __name__ == "__main__":
    unittest.main()
