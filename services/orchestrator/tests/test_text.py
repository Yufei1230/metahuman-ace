from __future__ import annotations

import unittest

from app.text import SentenceChunker


class SentenceChunkerTests(unittest.TestCase):
    def test_english_sentences_across_deltas_preserve_decimal(self) -> None:
        chunker = SentenceChunker()
        self.assertEqual(chunker.push("The value is 3."), [])
        self.assertEqual(chunker.push("14. Hello"), ["The value is 3.14."])
        self.assertEqual(chunker.push(" there!"), ["Hello there!"])
        self.assertEqual(chunker.flush(), "")
    def test_chunker_emits_complete_sentences(self) -> None:
        chunker = SentenceChunker()
        first = chunker.push("今日は")
        second = chunker.push("晴れです。明日")
        third = chunker.push("も晴れるでしょう。")
        self.assertEqual(first, [])
        self.assertEqual(second, ["今日は晴れです。"])
        self.assertEqual(third, ["明日も晴れるでしょう。"])


if __name__ == "__main__":
    unittest.main()

