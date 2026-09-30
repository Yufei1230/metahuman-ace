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
        first = chunker.push("It is ")
        second = chunker.push("sunny today!Tomorrow ")
        third = chunker.push("will also be sunny!")
        self.assertEqual(first, [])
        self.assertEqual(second, ["It is sunny today!"])
        self.assertEqual(third, ["Tomorrow will also be sunny!"])


if __name__ == "__main__":
    unittest.main()

