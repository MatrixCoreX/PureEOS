// Compile the actual contract fixture with an explicit additional boundary test.
// No copied implementation and no production/table mutation shortcut.
#include "../unittests/pureeos_governance_tests.cpp"
#if defined(PUREEOS_ROTATION_CONTRACTS_DIR) && defined(PUREEOS_RESOURCE_SPLIT_CONTRACTS_DIR) && defined(PUREEOS_SINGLE_CHOICE_CONTRACTS_DIR) && defined(PUREEOS_GOVERNANCE_CONTRACTS_DIR)
#include "../unittests/pureeos_blacklist_current_policy_tests.inc"
#endif
