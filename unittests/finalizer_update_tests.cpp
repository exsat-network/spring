#pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wsign-compare"
    #include <boost/test/unit_test.hpp>
#pragma GCC diagnostic pop

#include <eosio/testing/tester.hpp>

using namespace eosio::chain::literals;
using namespace eosio::testing;
using namespace eosio::chain;


/*
 * register test suite `finalizer_update_tests`
 */
BOOST_AUTO_TEST_SUITE(finalizer_update_tests)

// -----------------------------------------------------------------------
// produce one block, and verify that the active finalizer_policy for this
// newly produced `block` matches the passed `generation` and `keys_span`.
// -----------------------------------------------------------------------
static void ensure_next_block_finalizer_policy(validating_tester& t,
                                               uint32_t generation,
                                               std::span<const bls_public_key> keys_span) {
   auto b = t.produce_block();
   t.check_active_finalizer_policy(b, generation, keys_span);
}

// ---------------------------------------------------------------------
// verify that finalizer policy change via set_finalizer take 2 3-chains
// to take effect.
// ---------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(savanna_set_finalizer_single_test) { try {
   validating_tester t;
   size_t num_keys    = 22;
   size_t finset_size = 21;

   // Create finalizer keys
   finalizer_keys fin_keys(t, num_keys, finset_size);

   // set finalizers on current node
   fin_keys.set_node_finalizers(0, num_keys);

   // run initial set_finalizer_policy() and waits until transition is complete
   auto pubkeys0 = fin_keys.transition_to_Savanna();

   // run set_finalizers(), verify it becomes active after exactly two 3-chains
   // -------------------------------------------------------------------------
   auto pubkeys1 = fin_keys.set_finalizer_policy(1);
   auto b0 = t.produce_block();
   t.check_active_finalizer_policy(b0, 1, pubkeys0); // new policy should only be active until after two 3-chains

   auto b3 = t.produce_blocks(3);
   t.check_active_finalizer_policy(b3, 1, pubkeys0); // one 3-chain - new policy still should not be active

   auto b5 = t.produce_blocks(2);
   t.check_active_finalizer_policy(b5, 1, pubkeys0); // one 3-chain + 2 blocks - new policy still should not be active

   auto b6 = t.produce_block();
   t.check_active_finalizer_policy(b6, 2, pubkeys1); // two 3-chain - new policy *should* be active

} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
// Test correct behavior when multiple finalizer policy changes are in-flight
// at the same time.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(savanna_set_finalizer_multiple_test) { try {
   validating_tester t;
   size_t num_keys    = 50;
   size_t finset_size = 21;

   // Create finalizer keys
   finalizer_keys fin_keys(t, num_keys, finset_size);

   // set finalizers on current node
   fin_keys.set_node_finalizers(0, num_keys);

   // run initial set_finalizer_policy() and waits until transition is complete
   auto pubkeys0 = fin_keys.transition_to_Savanna();

   // run set_finalizers() twice in same block, verify only latest one becomes active
   // -------------------------------------------------------------------------------
   (void)fin_keys.set_finalizer_policy(1);
   auto pubkeys2 = fin_keys.set_finalizer_policy(2);
   auto b0 = t.produce_block();
   t.check_active_finalizer_policy(b0, 1, pubkeys0); // new policy should only be active until after two 3-chains
   auto b5 = t.produce_blocks(5);
   t.check_active_finalizer_policy(b5, 1, pubkeys0); // new policy should only be active until after two 3-chains
   auto b6 = t.produce_block();
   t.check_active_finalizer_policy(b6, 2, pubkeys2); // two 3-chain - new policy pubkeys2 *should* be active

   // run a test with multiple set_finlizers in-flight during the two 3-chains they
   // take to become active
   // -----------------------------------------------------------------------------
   auto pubkeys3 = fin_keys.set_finalizer_policy(3);
   b0 = t.produce_block();
   auto pubkeys4 = fin_keys.set_finalizer_policy(4);
   auto b1 = t.produce_block();
   auto b2 = t.produce_block();
   auto pubkeys5 = fin_keys.set_finalizer_policy(5);
   b5 = t.produce_blocks(3);
   t.check_active_finalizer_policy(b5, 2, pubkeys2); // 5 blocks after pubkeys3 (b5 - b0), pubkeys2 should still be active
   b6 = t.produce_block();
   t.check_active_finalizer_policy(b6, 3, pubkeys3); // 6 blocks after pubkeys3 (b6 - b0), pubkeys3 should be active
   auto b7 = t.produce_block();
   t.check_active_finalizer_policy(b7, 4, pubkeys4); // 6 blocks after pubkeys4 (b7 - b1), pubkeys4 should be active

   auto b8 = t.produce_block();
   t.check_active_finalizer_policy(b8, 4, pubkeys4); // 7 blocks after pubkeys4, pubkeys4 should still be active
   auto b9 = t.produce_block();
   t.check_active_finalizer_policy(b9, 5, pubkeys5); // 6 blocks after pubkeys5 (b9 - b3), pubkeys5 should be active

   // and no further change
   ensure_next_block_finalizer_policy(t, 5, pubkeys5);
   ensure_next_block_finalizer_policy(t, 5, pubkeys5);
   ensure_next_block_finalizer_policy(t, 5, pubkeys5);
   ensure_next_block_finalizer_policy(t, 5, pubkeys5);
   ensure_next_block_finalizer_policy(t, 5, pubkeys5);
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()