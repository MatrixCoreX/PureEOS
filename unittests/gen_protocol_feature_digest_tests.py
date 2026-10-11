#!/usr/bin/env python3

import re
import hashlib
import json
import sys

pattern = re.compile(r"\*\n(Builtin protocol feature: (.*)(?:\*[^/]|[^\*])*)\*/")
# PureEOS also declares description preimages directly as string literals. Hash
# them independently in Python, never copy the digest calculated by the C++ code.
inline_pattern = re.compile(
    r'builtin_protocol_feature_spec\s*\{\s*"([^"\n]+)"\s*,\s*'
    r'digest_type::hash\(std::string\(("(?:[^"\\]|\\.)*")\)\)')


def description_digests(contents):
    descriptions = [(m.group(2), m.group(1)) for m in pattern.finditer(contents)]
    descriptions += [(m.group(1), json.loads(m.group(2))) for m in inline_pattern.finditer(contents)]
    result = {}
    for codename, description in descriptions:
        if codename in result:
            raise ValueError('duplicate description for ' + codename)
        result[codename] = hashlib.sha256(description.encode('utf8')).hexdigest()
    return result

def main():
    with open(sys.argv[1], "r") as f:
        contents = f.read()
        print('#include <eosio/chain/protocol_feature_manager.hpp>')
        print('#include <map>')

        print('#include <boost/test/unit_test.hpp>')

        print('using namespace eosio::chain;')

        print('BOOST_AUTO_TEST_CASE(protocol_feature_digest_tests) {')
        print('   std::map<std::string, std::string> digests;')
        for codename, digest in description_digests(contents).items():
            print('   digests.emplace("%s", "%s");' % (codename, digest))
        print('   BOOST_REQUIRE_EQUAL(digests.size(), builtin_protocol_feature_codenames.size());')
        print('   for(const auto& [id, spec] : builtin_protocol_feature_codenames) {')
        print('      BOOST_REQUIRE_MESSAGE(digests.count(spec.codename), spec.codename);')
        print('      BOOST_TEST(digests.at(spec.codename) == fc::variant(spec.description_digest).as<std::string>());')
        print('   }')
        print('}')

if __name__ == "__main__":
    main()
