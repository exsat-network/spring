#include <eosio/chain/block_header_state.hpp>
#include <eosio/chain/block_header_state_utils.hpp>
#include <eosio/chain/finality/instant_finality_extension.hpp>
#include <eosio/chain/finality/proposer_policy.hpp>
#include <eosio/chain/exceptions.hpp>
#include <limits>

namespace eosio::chain {

// this is a versioning scheme that is separate from protocol features that only
// gets updated if a protocol feature causes a breaking change to light block
// header validation

// compute base_digest explicitly because of pointers involved.
digest_type block_header_state::compute_base_digest() const {
   digest_type::encoder enc;

   fc::raw::pack( enc, header );
   fc::raw::pack( enc, core );

   fc::raw::pack( enc, proposed_finalizer_policies );
   fc::raw::pack( enc, pending_finalizer_policy );

   assert(active_proposer_policy);
   fc::raw::pack( enc, *active_proposer_policy );

   for (const auto& pp_pair : proposer_policies) {
      fc::raw::pack( enc, pp_pair.first );
      assert(pp_pair.second);
      fc::raw::pack( enc, *pp_pair.second );
   }

   if (activated_protocol_features) {
      fc::raw::pack( enc, *activated_protocol_features );
   }

   return enc.result();
}

digest_type block_header_state::compute_finality_digest() const {
   auto base_digest = compute_base_digest();
   std::pair<const digest_type&, const digest_type&> last_pending_and_base{ last_pending_finalizer_policy_digest, base_digest };
   auto lpfp_base_digest = fc::sha256::hash(last_pending_and_base);

   assert(active_finalizer_policy);
   finality_digest_data_v1 finality_digest_data {
      .active_finalizer_policy_generation      = active_finalizer_policy->generation,
      .final_on_strong_qc_block_num            = core.final_on_strong_qc_block_num,
      .finality_tree_digest                    = finality_mroot(),
      .last_pending_finalizer_policy_and_base_digest = lpfp_base_digest
   };

   return fc::sha256::hash(finality_digest_data);
}

const producer_authority& block_header_state::get_scheduled_producer(block_timestamp_type t) const {
   return detail::get_scheduled_producer(active_proposer_policy->proposer_schedule.producers, t);
}

const vector<digest_type>& block_header_state::get_new_protocol_feature_activations()const {
   return detail::get_new_protocol_feature_activations(header_exts);
}

// The last proposed finalizer policy if none proposed or pending then the active finalizer policy
const finalizer_policy& block_header_state::get_last_proposed_finalizer_policy() const {
   if (!proposed_finalizer_policies.empty()) {
       return *proposed_finalizer_policies.back().second;
   } else if (pending_finalizer_policy.has_value()) {
      return *pending_finalizer_policy->second;
   }
   return *active_finalizer_policy;
}

// The last pending finalizer policy if none pending then the active finalizer policy
// Used to populate last_pending_finalizer_policy_digest
const finalizer_policy& block_header_state::get_last_pending_finalizer_policy() const {
   if (pending_finalizer_policy.has_value()) {
      return *pending_finalizer_policy->second;
   }
   return *active_finalizer_policy;
}

// The last proposed proposer policy, if none proposed then the active proposer policy
const proposer_policy& block_header_state::get_last_proposed_proposer_policy() const {
   if (proposer_policies.empty()) {
      assert(active_proposer_policy);
      return *active_proposer_policy;
   }
   auto it = proposer_policies.rbegin();
   assert(it != proposer_policies.rend());
   return *it->second;
}

// This function evaluates possible promotions from pending to active
// and from proposed to pending (removing any proposed policies that are known at that
// time to never become pending)
//
// In particular,
// 1. If there is a pending policy, determine whether it should be promoted to active.
//    If the associated block number is less than or equal to the new LIB number,
//    the pending policy should be promoted to active. This also means the pending slot
//    is now open for a possible promotion of a proposed policy to pending.
//    This guarantees that there will be at most one pending policy at any given time.
// 2. If there is any proposed policy with an associated block number that is less than
//    or equal to the new LIB number:
//  i.   Find the proposed policy with the greatest associated block number that is still
//       less than or equal to the new LIB number (call this the target proposed policy).
//  ii.  Remove any proposed policies with an associated block number less than that of
//       the target proposed policy.
//  iii. If the pending slot is open, promote that target proposed policy to pending.
//       Otherwise, leave the target proposed policy (and any other proposed policies with
//       greater associated block numbers) alone in the proposed policy queue.
//
void evaluate_finalizer_policies_for_promotion(const block_header_state& prev,
                                               block_header_state& next_header_state) {
   // Common case: if no pending policy and no proposed_finalizer_policies to evaluate,
   //              just return
   if (!prev.pending_finalizer_policy.has_value() && prev.proposed_finalizer_policies.empty() ) {
      return;
   }

   auto lib = next_header_state.core.last_final_block_num();
   const auto& prev_pending = prev.pending_finalizer_policy;
   const auto& prev_proposed = prev.proposed_finalizer_policies;
   auto& next_pending = next_header_state.pending_finalizer_policy;
   auto& next_proposed = next_header_state.proposed_finalizer_policies;

   // Evaluate pending first.
   bool pending_slot_open = true;
   if (prev_pending.has_value()) {
      if (prev_pending->first <= lib) {
         // The block associated with the pending has become final, promote pending to active
         next_header_state.active_finalizer_policy = prev_pending->second;
      } else {
         // Pending not yet to become final, copy it to next_header_state
         next_pending = prev_pending;
         pending_slot_open = false;  // no slot openned up
      }
   }

   // Nothing more to do if existing proposed_finalizer_policies is empty
   if (prev_proposed.empty()) {
      return;
   }

   // Find the target proposed policy which is the proposed policy with the greatest
   // associated block number that is less than or equal to the new LIB number
   auto first_reversible = std::ranges::find_if(prev_proposed, [&](const auto& p) { return p.first > lib; });
   auto target = first_reversible > prev_proposed.begin() ? first_reversible - 1 : prev_proposed.end();

   // Promote target policy to pending if the pending slot is available, otherwise
   // copy it to next_proposed
   if (target != prev_proposed.end()) {
      if (pending_slot_open) {
         // promote the target to pending
         auto block_num = next_header_state.block_num();
         next_pending.emplace(block_num, target->second);
      } else {
         // leave the target alone in the proposed policies
         next_proposed.emplace_back(*target);
      }

      if (first_reversible != prev_proposed.end()) {
         // Copy proposed policies with an associated block number greater than that of the target
         // proposed policy (implictly remove any proposed policies with an associated block
         // number less than that of the target proposed policy)
         next_proposed.insert( next_proposed.end(), first_reversible, prev_proposed.end());
      }
   } else {
      // No target proposed policy exists, just copy the previous proposed policies to next
      next_proposed = prev_proposed;
   }
}

// -------------------------------------------------------------------------------------------------
// `finish_next` updates the next `block_header_state` according to the contents of the
// header extensions (either new protocol_features or instant_finality_extension) applicable to this
// next block .
//
// These extensions either result from the execution of the previous block (in case this node
// was the block producer) or were received from the network in a `signed_block`.
// -------------------------------------------------------------------------------------------------
void finish_next(const block_header_state& prev,
                 block_header_state& next_header_state,
                 vector<digest_type> new_protocol_feature_activations,
                 instant_finality_extension if_ext,
                 bool log) { // only log on assembled blocks, to avoid double logging
   // activated protocol features
   // ---------------------------
   if (!new_protocol_feature_activations.empty()) {
      next_header_state.activated_protocol_features = std::make_shared<protocol_feature_activation_set>(
         *prev.activated_protocol_features, std::move(new_protocol_feature_activations));
   } else {
      next_header_state.activated_protocol_features = prev.activated_protocol_features;
   }

   // proposer policy
   // ---------------
   next_header_state.active_proposer_policy = prev.active_proposer_policy;

   if (!prev.proposer_policies.empty()) {
      auto it = prev.proposer_policies.begin();
      // +1 since this is called after the block is built, this will be the active schedule for the next block
      if (it->first.slot <= next_header_state.header.timestamp.slot + 1) {
         next_header_state.active_proposer_policy = it->second;
         next_header_state.proposer_policies = { ++it, prev.proposer_policies.end() };
      } else {
         next_header_state.proposer_policies = prev.proposer_policies;
      }
   }

   std::optional<proposer_policy> new_proposer_policy;
   if (if_ext.new_proposer_policy_diff) {
      new_proposer_policy = prev.get_last_proposed_proposer_policy().apply_diff(*if_ext.new_proposer_policy_diff);
   }
   if (new_proposer_policy) {
      // called when assembling the block
      next_header_state.proposer_policies[new_proposer_policy->active_time] =
         std::make_shared<proposer_policy>(std::move(*new_proposer_policy));
   }

   // finality_core
   // -------------
   block_ref parent_block {
      .block_id  = prev.block_id,
      .timestamp = prev.timestamp(),
      .finalizer_policy_generation = prev.active_finalizer_policy->generation
   };
   next_header_state.core = prev.core.next(parent_block, if_ext.qc_claim);

   // finalizer policy
   // ----------------
   next_header_state.active_finalizer_policy = prev.active_finalizer_policy;

   evaluate_finalizer_policies_for_promotion(prev, next_header_state);

   next_header_state.last_pending_finalizer_policy_digest = fc::sha256::hash(next_header_state.get_last_pending_finalizer_policy());

   if (if_ext.new_finalizer_policy_diff) {
      finalizer_policy new_finalizer_policy = prev.get_last_proposed_finalizer_policy().apply_diff(*if_ext.new_finalizer_policy_diff);

      // a new `finalizer_policy` was proposed in the previous block, and is present in the previous
      // block's header extensions.
      // Add this new proposal to the `proposed_finalizer_policies` which tracks the in-flight proposals,
      // and log that proposal (debug level).
      // ------------------------------------------------------------------------------------------------
      if (log)
         dlog("New finalizer policy proposed in block ${id}..: ${pol}",
              ("id", prev.block_id.str().substr(8,16))("pol", new_finalizer_policy));
      next_header_state.finalizer_policy_generation = new_finalizer_policy.generation;
      next_header_state.proposed_finalizer_policies.emplace_back(
         std::make_pair(next_header_state.block_num(),
                        std::make_shared<finalizer_policy>(std::move(new_finalizer_policy))));
   } else {
      next_header_state.finalizer_policy_generation = prev.finalizer_policy_generation;
   }

   // Finally update block id from header
   // -----------------------------------
   next_header_state.block_id = next_header_state.header.calculate_id();

   // Now that we have the block id of the new block, log what changed.
   // -----------------------------------------------------------------
   if (next_header_state.active_finalizer_policy != prev.active_finalizer_policy) {
      const auto& act = next_header_state.active_finalizer_policy;
      if (log) {
         ilog("Finalizer policy generation change: ${old_gen} -> ${new_gen}",
              ("old_gen", prev.active_finalizer_policy->generation)("new_gen",act->generation));
         ilog("New finalizer policy becoming active in block ${id}: ${pol}",("id", next_header_state.block_id)("pol", *act));
      }
   }
}
   
block_header_state block_header_state::next(block_header_state_input& input) const {
   block_header_state next_header_state;

   // header
   // ------
   next_header_state.header = {
      .timestamp         = input.timestamp,
      .producer          = input.producer,
      .confirmed         = 0,
      .previous          = input.parent_id,
      .transaction_mroot = input.transaction_mroot,
      .action_mroot      = input.finality_mroot_claim,
      .schedule_version  = block_header::proper_svnn_schedule_version
   };

   // finality extension
   // ------------------
   std::optional<finalizer_policy_diff> new_finalizer_policy_diff;
   if (input.new_finalizer_policy) {
      new_finalizer_policy_diff = get_last_proposed_finalizer_policy().create_diff(*input.new_finalizer_policy);
   }
   std::optional<proposer_policy_diff> new_proposer_policy_diff;
   if (input.new_proposer_policy) {
      new_proposer_policy_diff = get_last_proposed_proposer_policy().create_diff(*input.new_proposer_policy);
   }
   instant_finality_extension new_if_ext { input.most_recent_ancestor_with_qc,
                                           std::move(new_finalizer_policy_diff),
                                           std::move(new_proposer_policy_diff) };

   uint16_t if_ext_id = instant_finality_extension::extension_id();
   emplace_extension(next_header_state.header.header_extensions, if_ext_id, fc::raw::pack(new_if_ext));
   next_header_state.header_exts.emplace(if_ext_id, new_if_ext);

   // add protocol_feature_activation extension
   // -----------------------------------------
   if (!input.new_protocol_feature_activations.empty()) {
      uint16_t ext_id = protocol_feature_activation::extension_id();
      protocol_feature_activation pfa_ext{.protocol_features = input.new_protocol_feature_activations};

      emplace_extension(next_header_state.header.header_extensions, ext_id, fc::raw::pack(pfa_ext));
      next_header_state.header_exts.emplace(ext_id, std::move(pfa_ext));
   }

   finish_next(*this, next_header_state, std::move(input.new_protocol_feature_activations), std::move(new_if_ext), true);

   return next_header_state;
}

/**
 *  Transitions the current header state into the next header state given the supplied signed block header.
 *
 *  Given a signed block header, generate the expected template based upon the header time,
 *  then validate that the provided header matches the template.
 */
block_header_state block_header_state::next(const signed_block_header& h, validator_t& validator) const {
   auto producer = detail::get_scheduled_producer(active_proposer_policy->proposer_schedule.producers, h.timestamp).producer_name;
   
   EOS_ASSERT( h.previous == block_id, unlinkable_block_exception,
               "previous mismatch ${p} != ${id}", ("p", h.previous)("id", block_id) );
   EOS_ASSERT( h.producer == producer, wrong_producer, "wrong producer specified" );
   EOS_ASSERT( !h.new_producers, producer_schedule_exception,
               "Block header contains legacy producer schedule outdated by activation of WTMsig Block Signatures" );

   block_header_state next_header_state;
   next_header_state.header = static_cast<const block_header&>(h);
   next_header_state.header_exts = h.validate_and_extract_header_extensions();
   next_header_state.finalizer_policy_generation = finalizer_policy_generation;

   const auto& exts = next_header_state.header_exts;

   // retrieve protocol_feature_activation from incoming block header extension
   // -------------------------------------------------------------------------
   vector<digest_type> new_protocol_feature_activations;
   if( exts.count(protocol_feature_activation::extension_id() > 0) ) {
      auto  pfa_entry = exts.lower_bound(protocol_feature_activation::extension_id());
      auto& pfa_ext   = std::get<protocol_feature_activation>(pfa_entry->second);
      new_protocol_feature_activations = pfa_ext.protocol_features;
      validator( timestamp(), activated_protocol_features->protocol_features, new_protocol_feature_activations );
   }

   // retrieve instant_finality_extension data from block header extension
   // --------------------------------------------------------------------
   EOS_ASSERT(exts.count(instant_finality_extension::extension_id()) > 0, invalid_block_header_extension,
              "Instant Finality Extension is expected to be present in all block headers after switch to IF");
   auto  if_entry     = exts.lower_bound(instant_finality_extension::extension_id());
   const auto& if_ext = std::get<instant_finality_extension>(if_entry->second);

   if (h.is_proper_svnn_block()) {
      // if there is no Finality Tree Root associated with the block,
      // then this needs to validate that h.action_mroot is the empty digest
      auto next_core_metadata = core.next_metadata(if_ext.qc_claim);
      bool no_finality_tree_associated = core.is_genesis_block_num(next_core_metadata.final_on_strong_qc_block_num);

      EOS_ASSERT(no_finality_tree_associated == h.action_mroot.empty(), block_validate_exception,
                 "No Finality Tree Root associated with the block, does not match with empty action_mroot: "
                 "(${n}), action_mroot empty (${e}), final_on_strong_qc_block_num (${f})",
                 ("n", no_finality_tree_associated)("e", h.action_mroot.empty())
                 ("f", next_core_metadata.final_on_strong_qc_block_num));
   };

   finish_next(*this, next_header_state, std::move(new_protocol_feature_activations), if_ext, false);

   return next_header_state;
}

} // namespace eosio::chain

