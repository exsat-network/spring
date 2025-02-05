#pragma once
#include <eosio/chain/block_header.hpp>
#include <eosio/chain/finality/finality_core.hpp>
#include <eosio/chain/protocol_feature_manager.hpp>
#include <eosio/chain/finality/quorum_certificate.hpp>
#include <eosio/chain/finality/finalizer_policy.hpp>
#include <eosio/chain/finality/instant_finality_extension.hpp>
#include <eosio/chain/chain_snapshot.hpp>
#include <future>

namespace eosio::chain {

namespace snapshot_detail {
  struct snapshot_block_state_v7;
}

namespace detail { struct schedule_info; };

// Light header protocol version, separate from protocol feature version
constexpr uint32_t light_header_protocol_version_major = 1;
constexpr uint32_t light_header_protocol_version_minor = 0;

// data for finality_digest
struct finality_digest_data_v1 {
   uint32_t    major_version{light_header_protocol_version_major};
   uint32_t    minor_version{light_header_protocol_version_minor};
   uint32_t    active_finalizer_policy_generation {0};
   uint32_t    final_on_strong_qc_block_num {0};
   digest_type finality_tree_digest;
   digest_type last_pending_finalizer_policy_and_base_digest;
};

// ------------------------------------------------------------------------------------------
// this is used for tracking in-flight `finalizer_policy` changes, which have been requested,
// but are not activated yet. This struct is associated to a block_number in the
// `finalizer_policies` flat_multimap: `block_num => state, finalizer_policy`
//
// When state == proposed, the block_num identifies the block in which the new policy was
// proposed via set_finalizers.
//
// When that block becomes final, according to the block_header_state's finality_core,
// 1. the policy becomes pending
// 2. its key `block_num,` in the proposer_policies multimap, is the current block
//
// When this current block itself becomes final, the policy becomes active.
// ------------------------------------------------------------------------------------------
struct building_block_input {
   block_id_type                     parent_id;
   block_timestamp_type              parent_timestamp;
   block_timestamp_type              timestamp;
   account_name                      producer;
   vector<digest_type>               new_protocol_feature_activations;
};

// this struct can be extracted from a building block
struct block_header_state_input : public building_block_input {
   digest_type                       transaction_mroot;    // Comes from std::get<checksum256_type>(building_block::trx_mroot_or_receipt_digests)
   std::optional<proposer_policy>    new_proposer_policy;  // Comes from building_block::new_proposer_policy
   std::optional<finalizer_policy>   new_finalizer_policy; // Comes from building_block::new_finalizer_policy
   qc_claim_t                        most_recent_ancestor_with_qc; // Comes from traversing branch from parent and calling get_best_qc()
   digest_type                       finality_mroot_claim;
};

struct block_header_state {
   // ------ data members ------------------------------------------------------------
   block_id_type                       block_id;
   block_header                        header;
   protocol_feature_activation_set_ptr activated_protocol_features;

   finality_core                       core;                    // thread safe, not modified after creation

   finalizer_policy_ptr                active_finalizer_policy; // finalizer set + threshold + generation, supports `digest()`
   proposer_policy_ptr                 active_proposer_policy;  // producer authority schedule, supports `digest()`

   // block time when proposer_policy will become active
   // current algorithm only two entries possible, for the next,next round and one for block round after that
   // The active time is the next,next producer round. For example,
   //   round A [1,2,..12], next_round B [1,2,..12], next_next_round C [1,2,..12], D [1,2,..12]
   //   If proposed in A1, A2, .. A12 becomes active in C1
   //   If proposed in B1, B2, .. B12 becomes active in D1
   flat_map<block_timestamp_type, proposer_policy_ptr>     proposer_policies;

   // Track in-flight proposed finalizer policies.
   // When the block associated with a proposed finalizer policy becomes final,
   // it becomes pending.
   std::vector<std::pair<block_num_type, finalizer_policy_ptr>> proposed_finalizer_policies;
   // Track in-flight pending finalizer policy. At most one pending
   // finalizer policy at any moment.
   // When the block associated with the pending finalizer policy becomes final,
   // it becomes active.
   std::optional<std::pair<block_num_type, finalizer_policy_ptr>> pending_finalizer_policy;

   // generation increases by one each time a new finalizer_policy is proposed in a block
   // It matches the finalizer policy generation most recently included in this block's `if_extension` or its ancestors
   uint32_t                            finalizer_policy_generation{1};

   // digest of the finalizer policy (which includes the generation number in it) with the greatest generation number
   // in the history of the blockchain so far that is not in proposed state (so either pending or active state)
   digest_type                         last_pending_finalizer_policy_digest;

   // ------ data members caching information available elsewhere ----------------------
   header_extension_multimap           header_exts;     // redundant with the data stored in header


   // ------ functions -----------------------------------------------------------------
   const block_id_type&  id()             const { return block_id; }
   const digest_type     finality_mroot() const { return header.is_proper_svnn_block() ? header.action_mroot : digest_type{}; }
   block_timestamp_type  timestamp()      const { return header.timestamp; }
   account_name          producer()       const { return header.producer; }
   const block_id_type&  previous()       const { return header.previous; }
   uint32_t              block_num()      const { return block_header::num_from_id(previous()) + 1; }
   block_timestamp_type  last_qc_block_timestamp() const {
      auto last_qc_block_num  = core.latest_qc_claim().block_num;
      return core.get_block_reference(last_qc_block_num).timestamp;
   }
   const producer_authority_schedule& active_schedule_auth()  const { return active_proposer_policy->proposer_schedule; }
   const protocol_feature_activation_set_ptr& get_activated_protocol_features() const { return activated_protocol_features; }

   block_header_state next(block_header_state_input& data) const;
   block_header_state next(const signed_block_header& h, validator_t& validator) const;

   digest_type compute_base_digest() const;
   digest_type compute_finality_digest() const;

   // Returns true if the block is a Savanna Genesis Block.
   // This method is applicable to any transition block which is re-classified as a Savanna block.
   bool is_savanna_genesis_block() const { return core.is_genesis_block_num(block_num()); }

   // Returns true if the block is a Proper Savanna Block
   bool is_proper_svnn_block() const { return header.is_proper_svnn_block(); }

   // block descending from this need the provided qc in the block extension
   bool is_needed(const qc_claim_t& qc_claim) const {
      return qc_claim > core.latest_qc_claim();
   }

   const vector<digest_type>& get_new_protocol_feature_activations() const;
   const producer_authority& get_scheduled_producer(block_timestamp_type t) const;

   const finalizer_policy& get_last_proposed_finalizer_policy() const;
   const finalizer_policy& get_last_pending_finalizer_policy() const;
   const proposer_policy& get_last_proposed_proposer_policy() const;
};

using block_header_state_ptr = std::shared_ptr<block_header_state>;

}

FC_REFLECT( eosio::chain::block_header_state, (block_id)(header)
            (activated_protocol_features)(core)(active_finalizer_policy)
            (active_proposer_policy)(proposer_policies)(proposed_finalizer_policies)
            (pending_finalizer_policy)(finalizer_policy_generation)
            (last_pending_finalizer_policy_digest))

FC_REFLECT( eosio::chain::finality_digest_data_v1, (major_version)(minor_version)(active_finalizer_policy_generation)(final_on_strong_qc_block_num)(finality_tree_digest)(last_pending_finalizer_policy_and_base_digest) )
