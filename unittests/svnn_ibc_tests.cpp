#include <eosio/chain/abi_serializer.hpp>
#include <eosio/testing/tester.hpp>

#include <fc/variant_object.hpp>

#include <boost/test/unit_test.hpp>

#include <contracts.hpp>
#include <test_contracts.hpp>
#include "fork_test_utilities.hpp"

#include <eosio/chain/exceptions.hpp>

#include "finality_test_cluster.hpp"

using namespace eosio::chain;
using namespace eosio::testing;

using mvo = mutable_variant_object;

static digest_type hash_pair(const digest_type& a, const digest_type& b) {
   return fc::sha256::hash(std::pair<const digest_type&, const digest_type&>(a, b));
}

BOOST_AUTO_TEST_SUITE(svnn_ibc)

   //extract instant finality data from block header extension, as well as qc data from block extension
   qc_data_t extract_qc_data(const signed_block_ptr& b) {
      auto hexts = b->validate_and_extract_header_extensions();
      if (auto if_entry = hexts.lower_bound(instant_finality_extension::extension_id()); if_entry != hexts.end()) {
         auto& if_ext   = std::get<instant_finality_extension>(if_entry->second);

         // get the matching qc extension if present
         auto exts = b->validate_and_extract_extensions();
         if (auto entry = exts.lower_bound(quorum_certificate_extension::extension_id()); entry != exts.end()) {
            auto& qc_ext = std::get<quorum_certificate_extension>(entry->second);
            return qc_data_t{ std::move(qc_ext.qc), if_ext.qc_claim };
         }
         return qc_data_t{ {}, if_ext.qc_claim };
      }
      return {};
   }

   //generate a proof of inclusion for a node at index from a list of leaves
   std::vector<digest_type> generate_proof_of_inclusion(const std::vector<digest_type> leaves, const size_t index) {
      auto _leaves = leaves;
      auto _index = index;

      std::vector<digest_type> merkle_branches;

      while (_leaves.size()>1){
         std::vector<digest_type> new_level;
         for (size_t i = 0 ; i < _leaves.size() ; i+=2){
            digest_type left = _leaves[i];

            if (i + 1 < _leaves.size() && (i + 1 != _leaves.size() - 1 || _leaves.size() % 2 == 0)){
               // Normal case: both children exist and are not at the end or are even
               digest_type right = _leaves[i+1];

               new_level.push_back(fc::sha256::hash(std::pair<digest_type, digest_type>(left, right)));
               if (_index == i || _index == i + 1) {
                 merkle_branches.push_back(_index == i ? right : left);
                 _index = i / 2; // Update index for next level

               }
            } else {
               // Odd number of leaves at this level, and we're at the end
               new_level.push_back(left); // Promote the left (which is also the right in this case)
               if (_index == i) _index = i / 2; // Update index for next level, no sibling to add

            }
         }
         _leaves = new_level;
      }
      return merkle_branches;
   }

   bool has_new_finalizer_policy(const signed_block_ptr block){

      // extract new finalizer policy
      eosio::chain::block_header_extension block_if_ext =
         *block->extract_header_extension(eosio::chain::instant_finality_extension::extension_id());

      std::optional<eosio::chain::finalizer_policy> maybe_new_finalizer_policy =
         std::get<eosio::chain::instant_finality_extension>(block_if_ext).new_finalizer_policy;

      if (maybe_new_finalizer_policy.has_value()) return true;
      else return false;

   }

   eosio::chain::finalizer_policy extract_finalizer_policy(const signed_block_ptr block){

      // extract new finalizer policy
      eosio::chain::block_header_extension block_if_ext =
         *block->extract_header_extension(eosio::chain::instant_finality_extension::extension_id());

      std::optional<eosio::chain::finalizer_policy> maybe_new_finalizer_policy =
         std::get<eosio::chain::instant_finality_extension>(block_if_ext).new_finalizer_policy;

      assert(maybe_new_finalizer_policy.has_value());

      return maybe_new_finalizer_policy.value();

   }

   //ibc_cluster inherits from finality_test_cluster and returns an extended struct, with data relevant to IBC when producing blocks
   //Since the ibc contract only cares about verifying finality proofs, ibc_cluster doesn't support forks or rollbacks, and always assumes the happy path in finality progression
   template<size_t NUM_NODES>
   class ibc_cluster : public finality_test_cluster<NUM_NODES> {
   public:

      struct ibc_block_data_t {
         signed_block_ptr block;
         action_trace onblock_trace;
         std::vector<transaction_trace_ptr> traces;
         finality_data_t finality_data;
         digest_type action_mroot;
         digest_type base_digest;
         digest_type finalizer_policy_digest;
         digest_type finality_digest;
         digest_type computed_finality_digest;
         digest_type afp_base_digest;
         digest_type finality_leaf;
         digest_type finality_root;
      };

      // cache finalizer policy + digest
      eosio::chain::finalizer_policy current_finalizer_policy;
      digest_type current_finalizer_policy_digest;

      eosio::chain::finalizer_policy pending_finalizer_policy;
      digest_type pending_finalizer_policy_digest;

      uint32_t blocks_since_pending_policy = 0;

      bool is_genesis = true;

      std::vector<digest_type> get_finality_leaves(const size_t cutoff){
         assert(cutoff>=0 && cutoff<finality_leaves.size());
         return std::vector<digest_type>(finality_leaves.begin(), finality_leaves.begin() + cutoff + 1);
      }

      ibc_block_data_t produce_ibc_block(){
         auto result = this->produce_and_push_block_ex();
         signed_block_ptr block = result.block;
         action_trace onblock_trace = result.onblock_trace->action_traces[0];
         std::vector<transaction_trace_ptr> traces = result.traces;

         if (has_new_finalizer_policy(block)){
            
            eosio::chain::finalizer_policy new_policy = extract_finalizer_policy(block);

            if (is_genesis){
               current_finalizer_policy = new_policy;
               current_finalizer_policy_digest = fc::sha256::hash(new_policy);
               pending_finalizer_policy = new_policy;
               pending_finalizer_policy_digest = fc::sha256::hash(new_policy);
            }
            else {
               pending_finalizer_policy = new_policy;
               pending_finalizer_policy_digest = fc::sha256::hash(new_policy);
               blocks_since_pending_policy = 0;
            }

         }

         if (current_finalizer_policy_digest!= pending_finalizer_policy_digest && blocks_since_pending_policy == 6){
            current_finalizer_policy = pending_finalizer_policy;
            current_finalizer_policy_digest = fc::sha256::hash(current_finalizer_policy);
         }

         blocks_since_pending_policy++;

         this->process_votes(1, this->num_needed_for_quorum); //enough to reach quorum threshold
         finality_data_t finality_data = *this->node0.control->head_finality_data();
         digest_type action_mroot = finality_data.action_mroot;
         digest_type base_digest = finality_data.base_digest;
         digest_type afp_base_digest = hash_pair(current_finalizer_policy_digest, base_digest);
         digest_type finality_digest;

         if (is_genesis){

            // one-time genesis finality digest computation
            finality_digest = fc::sha256::hash(eosio::chain::finality_digest_data_v1{
               .active_finalizer_policy_generation      = 1,
               .finality_tree_digest                    = digest_type(), //nothing to finalize yet
               .active_finalizer_policy_and_base_digest = afp_base_digest
            });
            is_genesis = false;
         }
         else finality_digest = this->node0.control->get_strong_digest_by_id(block->calculate_id());


         digest_type finality_leaf = fc::sha256::hash(valid_t::finality_leaf_node_t{
            .block_num = block->block_num(),
            .finality_digest = finality_digest,
            .action_mroot = action_mroot
         });
         digest_type finality_root = block->action_mroot;

         digest_type computed_finality_digest = fc::sha256::hash(eosio::chain::finality_digest_data_v1{
               .active_finalizer_policy_generation      = current_finalizer_policy.generation,
               .finality_tree_digest                    = finality_root, //nothing to finalize yet
               .active_finalizer_policy_and_base_digest = afp_base_digest
            });

         finality_leaves.push_back(finality_leaf);

         return {block, onblock_trace, traces, finality_data, action_mroot, base_digest, current_finalizer_policy_digest, finality_digest, computed_finality_digest, afp_base_digest, finality_leaf, finality_root };

      }

      void produce_ibc_blocks(const uint32_t count){
         for (uint32_t i = 0 ; i < count ; i++){
            produce_ibc_block();
         } 
      }

      ibc_cluster(finality_cluster_config_t config = {.transition_to_savanna = false})
      : finality_test_cluster<NUM_NODES>(config) {

      }

   private:
      std::vector<digest_type> finality_leaves;

   };

   BOOST_AUTO_TEST_CASE(ibc_test) { try {

      // cluster is set up with the head about to produce IF Genesis
      ibc_cluster<4> cluster;

      // produce IF Genesis block
      auto genesis_block_result = cluster.produce_ibc_block();

      // ensure out of scope setup and initial cluster wiring is consistent
      BOOST_CHECK_EQUAL(genesis_block_result.block->block_num(), 4);
      
      BOOST_CHECK_EQUAL(cluster.current_finalizer_policy.finalizers.size(), cluster.num_nodes);
      BOOST_CHECK_EQUAL(cluster.current_finalizer_policy.generation, 1);

      // create the ibc account and deploy the ibc contract to it 
      cluster.node0.create_account( "ibc"_n );
      cluster.node0.set_code( "ibc"_n, eosio::testing::test_contracts::svnn_ibc_wasm());
      cluster.node0.set_abi( "ibc"_n, eosio::testing::test_contracts::svnn_ibc_abi());

      cluster.node0.push_action( "ibc"_n, "setfpolicy"_n, "ibc"_n, mvo()
         ("from_block_num", 1)
         ("policy", cluster.current_finalizer_policy)
      );

      // Transition block. Finalizers are not expected to vote on this block.
      auto block_1_result = cluster.produce_ibc_block();

      // Proper IF Block. From now on, finalizers must vote. Moving forward, the header action_mroot
      // field is reconverted to provide the finality_mroot.
      // The action_mroot is instead provided via the finality data
      auto block_2_result = cluster.produce_ibc_block();
      
      // block_3 contains a QC over block_2
      auto block_3_result = cluster.produce_ibc_block();
      // block_4 contains a QC over block_3
      auto block_4_result = cluster.produce_ibc_block();
      // block_5 contains a QC over block_4, which completes the 3-chain for block_2 and
      // serves as a proof of finality for it
      auto block_5_result = cluster.produce_ibc_block();
      auto block_6_result = cluster.produce_ibc_block();

      qc_data_t qc_b_4 = extract_qc_data(block_4_result.block);
      qc_data_t qc_b_5 = extract_qc_data(block_5_result.block);
      qc_data_t qc_b_6 = extract_qc_data(block_6_result.block);

      BOOST_TEST(qc_b_4.qc.has_value());
      BOOST_TEST(qc_b_5.qc.has_value());
      BOOST_TEST(qc_b_6.qc.has_value());

      std::stringstream sstream;
      sstream << std::hex << (1 << (cluster.num_needed_for_quorum + 1)) - 1; // we expect a quorum of finalizers to vote
                                                                             // +1 because num_needed_for_quorum excludes node0
      std::string raw_bitset = sstream.str();
      if (raw_bitset.size() % 2)
         raw_bitset.insert(0, "0");

      // create a few proofs we'll use to perform tests

      // heavy proof #1. Proving finality of block #2 using block #2 finality root
      mutable_variant_object heavy_proof_1 = mvo()
         ("assert", false)
         ("proof", mvo() 
            ("finality_proof", mvo() //proves finality of block #2
               ("qc_block", mvo()
                  ("major_version", 1)
                  ("minor_version", 0)
                  ("finalizer_policy_generation", 1)
                  ("witness_hash", block_4_result.afp_base_digest)
                  ("finality_mroot", block_4_result.finality_root)
               )
               ("qc", mvo()
                  ("signature", qc_b_5.qc.value().data.sig.to_string())
                  ("finalizers", raw_bitset) 
               )
            )
            ("target_block_proof_of_inclusion", mvo() 
               ("target_node_index", 2)
               ("last_node_index", 2)
               ("target",  mvo() //target block #2
                  ("finality_data", mvo() 
                     ("major_version", 1)
                     ("minor_version", 0)
                     ("finalizer_policy_generation", 1)
                     ("witness_hash", block_2_result.afp_base_digest)
                     ("finality_mroot", block_2_result.finality_root)
                  )
                  ("dynamic_data", mvo() 
                     ("block_num", block_2_result.block->block_num())
                     ("action_proofs", fc::variants())
                     ("action_mroot", block_2_result.action_mroot)
                  )
               )
               ("merkle_branches", generate_proof_of_inclusion(cluster.get_finality_leaves(2), 2))
            )
         );

      // heavy proof #2. Proving finality of block #2 using block #3 finality root
      mutable_variant_object heavy_proof_2 = mvo()
         ("assert", false)
         ("proof", mvo() 
            ("finality_proof", mvo()  //proves finality of block #3
               ("qc_block", mvo()
                  ("major_version", 1)
                  ("minor_version", 0)
                  ("finalizer_policy_generation", 1)
                  ("witness_hash", block_5_result.afp_base_digest)
                  ("finality_mroot", block_5_result.finality_root)
               )
               ("qc", mvo()
                  ("signature", qc_b_6.qc.value().data.sig.to_string())
                  ("finalizers", raw_bitset) 
               )
            )
            ("target_block_proof_of_inclusion", mvo() 
               ("target_node_index", 2)
               ("last_node_index", 3)
               ("target", mvo() //target block #2
                  ("finality_data", mvo() 
                     ("major_version", 1)
                     ("minor_version", 0)
                     ("finalizer_policy_generation", 1)
                     ("witness_hash", block_2_result.afp_base_digest)
                     ("finality_mroot", block_2_result.finality_root)
                  )
                  ("dynamic_data", mvo() 
                     ("block_num", block_2_result.block->block_num())
                     ("action_proofs", fc::variants())
                     ("action_mroot", block_2_result.action_mroot)
                  )
               )
               ("merkle_branches", generate_proof_of_inclusion(cluster.get_finality_leaves(3), 2))
            )
         );

      // light proof #1. Attempt to prove finality of block #2 with previously proven finality root of block #2
      mutable_variant_object light_proof_1 = mvo()
         ("assert", false)
         ("proof", mvo() 
            ("target_block_proof_of_inclusion", mvo() 
               ("target_node_index", 2)
               ("last_node_index", 2)
               ("target", mvo() 
                  ("finality_data", mvo() 
                     ("major_version", 1)
                     ("minor_version", 0)
                     ("finalizer_policy_generation", 1)
                     ("witness_hash", block_2_result.afp_base_digest)
                     ("finality_mroot", block_2_result.finality_root)
                  )
                  ("dynamic_data", mvo() 
                     ("block_num", block_2_result.block->block_num())
                     ("action_proofs", fc::variants())
                     ("action_mroot", block_2_result.action_mroot)
                  )
               )
               ("merkle_branches", generate_proof_of_inclusion(cluster.get_finality_leaves(2), 2))
            )
         );
      

      // verify first heavy proof
      action_trace check_heavy_proof_1_trace = cluster.node0.push_action("ibc"_n, "checkproof"_n, "ibc"_n, heavy_proof_1)->action_traces[0];

      // now that we stored the proven root, we should be able to verify the same proof without
      // the finality data (aka light proof)
      action_trace check_light_proof_1_trace = cluster.node0.push_action("ibc"_n, "checkproof"_n, "ibc"_n, light_proof_1)->action_traces[0];

      // verify a second proof where the target block is different from the finality block.
      // This also saves a second finality root to the contract, marking the beginning of the cache
      // timer for the older finality root.
      action_trace check_heavy_proof_2_trace = cluster.node0.push_action("ibc"_n, "checkproof"_n, "ibc"_n, heavy_proof_2)->action_traces[0];

      // produce the block to avoid duplicate transaction error
      auto block_7_result = cluster.produce_ibc_block();

      // since a few actions were included in the previous block, we can verify that they correctly hash into the action_mroot for that block
      auto pair_1_hash = hash_pair(block_7_result.onblock_trace.digest_savanna(), check_heavy_proof_1_trace.digest_savanna());
      auto pair_2_hash = hash_pair(check_light_proof_1_trace.digest_savanna(), check_heavy_proof_2_trace.digest_savanna());

      auto computed_action_mroot = hash_pair(pair_1_hash, pair_2_hash);

      BOOST_CHECK(computed_action_mroot == block_7_result.action_mroot);

      // we now test finalizer policy change
      auto indices1 = cluster.fin_policy_indices_0;  // start from original set of indices
      indices1[0] = 1;                       // update key used for node0 in policy

      digest_type previous_policy_digest = cluster.current_finalizer_policy_digest;

      std::cout << "previous_policy_digest before block 8 " << previous_policy_digest << "\n";

      // change the finalizer policy by rotating the key of node0
      cluster.node0.finkeys.set_finalizer_policy(indices1);

      // produce a new block. This block contains a new finalizer policy
      auto block_8_result = cluster.produce_ibc_block();

      BOOST_TEST(has_new_finalizer_policy(block_8_result.block));

      auto new_finalizer_policy = extract_finalizer_policy(block_8_result.block);

      // make new finalizer policy block final
      auto block_9_result = cluster.produce_ibc_block();
      auto block_10_result = cluster.produce_ibc_block();
      auto block_11_result = cluster.produce_ibc_block(); //QC on #10 included in #11 makes #8 final, new policy is now pending


      auto block_12_result = cluster.produce_ibc_block();
      auto block_13_result = cluster.produce_ibc_block(); //new policy takes effect on next block
   
      //verify that the current finalizer policy is still in force up to this point    
      BOOST_TEST(previous_policy_digest==cluster.current_finalizer_policy_digest);
      
      auto block_14_result = cluster.produce_ibc_block();

      //verify that the new finalizer policy is now in force
      BOOST_TEST(previous_policy_digest!=cluster.current_finalizer_policy_digest);

      auto block_15_result = cluster.produce_ibc_block();
      auto block_16_result = cluster.produce_ibc_block();
      auto block_17_result = cluster.produce_ibc_block();

      std::cout << " policy_digest 1  " <<  block_1_result.finalizer_policy_digest << " " <<  block_1_result.computed_finality_digest << " " <<  block_1_result.finality_digest << "\n";
      std::cout << " policy_digest 2  " <<  block_2_result.finalizer_policy_digest << " " <<  block_2_result.computed_finality_digest << " " <<  block_2_result.finality_digest << "\n";
      std::cout << " policy_digest 3  " <<  block_3_result.finalizer_policy_digest << " " <<  block_3_result.computed_finality_digest << " " <<  block_3_result.finality_digest << "\n";
      std::cout << " policy_digest 4  " <<  block_4_result.finalizer_policy_digest << " " <<  block_4_result.computed_finality_digest << " " <<  block_4_result.finality_digest << "\n";
      std::cout << " policy_digest 5  " <<  block_5_result.finalizer_policy_digest << " " <<  block_5_result.computed_finality_digest << " " <<  block_5_result.finality_digest << "\n";
      std::cout << " policy_digest 6  " <<  block_6_result.finalizer_policy_digest << " " <<  block_6_result.computed_finality_digest << " " <<  block_6_result.finality_digest << "\n";
      std::cout << " policy_digest 7  " <<  block_7_result.finalizer_policy_digest << " " <<  block_7_result.computed_finality_digest << " " <<  block_7_result.finality_digest << "\n";
      std::cout << " policy_digest 8  " <<  block_8_result.finalizer_policy_digest << " " <<  block_8_result.computed_finality_digest << " " <<  block_8_result.finality_digest << "\n";
      std::cout << " policy_digest 9  " <<  block_9_result.finalizer_policy_digest << " " <<  block_9_result.computed_finality_digest << " " <<  block_9_result.finality_digest << "\n";
      std::cout << " policy_digest 10 " << block_10_result.finalizer_policy_digest << " " << block_10_result.computed_finality_digest << " " << block_10_result.finality_digest << "\n";
      std::cout << " policy_digest 11 " << block_11_result.finalizer_policy_digest << " " << block_11_result.computed_finality_digest << " " << block_11_result.finality_digest << "\n";
      std::cout << " policy_digest 12 " << block_12_result.finalizer_policy_digest << " " << block_12_result.computed_finality_digest << " " << block_12_result.finality_digest << "\n";
      std::cout << " policy_digest 13 " << block_13_result.finalizer_policy_digest << " " << block_13_result.computed_finality_digest << " " << block_13_result.finality_digest << "\n";
      std::cout << " policy_digest 14 " << block_14_result.finalizer_policy_digest << " " << block_14_result.computed_finality_digest << " " << block_14_result.finality_digest << "\n";
      std::cout << " policy_digest 15 " << block_15_result.finalizer_policy_digest << " " << block_15_result.computed_finality_digest << " " << block_15_result.finality_digest << "\n";
      std::cout << " policy_digest 16 " << block_16_result.finalizer_policy_digest << " " << block_16_result.computed_finality_digest << " " << block_16_result.finality_digest << "\n";
      std::cout << " policy_digest 17 " << block_17_result.finalizer_policy_digest << " " << block_17_result.computed_finality_digest << " " << block_17_result.finality_digest << "\n";
 



      qc_data_t qc_b_8 = extract_qc_data(block_8_result.block);
      qc_data_t qc_b_9 = extract_qc_data(block_9_result.block);
      qc_data_t qc_b_10 = extract_qc_data(block_10_result.block);
      qc_data_t qc_b_11 = extract_qc_data(block_11_result.block);
      qc_data_t qc_b_12 = extract_qc_data(block_12_result.block);
      qc_data_t qc_b_13 = extract_qc_data(block_13_result.block);
      qc_data_t qc_b_14 = extract_qc_data(block_14_result.block);
      qc_data_t qc_b_15 = extract_qc_data(block_15_result.block);
      qc_data_t qc_b_16 = extract_qc_data(block_16_result.block);
      qc_data_t qc_b_17 = extract_qc_data(block_17_result.block);

      BOOST_TEST(qc_b_8.qc.has_value());
      BOOST_TEST(qc_b_9.qc.has_value());
      BOOST_TEST(qc_b_10.qc.has_value());
      BOOST_TEST(qc_b_11.qc.has_value());
      BOOST_TEST(qc_b_12.qc.has_value());
      BOOST_TEST(qc_b_13.qc.has_value());
      BOOST_TEST(qc_b_14.qc.has_value());
      BOOST_TEST(qc_b_15.qc.has_value());
      BOOST_TEST(qc_b_16.qc.has_value());
      BOOST_TEST(qc_b_17.qc.has_value());

      // heavy proof #3. Proving finality of block #11 using block #11 finality root
      mutable_variant_object heavy_proof_3 = mvo()
         ("assert", false)
         ("proof", mvo() 
            ("finality_proof", mvo()
               ("qc_block", mvo()
                  ("major_version", 1)
                  ("minor_version", 0)
                  ("finalizer_policy_generation", 1)
                  ("witness_hash", block_13_result.afp_base_digest)
                  ("finality_mroot", block_13_result.finality_root)
               )
               ("qc", mvo()
                  ("signature", qc_b_14.qc.value().data.sig.to_string())
                  ("finalizers", raw_bitset) 
               )
            )
            ("target_block_proof_of_inclusion", mvo() 
               ("target_node_index", 11)
               ("last_node_index", 11)
               ("target",  mvo() 
                  ("finality_data", mvo() 
                     ("major_version", 1)
                     ("minor_version", 0)
                     ("finalizer_policy_generation", 1)
                     ("witness_hash", block_11_result.afp_base_digest)
                     ("finality_mroot", block_11_result.finality_root)
                  )
                  ("dynamic_data", mvo() 
                     ("block_num", block_11_result.block->block_num())
                     ("action_proofs", fc::variants())
                     ("action_mroot", block_11_result.action_mroot)
                  )
               )
               ("merkle_branches", generate_proof_of_inclusion(cluster.get_finality_leaves(11), 11))
            )
         );

      cluster.node0.push_action( "ibc"_n, "setfpolicy"_n, "ibc"_n, mvo()
         ("from_block_num", 1)
         ("policy", cluster.current_finalizer_policy)
      );

      // heavy proof #4. Proving finality of block #12 using block #12 finality root
      mutable_variant_object heavy_proof_4= mvo()
         ("assert", false)
         ("proof", mvo() 
            ("finality_proof", mvo()
               ("qc_block", mvo()
                  ("major_version", 1)
                  ("minor_version", 0)
                  ("finalizer_policy_generation", 2)
                  ("witness_hash", block_14_result.afp_base_digest)
                  ("finality_mroot", block_14_result.finality_root)
               )
               ("qc", mvo()
                  ("signature", qc_b_15.qc.value().data.sig.to_string())
                  ("finalizers", raw_bitset) 
               )
            )
            ("target_block_proof_of_inclusion", mvo() 
               ("target_node_index", 12)
               ("last_node_index", 12)
               ("target",  mvo() 
                  ("finality_data", mvo() 
                     ("major_version", 1)
                     ("minor_version", 0)
                     ("finalizer_policy_generation", 1)
                     ("witness_hash", block_12_result.afp_base_digest)
                     ("finality_mroot", block_12_result.finality_root)
                  )
                  ("dynamic_data", mvo() 
                     ("block_num", block_12_result.block->block_num())
                     ("action_proofs", fc::variants())
                     ("action_mroot", block_12_result.action_mroot)
                  )
               )
               ("merkle_branches", generate_proof_of_inclusion(cluster.get_finality_leaves(12), 12))
            )
         );


      // verify heavy proof #3
      action_trace check_heavy_proof_3_trace = cluster.node0.push_action("ibc"_n, "checkproof"_n, "ibc"_n, heavy_proof_3)->action_traces[0];

      // verify heavy proof #4
      action_trace check_heavy_proof_4_trace = cluster.node0.push_action("ibc"_n, "checkproof"_n, "ibc"_n, heavy_proof_4)->action_traces[0];

      // We now test light proof we should still be able to verify a proof of finality for block #2 without finality proof,
      // since the previous root is still cached
      cluster.node0.push_action("ibc"_n, "checkproof"_n, "ibc"_n, light_proof_1);

      cluster.produce_ibc_blocks(1200); //advance 10 minutes

      // the root is still cached when performing this action, so the action succeeds.
      // However, it also triggers garbage collection,removing the old proven root for block #2,
      // so subsequent calls with the same action data will fail
      cluster.node0.push_action("ibc"_n, "checkproof"_n, "ibc"_n, light_proof_1);

      cluster.produce_ibc_blocks(1); //advance 1 block to avoid duplicate transaction

      bool failed = false;

      // Since garbage collection was previously triggered for the merkle root of block #2 which this
      // proof attempts to link to, action will now fail
      try {
         cluster.node0.push_action("ibc"_n, "checkproof"_n, "ibc"_n, light_proof_1);
      }
      catch(const eosio_assert_message_exception& e){
         failed = true;
      }

      // verify action has failed, as expected
      BOOST_CHECK(failed); 
      
   } FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
