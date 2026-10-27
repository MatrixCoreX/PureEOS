#!/usr/bin/env python3
"""Regression checks for the independent protocol-description hash generator."""
import hashlib
from pathlib import Path
import unittest

from gen_protocol_feature_digest_tests import description_digests


class DescriptionDigestsTest(unittest.TestCase):
    def test_comment_preimage(self):
        preimage = 'Builtin protocol feature: SAMPLE\n\nA description.\n'
        self.assertEqual(description_digests('/*\n' + preimage + '*/'),
                         {'SAMPLE': hashlib.sha256(preimage.encode()).hexdigest()})

    def test_inline_preimage(self):
        source = r'''builtin_protocol_feature_spec{
            "SAMPLE", digest_type::hash(std::string("A description.\n")), {}}
        '''
        self.assertEqual(description_digests(source),
                         {'SAMPLE': hashlib.sha256(b'A description.\n').hexdigest()})

    def test_duplicate_rejected(self):
        source = 'builtin_protocol_feature_spec{"A", digest_type::hash(std::string("a")), {}}'
        with self.assertRaises(ValueError):
            description_digests(source + source)

    def test_rotation_preimage_is_stable(self):
        source = Path(__file__).resolve().parents[1] / 'libraries/chain/protocol_feature_manager.cpp'
        digests = description_digests(source.read_text())
        self.assertEqual(len(digests), 26)
        self.assertEqual(digests['PUREEOS_ROTATION_CONTEXT'],
                         'a1a895da563991c72cef4dfd248d648c979ba104def68f529b2fb7643c584241')


if __name__ == '__main__':
    unittest.main()
