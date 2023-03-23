// Copyright (c) 2014-2019, The Monero Project
// 
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
// 
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
// 
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
// 
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// 
// Parts of this file are originally copyright (c) 2012-2013 The Cryptonote developers

#include <unordered_set>
#include <random>
#include "include_base_utils.h"
#include "string_tools.h"
using namespace epee;

#include "common/apply_permutation.h"
#include "cryptonote_tx_utils.h"
#include "cryptonote_config.h"
#include "blockchain.h"
#include "cryptonote_basic/miner.h"
#include "cryptonote_basic/tx_extra.h"
#include "crypto/crypto.h"
#include "crypto/hash.h"
#include "ringct/rctSigs.h"
#include "multisig/multisig.h"
#include "offshore/asset_types.h"

#include <boost/multiprecision/cpp_bin_float.hpp>

using namespace crypto;

namespace cryptonote
{
  //---------------------------------------------------------------
  void classify_addresses(const std::vector<tx_destination_entry> &destinations, const boost::optional<cryptonote::account_public_address>& change_addr, size_t &num_stdaddresses, size_t &num_subaddresses, account_public_address &single_dest_subaddress)
  {
    num_stdaddresses = 0;
    num_subaddresses = 0;
    std::unordered_set<cryptonote::account_public_address> unique_dst_addresses;
    for(const tx_destination_entry& dst_entr: destinations)
    {
      if (change_addr && dst_entr.addr == change_addr)
        continue;
      if (unique_dst_addresses.count(dst_entr.addr) == 0)
      {
        unique_dst_addresses.insert(dst_entr.addr);
        if (dst_entr.is_subaddress)
        {
          ++num_subaddresses;
          single_dest_subaddress = dst_entr.addr;
        }
        else
        {
          ++num_stdaddresses;
        }
      }
    }
    LOG_PRINT_L2("destinations include " << num_stdaddresses << " standard addresses and " << num_subaddresses << " subaddresses");
  }

  // Governance code credit to Loki project https://github.com/loki-project/loki
  keypair get_deterministic_keypair_from_height(uint64_t height)
  {
    keypair k;

    ec_scalar& sec = k.sec;

    for (int i=0; i < 8; i++)
    {
      uint64_t height_byte = height & ((uint64_t)0xFF << (i*8));
      uint8_t byte = height_byte >> i*8;
      sec.data[i] = byte;
    }
    for (int i=8; i < 32; i++)
    {
      sec.data[i] = 0x00;
    }

    generate_keys(k.pub, k.sec, k.sec, true);

    return k;
  }

  uint64_t get_governance_reward(uint64_t height, uint64_t base_reward)
  {
    return base_reward / 20;
  }

  bool get_deterministic_output_key(const account_public_address& address, const keypair& tx_key, size_t output_index, crypto::public_key& output_key)
  {

    crypto::key_derivation derivation = AUTO_VAL_INIT(derivation);
    bool r = crypto::generate_key_derivation(address.m_view_public_key, tx_key.sec, derivation);
    CHECK_AND_ASSERT_MES(r, false, "failed to generate_key_derivation(" << address.m_view_public_key << ", " << tx_key.sec << ")");

    r = crypto::derive_public_key(derivation, output_index, address.m_spend_public_key, output_key);
    CHECK_AND_ASSERT_MES(r, false, "failed to derive_public_key(" << derivation << ", "<< address.m_spend_public_key << ")");

    return true;
  }

  bool validate_governance_reward_key(uint64_t height, const std::string& governance_wallet_address_str, size_t output_index, const crypto::public_key& output_key, cryptonote::network_type nettype)
  {
    keypair gov_key = get_deterministic_keypair_from_height(height);

    cryptonote::address_parse_info governance_wallet_address;
    cryptonote::get_account_address_from_str(governance_wallet_address, nettype, governance_wallet_address_str);
    crypto::public_key correct_key;

    if (!get_deterministic_output_key(governance_wallet_address.address, gov_key, output_index, correct_key))
    {
      MERROR("Failed to generate deterministic output key for governance wallet output validation");
      return false;
    }

    return correct_key == output_key;
  }

  std::string get_governance_address(uint32_t version, network_type nettype) {
    if (version >= HF_VERSION_XASSET_FULL) {
      if (nettype == TESTNET) {
        return ::config::testnet::GOVERNANCE_WALLET_ADDRESS_MULTI;
      } else if (nettype == STAGENET) {
        return ::config::stagenet::GOVERNANCE_WALLET_ADDRESS_MULTI;
      } else {
        return ::config::GOVERNANCE_WALLET_ADDRESS_MULTI_NEW;
      }
    } else if (version >= 4) {
      if (nettype == TESTNET) {
        return ::config::testnet::GOVERNANCE_WALLET_ADDRESS_MULTI;
      } else if (nettype == STAGENET) {
        return ::config::stagenet::GOVERNANCE_WALLET_ADDRESS_MULTI;
      } else {
        return ::config::GOVERNANCE_WALLET_ADDRESS_MULTI;
      }
    } else {
      if (nettype == TESTNET) {
        return ::config::testnet::GOVERNANCE_WALLET_ADDRESS;
      } else if (nettype == STAGENET) {
        return ::config::stagenet::GOVERNANCE_WALLET_ADDRESS;
      } else {
        return ::config::GOVERNANCE_WALLET_ADDRESS;
      }
    }
  }
  
  //---------------------------------------------------------------
  bool construct_miner_tx(size_t height, size_t median_weight, uint64_t already_generated_coins, size_t current_block_weight, std::map<std::string, uint64_t> fee_map,  std::map<std::string, uint64_t> offshore_fee_map, std::map<std::string, uint64_t> xasset_fee_map, const account_public_address &miner_address, transaction& tx, const blobdata& extra_nonce, size_t max_outs, uint8_t hard_fork_version, cryptonote::network_type nettype) {
    tx.vin.clear();
    tx.vout.clear();
    tx.extra.clear();
    tx.output_unlock_times.clear();

    keypair txkey = keypair::generate(hw::get_device("default"));
    add_tx_pub_key_to_extra(tx, txkey.pub);
    if(!extra_nonce.empty())
      if(!add_extra_nonce_to_tx_extra(tx.extra, extra_nonce))
        return false;
    if (!sort_tx_extra(tx.extra, tx.extra))
      return false;

    keypair gov_key = get_deterministic_keypair_from_height(height);

    txin_gen in;
    in.height = height;

    uint64_t block_reward;
    if(!get_block_reward(median_weight, current_block_weight, already_generated_coins, block_reward, hard_fork_version))
    {
      LOG_PRINT_L0("Block is too big");
      return false;
    }

#if defined(DEBUG_CREATE_BLOCK_TEMPLATE)
    // NEAC: need to iterate over the currency maps to output all fees
    LOG_PRINT_L1("Creating block template: block reward " << block_reward);
    for (const auto &fee: fee_map) {
      LOG_PRINT_L1("\t" << fee.first << " fee " << fee);
    }
#endif

    uint64_t governance_reward = 0;
    if (hard_fork_version >= 3) {
      if (already_generated_coins != 0)
      {
        governance_reward = get_governance_reward(height, block_reward);
        block_reward -= governance_reward;
      }
    }

    block_reward += fee_map["XHV"];
    uint64_t summary_amounts = 0;
    crypto::key_derivation derivation = AUTO_VAL_INIT(derivation);;
    crypto::public_key out_eph_public_key = AUTO_VAL_INIT(out_eph_public_key);
    bool r = crypto::generate_key_derivation(miner_address.m_view_public_key, txkey.sec, derivation);
    CHECK_AND_ASSERT_MES(r, false, "while creating outs: failed to generate_key_derivation(" << miner_address.m_view_public_key << ", " << txkey.sec << ")");
    r = crypto::derive_public_key(derivation, 0, miner_address.m_spend_public_key, out_eph_public_key);
    CHECK_AND_ASSERT_MES(r, false, "while creating outs: failed to derive_public_key(" << derivation << ", " << "0" << ", "<< miner_address.m_spend_public_key << ")");

    txout_to_key tk;
    tk.key = out_eph_public_key;

    tx_out out;
    summary_amounts += out.amount = block_reward;
    out.target = tk;
    tx.vout.push_back(out);

    // add governance wallet output for xhv
    cryptonote::address_parse_info governance_wallet_address;
    if (hard_fork_version >= 3) {
      if (already_generated_coins != 0)
      {
        add_tx_pub_key_to_extra(tx, gov_key.pub);
        cryptonote::get_account_address_from_str(governance_wallet_address, nettype, get_governance_address(hard_fork_version, nettype));
        crypto::public_key out_eph_public_key = AUTO_VAL_INIT(out_eph_public_key);
        if (!get_deterministic_output_key(governance_wallet_address.address, gov_key, 1 /* second output in miner tx */, out_eph_public_key))
        {
          MERROR("Failed to generate deterministic output key for governance wallet output creation");
          return false;
        }

        txout_to_key tk;
        tk.key = out_eph_public_key;
        tx_out out;
        summary_amounts += out.amount = governance_reward;
        if (hard_fork_version >= HF_VERSION_OFFSHORE_FULL) {
          out.amount += offshore_fee_map["XHV"];
        }

        out.target = tk;
        tx.vout.push_back(out);
        CHECK_AND_ASSERT_MES(summary_amounts == (block_reward + governance_reward), false, "Failed to construct miner tx, summary_amounts = " << summary_amounts << " not equal total block_reward = " << (block_reward + governance_reward));
      }
    }

    if (hard_fork_version >= HF_VERSION_OFFSHORE_FULL) {
      // Add all of the outputs for all of the currencies in the contained TXs
      uint64_t idx = 2;
      for (auto &fee_map_entry: fee_map) {
        // Skip XHV - we have already handled that above
        if (fee_map_entry.first == "XHV")
          continue;
    
        if (fee_map_entry.second != 0) {
          uint64_t block_reward_xasset = fee_map_entry.second;
          uint64_t governance_reward_xasset = 0;
          governance_reward_xasset = get_governance_reward(height, fee_map_entry.second);
          block_reward_xasset -= governance_reward_xasset;

          // Add the conversion fee to the governance payment (if provided)
          if (offshore_fee_map[fee_map_entry.first] != 0) {
            governance_reward_xasset += offshore_fee_map[fee_map_entry.first];
          }
          
          // handle xasset converion fees
          if (hard_fork_version >= HF_VERSION_XASSET_FEES_V2) {
            if (xasset_fee_map[fee_map_entry.first] != 0) {
              if (hard_fork_version >= HF_VERSION_USE_COLLATERAL) {
                // we got 1.5% from xasset conversions.
                // 80% of it(1.2% of the inital value) goes to governance wallet
                // 20% of it(0.3% of the inital value) goes to miners
                boost::multiprecision::uint128_t fee = xasset_fee_map[fee_map_entry.first];
                // 80%
                governance_reward_xasset += (uint64_t)((fee * 4) / 5);
                // 20%
                block_reward_xasset += (uint64_t)(fee /5);
              } else {
                // we got 0.5% from xasset conversions. Here we wanna burn 80%(0.4% of the initial whole) of it and 
                // spilit the rest between governance wallet and the miner
                uint64_t fee = xasset_fee_map[fee_map_entry.first];
                // burn 80%
                fee -= (fee * 4) / 5;
                // split the rest
                block_reward_xasset += fee / 2;
                governance_reward_xasset += fee / 2;
              }
            }
          }

          // Miner component of the xAsset TX fee
          r = crypto::derive_public_key(derivation, idx, miner_address.m_spend_public_key, out_eph_public_key);
          CHECK_AND_ASSERT_MES(r, false, "while creating outs: failed to derive_public_key(" << derivation << ", " << idx << ", "<< miner_address.m_spend_public_key << ")");
          idx++;

          if (fee_map_entry.first == "XUSD") {
            // Offshore TX
            txout_offshore tk_off;
            tk_off.key = out_eph_public_key;
            
            tx_out out_off;
            out_off.amount = block_reward_xasset;
            out_off.target = tk_off;
            tx.vout.push_back(out_off);
          } else {
            // xAsset TX
            txout_xasset tk_off;
            tk_off.key = out_eph_public_key;
            tk_off.asset_type = fee_map_entry.first;
            
            tx_out out_off;
            out_off.amount = block_reward_xasset;
            out_off.target = tk_off;
            tx.vout.push_back(out_off);
          }

          crypto::public_key out_eph_public_key_xasset = AUTO_VAL_INIT(out_eph_public_key_xasset);
          if (!get_deterministic_output_key(governance_wallet_address.address, gov_key, idx /* n'th output in miner tx */, out_eph_public_key_xasset))
          {
            MERROR("Failed to generate deterministic output key for governance wallet output creation (2)");
            return false;
          }
          idx++;

          if (fee_map_entry.first == "XUSD") {
            // Offshore TX
            txout_offshore tk_gov;
            tk_gov.key = out_eph_public_key_xasset;
            
            tx_out out_gov;
            out_gov.amount = governance_reward_xasset;
            out_gov.target = tk_gov;
            tx.vout.push_back(out_gov);
          } else {
            // xAsset TX
            txout_xasset tk_gov;
            tk_gov.key = out_eph_public_key_xasset;
            tk_gov.asset_type = fee_map_entry.first;
            
            tx_out out_gov;
            out_gov.amount = governance_reward_xasset;
            out_gov.target = tk_gov;
            tx.vout.push_back(out_gov);
          }
        }
      }
    }

    // set tx version
    if (hard_fork_version >= HF_VERSION_USE_COLLATERAL ) {
      tx.version = COLLATERAL_TRANSACTION_VERSION;
    } else if (hard_fork_version >= HF_PER_OUTPUT_UNLOCK_VERSION) {
      tx.version = POU_TRANSACTION_VERSION;
    } else if (hard_fork_version >= HF_VERSION_HAVEN2) {
      tx.version = 5;
    } else if (hard_fork_version >= HF_VERSION_XASSET_FEES_V2) {
      tx.version = 4;
    } else if (hard_fork_version >= HF_VERSION_OFFSHORE_FULL) {
      tx.version = 3;
    } else {
      tx.version = 2;
    }

    //lock
    tx.unlock_time = height + CRYPTONOTE_MINED_MONEY_UNLOCK_WINDOW;
    tx.vin.push_back(in);
    tx.invalidate_hashes();

    // per-output-unlock times
    if (hard_fork_version >= HF_PER_OUTPUT_UNLOCK_VERSION)
      for (size_t i=0; i<tx.vout.size(); i++)
        tx.output_unlock_times.push_back(tx.unlock_time);
    
    //LOG_PRINT("MINER_TX generated ok, block_reward=" << print_money(block_reward) << "("  << print_money(block_reward - fee) << "+" << print_money(fee)
    //  << "), current_block_size=" << current_block_size << ", already_generated_coins=" << already_generated_coins << ", tx_id=" << get_transaction_hash(tx), LOG_LEVEL_2);
    return true;
  }
  //---------------------------------------------------------------
  crypto::public_key get_destination_view_key_pub(const std::vector<tx_destination_entry> &destinations, const boost::optional<cryptonote::account_public_address>& change_addr)
  {
    account_public_address addr = {null_pkey, null_pkey};
    size_t count = 0;
    bool found_change = false;
    for (const auto &i : destinations)
    {
      if (i.amount == 0 && i.amount_usd == 0 && i.amount_xasset == 0)
        continue;
      if (change_addr && i.addr == *change_addr && !found_change)
        continue;
      if (i.addr == addr)
        continue;
      if (count > 0)
        return null_pkey;
      addr = i.addr;
      ++count;
    }
    if (count == 0 && change_addr)
      return change_addr->m_view_public_key;
    return addr.m_view_public_key;
  }
  //---------------------------------------------------------------
  uint64_t get_offshore_fee(const std::vector<cryptonote::tx_destination_entry>& dsts, const uint32_t unlock_time, const uint32_t hf_version) {

    // Calculate the amount being sent
    uint64_t amount = 0;
    for (auto dt: dsts) {
      // Filter out the change, which is never converted
      if (dt.amount_usd != 0 && !dt.is_collateral) {
        amount += dt.amount;
      }
    }

    uint64_t fee_estimate = 0;
    if (hf_version >= HF_VERSION_USE_COLLATERAL) {
      // Flat 1.5% fee
      fee_estimate = (amount * 3) / 200;
    } else if (hf_version >= HF_PER_OUTPUT_UNLOCK_VERSION) {
      // Flat 0.5% fee
      fee_estimate = amount / 200;
    } else {
      // The tests have to be written largest unlock_time first, as it is possible to delay the construction of the TX using GDB etc
      // which would otherwise cause the umlock_time to fall through the gaps and give a minimum fee for a short unlock_time.
      // This way, the code is safe, and the fee is always correct.
      fee_estimate =
      (unlock_time >= 5040) ? (amount / 500) :
      (unlock_time >= 1440) ? (amount / 20) :
      (unlock_time >= 720) ? (amount / 10) :
      amount / 5;
    }

    return fee_estimate;
  }
  //---------------------------------------------------------------
  uint64_t get_onshore_fee(const std::vector<cryptonote::tx_destination_entry>& dsts, const uint32_t unlock_time, const uint32_t hf_version) {

    // Calculate the amount being sent
    uint64_t amount_usd = 0;
    for (auto dt: dsts) {
      // Filter out the change, which is never converted
      if (dt.amount != 0 && !dt.is_collateral) {
        amount_usd += dt.amount_usd;
      }
    }

    uint64_t fee_estimate = 0;
     if (hf_version >= HF_VERSION_USE_COLLATERAL) {
      // Flat 1.5% fee
      fee_estimate = (amount_usd * 3) / 200;
    } else if (hf_version >= HF_PER_OUTPUT_UNLOCK_VERSION) {
      // Flat 0.5% fee
      fee_estimate = amount_usd / 200;
    } else {
      // The tests have to be written largest unlock_time first, as it is possible to delay the construction of the TX using GDB etc
      // which would otherwise cause the umlock_time to fall through the gaps and give a minimum fee for a short unlock_time.
      // This way, the code is safe, and the fee is always correct.
      fee_estimate =
      (unlock_time >= 5040) ? (amount_usd / 500) :
      (unlock_time >= 1440) ? (amount_usd / 20) :
      (unlock_time >= 720) ? (amount_usd / 10) :
      amount_usd / 5;

    }

    return fee_estimate;
  }
  //---------------------------------------------------------------
  uint64_t get_xasset_to_xusd_fee(const std::vector<cryptonote::tx_destination_entry>& dsts, const uint32_t hf_version) {

    // Calculate the amount being sent
    uint64_t amount_xasset = 0;
    for (auto dt: dsts) {
      // Filter out the change, which is never converted
      if (dt.amount_usd != 0) {
        amount_xasset += dt.amount_xasset;
      }
    }

    uint64_t fee_estimate = 0;  
    if (hf_version >= HF_VERSION_USE_COLLATERAL) {
      // Calculate 1.5% of the total being sent
      boost::multiprecision::uint128_t amount_128 = amount_xasset;
      amount_128 = (amount_128 * 15) / 1000; // 1.5%
      fee_estimate  = (uint64_t)amount_128;
    } else if (hf_version >= HF_VERSION_XASSET_FEES_V2) {
      // Calculate 0.5% of the total being sent
      boost::multiprecision::uint128_t amount_128 = amount_xasset;
      amount_128 = (amount_128 * 5) / 1000; // 0.5%
      fee_estimate  = (uint64_t)amount_128;
    } else {
      // Calculate 0.3% of the total being sent
      boost::multiprecision::uint128_t amount_128 = amount_xasset;
      amount_128 = (amount_128 * 3) / 1000;
      fee_estimate = (uint64_t)amount_128;
    }

   return fee_estimate;
  }
  //---------------------------------------------------------------
  uint64_t get_xusd_to_xasset_fee(const std::vector<cryptonote::tx_destination_entry>& dsts, const uint32_t hf_version) {

    // Calculate the amount being sent
    uint64_t amount_usd = 0;
    for (auto dt: dsts) {
      // Filter out the change, which is never converted
      // All other destinations should have both pre and post converted amounts set so far except
      // the change destinations.
      if (dt.amount_xasset != 0) {
        amount_usd += dt.amount_usd;
      }
    }

    uint64_t fee_estimate = 0;
    if (hf_version >= HF_VERSION_USE_COLLATERAL) {
      // Calculate 1.5% of the total being sent
      boost::multiprecision::uint128_t amount_128 = amount_usd;
      amount_128 = (amount_128 * 15) / 1000; // 1.5%
      fee_estimate  = (uint64_t)amount_128;
    } else if (hf_version >= HF_VERSION_XASSET_FEES_V2) {
      // Calculate 0.5% of the total being sent
      boost::multiprecision::uint128_t amount_128 = amount_usd;
      amount_128 = (amount_128 * 5) / 1000; // 0.5%
      fee_estimate  = (uint64_t)amount_128;
    } else {
      // Calculate 0.3% of the total being sent
      boost::multiprecision::uint128_t amount_128 = amount_usd;
      amount_128 = (amount_128 * 3) / 1000;
      fee_estimate = (uint64_t)amount_128;
    }

    return fee_estimate;
  }
  //---------------------------------------------------------------
  bool get_tx_asset_types(const transaction& tx, const crypto::hash &txid, std::string& source, std::string& destination, const bool is_miner_tx) {

    // Clear the source
    std::set<std::string> source_asset_types;
    source = "";
    for (size_t i = 0; i < tx.vin.size(); i++) {
      if (tx.vin[i].type() == typeid(txin_gen)) {
        if (!is_miner_tx) {
          LOG_ERROR("txin_gen detected in non-miner TX. Rejecting..");
          return false;
        }
        source_asset_types.insert("XHV");
      } else if (tx.vin[i].type() == typeid(txin_to_key)) {
        source_asset_types.insert("XHV");
      } else if (tx.vin[i].type() == typeid(txin_offshore)) {
        source_asset_types.insert("XUSD");
      } else if (tx.vin[i].type() == typeid(txin_onshore)) {
        source_asset_types.insert("XUSD");
      } else if (tx.vin[i].type() == typeid(txin_xasset)) {
        std::string xasset = boost::get<txin_xasset>(tx.vin[i]).asset_type;
        if (xasset == "XHV" || xasset == "XUSD") {
          LOG_ERROR("XHV or XUSD found in a xasset input. Rejecting..");
          return false;
        }
        source_asset_types.insert(xasset);
      } else {
        LOG_ERROR("txin_to_script / txin_to_scripthash detected. Rejecting..");
        return false;
      }
    }

    std::vector<std::string> sat;
    sat.reserve(source_asset_types.size());
    std::copy(source_asset_types.begin(), source_asset_types.end(), std::back_inserter(sat));
    
    // Sanity check that we only have 1 source asset type
    if (tx.version >= COLLATERAL_TRANSACTION_VERSION && sat.size() == 2) {
      // this is only possible for an onshore tx.
      if ((sat[0] == "XHV" && sat[1] == "XUSD") || (sat[0] == "XUSD" && sat[1] == "XHV")) {
        source = "XUSD";
      } else {
        LOG_ERROR("Impossible input asset types. Rejecting..");
        return false;
      }
    } else {
      if (sat.size() != 1) {
        LOG_ERROR("Multiple Source Asset types detected. Rejecting..");
        return false;
      }
      source = sat[0];
    }
    
    // Clear the destination
    std::set<std::string> destination_asset_types;
    destination = "";
    for (const auto &out: tx.vout) {
      if (out.target.type() == typeid(txout_to_key)) {
        destination_asset_types.insert("XHV");
      } else if (out.target.type() == typeid(txout_offshore)) {
        destination_asset_types.insert("XUSD");
      } else if (out.target.type() == typeid(txout_xasset)) {
        std::string xasset = boost::get<txout_xasset>(out.target).asset_type;
        if (xasset == "XHV" || xasset == "XUSD") {
          LOG_ERROR("XHV or XUSD found in a xasset output. Rejecting..");
          return false;
        }
        destination_asset_types.insert(xasset);
      } else {
        LOG_ERROR("txout_to_script / txout_to_scripthash detected. Rejecting..");
        return false;
      }
    }

    std::vector<std::string> dat;
    dat.reserve(destination_asset_types.size());
    std::copy(destination_asset_types.begin(), destination_asset_types.end(), std::back_inserter(dat));
    
    // Check that we have at least 1 destination_asset_type
    if (!dat.size()) {
      LOG_ERROR("No supported destinations asset types detected. Rejecting..");
      return false;
    }
    
    // Handle miner_txs differently - full validation is performed in validate_miner_transaction()
    if (is_miner_tx) {
      destination = "XHV";
    } else {
    
      // Sanity check that we only have 1 or 2 destination asset types
      if (dat.size() > 2) {
        LOG_ERROR("Too many (" << dat.size() << ") destination asset types detected in non-miner TX. Rejecting..");
        return false;
      } else if (dat.size() == 1) {
        if (sat.size() != 1) {
          LOG_ERROR("Impossible input asset types. Rejecting..");
          return false;
        }
        if (dat[0] != source) {
          LOG_ERROR("Conversion without change detected ([" << source << "] -> [" << dat[0] << "]). Rejecting..");
          return false;
        }
        destination = dat[0];
      } else {
        if (sat.size() == 2) {
          if (!((dat[0] == "XHV" && dat[1] == "XUSD") || (dat[0] == "XUSD" && dat[1] == "XHV"))) {
            LOG_ERROR("Impossible input asset types. Rejecting..");
            return false;
          }
        }
        if (dat[0] == source) {
          destination = dat[1];
        } else if (dat[1] == source) {
          destination = dat[0];
        } else {
          LOG_ERROR("Conversion outputs are incorrect asset types (source asset type not found - [" << source << "] -> [" << dat[0] << "," << dat[1] << "]). Rejecting..");
          return false;
        }
      }
    }
    
    // check both strSource and strDest are supported.
    if (std::find(offshore::ASSET_TYPES.begin(), offshore::ASSET_TYPES.end(), source) == offshore::ASSET_TYPES.end()) {
      LOG_ERROR("Source Asset type " << source << " is not supported! Rejecting..");
      return false;
    }
    if (std::find(offshore::ASSET_TYPES.begin(), offshore::ASSET_TYPES.end(), destination) == offshore::ASSET_TYPES.end()) {
      LOG_ERROR("Destination Asset type " << destination << " is not supported! Rejecting..");
      return false;
    }

    // Check for the 3 known exploited TXs that converted XJPY to XBTC
    const std::vector<std::string> exploit_txs = {"4c87e7245142cb33a8ed4f039b7f33d4e4dd6b541a42a55992fd88efeefc40d1",
                                                  "7089a8faf5bddf8640a3cb41338f1ec2cdd063b1622e3b27923e2c1c31c55418",
                                                  "ad5d15085594b8f2643f058b05931c3e60966128b4c33298206e70bdf9d41c22"};

    std::string tx_hash = epee::string_tools::pod_to_hex(txid);
    if (std::find(exploit_txs.begin(), exploit_txs.end(), tx_hash) != exploit_txs.end()) {
      destination = "XJPY";
    }
    return true;
  }
  //---------------------------------------------------------------
  bool get_tx_type(const std::string& source, const std::string& destination, transaction_type& type) {

    // check both source and destination are supported.
    if (std::find(offshore::ASSET_TYPES.begin(), offshore::ASSET_TYPES.end(), source) == offshore::ASSET_TYPES.end()) {
      LOG_ERROR("Source Asset type " << source << " is not supported! Rejecting..");
      return false;
    }
    if (std::find(offshore::ASSET_TYPES.begin(), offshore::ASSET_TYPES.end(), destination) == offshore::ASSET_TYPES.end()) {
      LOG_ERROR("Destination Asset type " << destination << " is not supported! Rejecting..");
      return false;
    }

    // Find the tx type
    if (source == destination) {
      if (source == "XHV") {
        type = transaction_type::TRANSFER;
      } else if (source == "XUSD") {
        type = transaction_type::OFFSHORE_TRANSFER;
      } else {
        type = transaction_type::XASSET_TRANSFER;
      }
    } else {
      if (source == "XHV" && destination == "XUSD") {
        type = transaction_type::OFFSHORE;
      } else if (source == "XUSD" && destination == "XHV") {
        type = transaction_type::ONSHORE;
      } else if (source == "XUSD" && destination != "XHV") {
        type = transaction_type::XUSD_TO_XASSET;
      } else if (destination == "XUSD" && source != "XHV") {
        type = transaction_type::XASSET_TO_XUSD;
      } else {
        LOG_ERROR("Invalid conversion from " << source << "to" << destination << ". Rejecting..");
        return false;
      }
    }

    // Return success to caller
    return true;
  }
  //---------------------------------------------------------------
  bool get_collateral_requirements(const transaction_type &tx_type, const uint64_t amount, uint64_t &collateral, const offshore::pricing_record &pr, const std::vector<std::pair<std::string, std::string>> &amounts)
  {
    using namespace boost::multiprecision;
    using tt = transaction_type;

    // Process the circulating supply data
    std::map<std::string, uint128_t> map_amounts;
    uint128_t mcap_xassets = 0;
    for (const auto &i: amounts)
    {
      // Copy into the map for expediency
      map_amounts[i.first] = uint128_t(i.second.c_str());
      
      // Skip XHV
      if (i.first == "XHV") continue;

      // Get the pricing data for the xAsset
      uint128_t price_xasset = pr[i.first];
      
      // Multiply by the amount of coin in circulation
      uint128_t amount_xasset(i.second.c_str());
      amount_xasset *= COIN;
      amount_xasset /= price_xasset;
      
      // Sum into our total for all xAssets
      mcap_xassets += amount_xasset;
    }

    // Calculate the XHV market cap
    boost::multiprecision::uint128_t price_xhv =
      (tx_type == tt::OFFSHORE) ? std::min(pr.unused1, pr.xUSD) :
      (tx_type == tt::ONSHORE)  ? std::max(pr.unused1, pr.xUSD) :
      0;
    uint128_t mcap_xhv = map_amounts["XHV"];
    mcap_xhv *= price_xhv;
    mcap_xhv /= COIN;

    // Calculate the market cap ratio
    cpp_bin_float_quad ratio_mcap_128 = mcap_xassets.convert_to<cpp_bin_float_quad>() / mcap_xhv.convert_to<cpp_bin_float_quad>();
    double ratio_mcap = ratio_mcap_128.convert_to<double>();

    // Calculate the spread ratio
    double ratio_spread = (ratio_mcap >= 1.0) ? 0.0 : 1.0 - ratio_mcap;
    
    // Calculate the MCAP VBS rate
    double rate_mcvbs = (ratio_mcap == 0) ? 0 : (ratio_mcap < 0.9) // Fix for "possible" 0 ratio
      ? std::exp((ratio_mcap + std::sqrt(ratio_mcap))*2.0) - 0.5 // Lower MCAP ratio
      : std::sqrt(ratio_mcap) * 40.0; // Higher MCAP ratio

    // Calculate the Spread Ratio VBS rate
    double rate_srvbs = std::exp(1 + std::sqrt(ratio_spread)) + rate_mcvbs + 1.5;
    
    // Set the Slippage Multiplier
    double slippage_multiplier = 10.0;

    // Convert amount to 128 bit
    boost::multiprecision::uint128_t amount_128 = amount;
  
    // Do the right thing based upon TX type
    using tt = cryptonote::transaction_type;
    if (tx_type == tt::TRANSFER || tx_type == tt::OFFSHORE_TRANSFER || tx_type == tt::XASSET_TRANSFER) {
      collateral = 0;
    } else if (tx_type == tt::OFFSHORE) {

      // Calculate MCRI
      boost::multiprecision::uint128_t amount_usd_128 = amount;
      amount_usd_128 *= price_xhv;
      amount_usd_128 /= COIN;
      cpp_bin_float_quad ratio_mcap_new_quad = ((amount_usd_128.convert_to<cpp_bin_float_quad>() + mcap_xassets.convert_to<cpp_bin_float_quad>()) /
						(mcap_xhv.convert_to<cpp_bin_float_quad>() - amount_usd_128.convert_to<cpp_bin_float_quad>()));
      double ratio_mcap_new = ratio_mcap_new_quad.convert_to<double>();
      double ratio_mcri = (ratio_mcap == 0.0) ? ratio_mcap_new : (ratio_mcap_new / ratio_mcap) - 1.0;
      ratio_mcri = std::abs(ratio_mcri);

      // Calculate Offshore Slippage VBS rate
      if (ratio_mcap_new <= 0.1) slippage_multiplier = 3.0;
      double rate_offsvbs = std::sqrt(ratio_mcri) * slippage_multiplier;

      // Calculate the combined VBS (collateral + "slippage")
      double vbs = rate_mcvbs + rate_offsvbs;
      const double min_vbs = 1.0;
      vbs = std::max(vbs, min_vbs);
      vbs = std::floor(vbs);
      vbs *= COIN;
      boost::multiprecision::uint128_t collateral_128 = static_cast<uint64_t>(vbs);
      collateral_128 *= amount_128;
      collateral_128 /= COIN;
      collateral = collateral_128.convert_to<uint64_t>();

      LOG_PRINT_L1("Offshore TX requires " << print_money(collateral) << " XHV as collateral to convert " << print_money(amount) << " XHV");
    
    } else if (tx_type == tt::ONSHORE) {

      // Calculate SRI
      cpp_bin_float_quad ratio_mcap_new_quad = ((mcap_xassets.convert_to<cpp_bin_float_quad>() - amount_128.convert_to<cpp_bin_float_quad>()) /
						(mcap_xhv.convert_to<cpp_bin_float_quad>() + amount_128.convert_to<cpp_bin_float_quad>()));
      double ratio_mcap_new = ratio_mcap_new_quad.convert_to<double>();
      double ratio_sri = (ratio_mcap == 0.0) ? (-1.0 * ratio_mcap_new) : ((1.0 - ratio_mcap_new) / (1.0 - ratio_mcap)) - 1.0;
      ratio_sri = std::max(ratio_sri, 0.0);
      
      // Calculate ONSVBS
      //if (ratio_mcap_new <= 0.1) slippage_multiplier = 3.0;
      //double rate_onsvbs = std::sqrt(ratio_sri) * slippage_multiplier;
      double rate_onsvbs = std::sqrt(ratio_sri) * 3.0;
  
      // Calculate the combined VBS (collateral + "slippage")
      double vbs = std::max(rate_mcvbs, rate_srvbs) + rate_onsvbs;
      const double min_vbs = 1.0;
      vbs = std::max(vbs, min_vbs);
      vbs = std::floor(vbs);
      vbs *= COIN;
      boost::multiprecision::uint128_t collateral_128 = static_cast<uint64_t>(vbs);
      collateral_128 *= amount_128;
      collateral_128 /= price_xhv;
      collateral = collateral_128.convert_to<uint64_t>();

      boost::multiprecision::uint128_t amount_usd_128 = amount;
      amount_usd_128 *= price_xhv;
      amount_usd_128 /= COIN;
      LOG_PRINT_L1("Onshore TX requires " << print_money(collateral) << " XHV as collateral to convert " << print_money((uint64_t)amount_128) << " xUSD");
    
    } else if (tx_type == tt::XUSD_TO_XASSET || tx_type == tt::XASSET_TO_XUSD) {
      collateral = 0;
    } else {
      // Throw a wallet exception - should never happen
      MERROR("Invalid TX type");
      return false;
    }

    return true;
  }
  //---------------------------------------------------------------
  uint64_t get_block_cap(const std::vector<std::pair<std::string, std::string>>& supply_amounts, const offshore::pricing_record& pr)
  {
    std::string str_xhv_supply;
    for (const auto& supply: supply_amounts) {
      if (supply.first == "XHV") {
        str_xhv_supply = supply.second;
        break;
      }
    }

    // get supply
    boost::multiprecision::uint128_t xhv_supply_128(str_xhv_supply);
    xhv_supply_128 /= COIN;
    uint64_t xhv_supply = xhv_supply_128.convert_to<uint64_t>();

    // get price
    double price = (double)(std::min(pr.unused1, pr.xUSD)); // smaller of the ma vs spot
    price /= COIN;

    // market cap
    uint64_t xhv_market_cap = xhv_supply * price;
    
    return (pow(xhv_market_cap * 3000, 0.42) + ((xhv_supply * 5) / 1000)) * COIN;
  }
  //---------------------------------------------------------------
  uint64_t get_xasset_amount(const uint64_t xusd_amount, const std::string& to_asset_type, const offshore::pricing_record& pr)
  {
    boost::multiprecision::uint128_t xusd_128 = xusd_amount;
    boost::multiprecision::uint128_t exchange_128 = pr[to_asset_type]; 
    // Now work out the amount
    boost::multiprecision::uint128_t xasset_128 = xusd_128 * exchange_128;
    xasset_128 /= 1000000000000;

    return (uint64_t)xasset_128;
  }
  //---------------------------------------------------------------
  uint64_t get_xusd_amount(const uint64_t amount, const std::string& amount_asset_type, const offshore::pricing_record& pr, const transaction_type tx_type, uint32_t hf_version)
  {

    if (amount_asset_type == "XUSD") {
      return amount;
    }

    boost::multiprecision::uint128_t amount_128 = amount;
    boost::multiprecision::uint128_t exchange_128 = pr[amount_asset_type];
    if (amount_asset_type == "XHV") {
      // xhv -> xusd
      if (hf_version >= HF_PER_OUTPUT_UNLOCK_VERSION) {
        if (tx_type == transaction_type::ONSHORE) {
          // Eliminate MA/spot advantage for onshore conversion
          exchange_128 = std::max(pr.unused1, pr.xUSD);
        } else {
          // Eliminate MA/spot advantage for offshore conversion
          exchange_128 = std::min(pr.unused1, pr.xUSD);
        }
      }
      boost::multiprecision::uint128_t xusd_128 = amount_128 * exchange_128;
      xusd_128 /= 1000000000000;
      return (uint64_t)xusd_128;
    } else {
      // xasset -> xusd
      boost::multiprecision::uint128_t xusd_128 = amount_128 * 1000000000000;
      xusd_128 /= exchange_128;
      return (uint64_t)xusd_128;
    }
  }
  //---------------------------------------------------------------
  uint64_t get_xhv_amount(const uint64_t xusd_amount, const offshore::pricing_record& pr, const transaction_type tx_type, uint32_t hf_version)
  {
    // Now work out the amount
    boost::multiprecision::uint128_t xusd_128 = xusd_amount;
    boost::multiprecision::uint128_t exchange_128 = pr.unused1;
    boost::multiprecision::uint128_t xhv_128 = xusd_128 * 1000000000000;
    if (hf_version >= HF_PER_OUTPUT_UNLOCK_VERSION) {
      if (tx_type == transaction_type::ONSHORE) {
        // Eliminate MA/spot advantage for onshore conversion
        exchange_128 = std::max(pr.unused1, pr.xUSD);
      } else {
        // Eliminate MA/spot advantage for offshore conversion
        exchange_128 = std::min(pr.unused1, pr.xUSD);
      }
    }
    xhv_128 /= exchange_128;
    return (uint64_t)xhv_128;
  }
  //----------------------------------------------------------------------------------------------------
  bool tx_pr_height_valid(const uint64_t current_height, const uint64_t pr_height, const crypto::hash& tx_hash) {
    if (pr_height >= current_height) {
      return false;
    }
    if ((current_height - PRICING_RECORD_VALID_BLOCKS) > pr_height) {
      // exception for 1 tx that used 11 block old record and is already in the chain.
      if (epee::string_tools::pod_to_hex(tx_hash) != "3e61439c9f751a56777a1df1479ce70311755b9d42db5bcbbd873c6f09a020a6") {
        return false;
      }
    }
    return true;
  }

  //---------------------------------------------------------------
  bool construct_tx_with_tx_key(
    const account_keys& sender_account_keys, 
    const std::unordered_map<crypto::public_key, subaddress_index>& subaddresses, 
    std::vector<tx_source_entry>& sources,
    std::vector<tx_destination_entry>& destinations, 
    const boost::optional<cryptonote::account_public_address>& change_addr, 
    const std::vector<uint8_t> &extra, 
    transaction& tx,
    transaction_type tx_type,
    const std::string strSource,
    const std::string strDest,
    uint64_t unlock_time, 
    const crypto::secret_key &tx_key, 
    const std::vector<crypto::secret_key> &additional_tx_keys, 
    const uint64_t current_height, 
    const offshore::pricing_record& pr,
    uint32_t hf_version,
    const uint64_t onshore_col_amount,
    bool rct, 
    const rct::RCTConfig &rct_config, 
    rct::multisig_out *msout, 
    bool shuffle_outs
  ){

    hw::device &hwdev = sender_account_keys.get_device();

    if (sources.empty())
    {
      LOG_ERROR("Empty sources");
      return false;
    }

    std::vector<rct::key> amount_keys;
    tx.set_null();
    amount_keys.clear();
    if (msout)
    {
      msout->c.clear();
    }

    if (hf_version >= HF_VERSION_USE_COLLATERAL) {
      tx.version = COLLATERAL_TRANSACTION_VERSION;
    } else if (hf_version >= HF_PER_OUTPUT_UNLOCK_VERSION){
      tx.version = POU_TRANSACTION_VERSION;
    } else if (hf_version >= HF_VERSION_HAVEN2) {
      tx.version = 5;
    } else if (hf_version >= HF_VERSION_XASSET_FEES_V2) {
      tx.version = 4;
    } else if (hf_version >= HF_VERSION_CLSAG) {
      tx.version = 3;
    } else {
      tx.version = 2;
    }
    tx.unlock_time = unlock_time;
    tx.extra = extra;

    // check both strSource and strDest are supported.
    if (std::find(offshore::ASSET_TYPES.begin(), offshore::ASSET_TYPES.end(), strSource) == offshore::ASSET_TYPES.end()) {
      LOG_ERROR("Unsupported source asset type " << strSource);
      return false;
    }
    if (std::find(offshore::ASSET_TYPES.begin(), offshore::ASSET_TYPES.end(), strDest) == offshore::ASSET_TYPES.end()) {
      LOG_ERROR("Unsupported destination asset type " << strDest);
      return false;
    }
    if (tx_type == transaction_type::UNSET) {
      LOG_ERROR("Invalid TX Type!");
      return false;
    }

    const bool use_offshore_outputs = (strSource == "XUSD");
    const bool use_xasset_outputs = (strSource != "XHV" && strSource != "XUSD");
    if (strSource != strDest) {
      tx.pricing_record_height = current_height;
    } else {
      tx.pricing_record_height = 0;
    }

    // if we have a stealth payment id, find it and encrypt it with the tx key now
    std::vector<tx_extra_field> tx_extra_fields;
    if (parse_tx_extra(tx.extra, tx_extra_fields))
    {
      bool add_dummy_payment_id = true;
      tx_extra_nonce extra_nonce;
      if (find_tx_extra_field_by_type(tx_extra_fields, extra_nonce))
      {
        crypto::hash payment_id = null_hash;
        crypto::hash8 payment_id8 = null_hash8;
        if (get_encrypted_payment_id_from_tx_extra_nonce(extra_nonce.nonce, payment_id8))
        {
          LOG_PRINT_L2("Encrypting payment id " << payment_id8);
          crypto::public_key view_key_pub = get_destination_view_key_pub(destinations, change_addr);
          if (view_key_pub == null_pkey)
          {
            LOG_ERROR("Destinations have to have exactly one output to support encrypted payment ids");
            return false;
          }

          if (!hwdev.encrypt_payment_id(payment_id8, view_key_pub, tx_key))
          {
            LOG_ERROR("Failed to encrypt payment id");
            return false;
          }

          std::string extra_nonce;
          set_encrypted_payment_id_to_tx_extra_nonce(extra_nonce, payment_id8);
          remove_field_from_tx_extra(tx.extra, typeid(tx_extra_nonce));
          if (!add_extra_nonce_to_tx_extra(tx.extra, extra_nonce))
          {
            LOG_ERROR("Failed to add encrypted payment id to tx extra");
            return false;
          }
          LOG_PRINT_L1("Encrypted payment ID: " << payment_id8);
          add_dummy_payment_id = false;
        }
        else if (get_payment_id_from_tx_extra_nonce(extra_nonce.nonce, payment_id))
        {
          add_dummy_payment_id = false;
        }
      }

      // we don't add one if we've got more than the usual 1 destination plus change
      if (destinations.size() > 2)
        add_dummy_payment_id = false;

      if (add_dummy_payment_id)
      {
        // if we have neither long nor short payment id, add a dummy short one,
        // this should end up being the vast majority of txes as time goes on
        std::string extra_nonce;
        crypto::hash8 payment_id8 = null_hash8;
        crypto::public_key view_key_pub = get_destination_view_key_pub(destinations, change_addr);
        if (view_key_pub == null_pkey)
        {
          LOG_ERROR("Failed to get key to encrypt dummy payment id with");
        }
        else
        {
          hwdev.encrypt_payment_id(payment_id8, view_key_pub, tx_key);
          set_encrypted_payment_id_to_tx_extra_nonce(extra_nonce, payment_id8);
          if (!add_extra_nonce_to_tx_extra(tx.extra, extra_nonce))
          {
            LOG_ERROR("Failed to add dummy encrypted payment id to tx extra");
            // continue anyway
          }
        }
      }
    }
    else
    {
      MWARNING("Failed to parse tx extra");
      tx_extra_fields.clear();
    }
    
    struct input_generation_context_data
    {
      keypair in_ephemeral;
    };
    std::vector<input_generation_context_data> in_contexts;

    uint64_t summary_inputs_money = 0, summary_inputs_money_usd = 0, summary_inputs_money_xasset = 0;
    //fill inputs
    int idx = -1;
    for(const tx_source_entry& src_entr:  sources)
    {
      ++idx;
      if(src_entr.real_output >= src_entr.outputs.size())
      {
        LOG_ERROR("real_output index (" << src_entr.real_output << ")bigger than output_keys.size()=" << src_entr.outputs.size());
        return false;
      }

      if (src_entr.asset_type == "XHV" && (hf_version < HF_VERSION_USE_COLLATERAL || tx_type != transaction_type::ONSHORE)) {
        summary_inputs_money += src_entr.amount;
      } else if (src_entr.asset_type == "XUSD") {
        summary_inputs_money_usd += src_entr.amount;
      } else {
        summary_inputs_money_xasset += src_entr.amount;
      }
      
      //key_derivation recv_derivation;
      in_contexts.push_back(input_generation_context_data());
      keypair& in_ephemeral = in_contexts.back().in_ephemeral;
      crypto::key_image img;
      const auto& out_key = reinterpret_cast<const crypto::public_key&>(src_entr.outputs[src_entr.real_output].second.dest);
      if(!generate_key_image_helper(sender_account_keys, subaddresses, out_key, src_entr.real_out_tx_key, src_entr.real_out_additional_tx_keys, src_entr.real_output_in_tx_index, in_ephemeral,img, hwdev))
      {
        LOG_ERROR("Key image generation failed!");
        return false;
      }

      //check that derivated key is equal with real output key (if non multisig)
      if(!msout && !(in_ephemeral.pub == src_entr.outputs[src_entr.real_output].second.dest) )
      {
        LOG_ERROR("derived public key mismatch with output public key at index " << idx << ", real out " << src_entr.real_output << "! "<< ENDL << "derived_key:"
          << string_tools::pod_to_hex(in_ephemeral.pub) << ENDL << "real output_public_key:"
          << string_tools::pod_to_hex(src_entr.outputs[src_entr.real_output].second.dest) );
        LOG_ERROR("amount " << src_entr.amount << ", rct " << src_entr.rct);
        LOG_ERROR("tx pubkey " << src_entr.real_out_tx_key << ", real_output_in_tx_index " << src_entr.real_output_in_tx_index);
        return false;
      }

      //put key image into tx input
      if (tx_type == transaction_type::OFFSHORE_TRANSFER || tx_type == transaction_type::XUSD_TO_XASSET) { // input is xUSD

        // In-wallet swap
        txin_offshore input_to_key;
        input_to_key.amount = src_entr.amount;
        input_to_key.k_image = msout ? rct::rct2ki(src_entr.multisig_kLRki.ki) : img;
        
        //fill outputs array and use relative offsets
        for(const tx_source_entry::output_entry& out_entry: src_entr.outputs)
          input_to_key.key_offsets.push_back(out_entry.first);
        
        input_to_key.key_offsets = absolute_output_offsets_to_relative(input_to_key.key_offsets);
        tx.vin.push_back(input_to_key);
	
      } else if (tx_type == transaction_type::ONSHORE) {   // input is xUSD

        if (src_entr.asset_type == "XUSD") {
          // Onshoring
          txin_onshore input_to_key;
          input_to_key.amount = src_entr.amount;
          input_to_key.k_image = msout ? rct::rct2ki(src_entr.multisig_kLRki.ki) : img;
          
          //fill outputs array and use relative offsets
          for(const tx_source_entry::output_entry& out_entry: src_entr.outputs)
            input_to_key.key_offsets.push_back(out_entry.first);
          
          input_to_key.key_offsets = absolute_output_offsets_to_relative(input_to_key.key_offsets);
          tx.vin.push_back(input_to_key);
        } else if (src_entr.asset_type == "XHV") {
          // colleteral input
          txin_to_key input_to_key;
          input_to_key.amount = src_entr.amount;
          input_to_key.k_image = msout ? rct::rct2ki(src_entr.multisig_kLRki.ki) : img;
          
          //fill outputs array and use relative offsets
          for(const tx_source_entry::output_entry& out_entry: src_entr.outputs)
            input_to_key.key_offsets.push_back(out_entry.first);
          
          input_to_key.key_offsets = absolute_output_offsets_to_relative(input_to_key.key_offsets);
          tx.vin.push_back(input_to_key);
        } else {
          LOG_ERROR("unsupported input asset for onshore " << src_entr.asset_type);
          return false;
        }
        
      } else if (tx_type == transaction_type::XASSET_TO_XUSD || tx_type == transaction_type::XASSET_TRANSFER) {  // input is xAsset

        // xAsset to xUSD
        txin_xasset input_to_key;
        input_to_key.amount = src_entr.amount;
        input_to_key.k_image = msout ? rct::rct2ki(src_entr.multisig_kLRki.ki) : img;
        input_to_key.asset_type = src_entr.asset_type;
        
        //fill outputs array and use relative offsets
        for(const tx_source_entry::output_entry& out_entry: src_entr.outputs)
          input_to_key.key_offsets.push_back(out_entry.first);
        
        input_to_key.key_offsets = absolute_output_offsets_to_relative(input_to_key.key_offsets);
        tx.vin.push_back(input_to_key);
	
      } else {

        // NEAC - bOffshoreTx doesn't matter if it's an OFFSHORE TX - the IN will still be txin_to_key
        txin_to_key input_to_key;
        input_to_key.amount = src_entr.amount;
        input_to_key.k_image = msout ? rct::rct2ki(src_entr.multisig_kLRki.ki) : img;
        
        //fill outputs array and use relative offsets
        for(const tx_source_entry::output_entry& out_entry: src_entr.outputs)
          input_to_key.key_offsets.push_back(out_entry.first);
        
        input_to_key.key_offsets = absolute_output_offsets_to_relative(input_to_key.key_offsets);
        tx.vin.push_back(input_to_key);
      }
    }

    // calculate offshore fees before shuffling destinations
    uint64_t fee = 0;
    uint64_t offshore_fee = 
      (tx_type == transaction_type::OFFSHORE) ? get_offshore_fee(destinations, unlock_time - current_height - 1, hf_version) :
      (tx_type == transaction_type::ONSHORE) ? get_onshore_fee(destinations, unlock_time - current_height - 1, hf_version) :
      (tx_type == transaction_type::XUSD_TO_XASSET) ? get_xusd_to_xasset_fee(destinations, hf_version) :
      (tx_type == transaction_type::XASSET_TO_XUSD) ? get_xasset_to_xusd_fee(destinations, hf_version) : 0;

    if (shuffle_outs)
    {
      std::shuffle(destinations.begin(), destinations.end(), crypto::random_device{});
    }

    // sort ins by their key image
    std::vector<size_t> ins_order(sources.size());
    for (size_t n = 0; n < sources.size(); ++n)
      ins_order[n] = n;
    std::sort(ins_order.begin(), ins_order.end(), [&](const size_t i0, const size_t i1) {
      if (tx_type == transaction_type::OFFSHORE_TRANSFER || tx_type == transaction_type::XUSD_TO_XASSET) {
        const txin_offshore &tk0 = boost::get<txin_offshore>(tx.vin[i0]);
        const txin_offshore &tk1 = boost::get<txin_offshore>(tx.vin[i1]);
        return memcmp(&tk0.k_image, &tk1.k_image, sizeof(tk0.k_image)) > 0;
      } else if (tx_type == transaction_type::ONSHORE) {
        crypto::key_image tk0_img = (tx.vin[i0].type() == typeid(txin_to_key) ?  boost::get<txin_to_key>(tx.vin[i0]).k_image :  boost::get<txin_onshore>(tx.vin[i0]).k_image);
        crypto::key_image tk1_img = (tx.vin[i1].type() == typeid(txin_to_key) ?  boost::get<txin_to_key>(tx.vin[i1]).k_image :  boost::get<txin_onshore>(tx.vin[i1]).k_image);
        return memcmp(&tk0_img, &tk1_img, sizeof(tk0_img)) > 0;
      } else if (tx_type == transaction_type::XASSET_TO_XUSD || tx_type == transaction_type::XASSET_TRANSFER) {
        const txin_xasset &tk0 = boost::get<txin_xasset>(tx.vin[i0]);
        const txin_xasset &tk1 = boost::get<txin_xasset>(tx.vin[i1]);
        return memcmp(&tk0.k_image, &tk1.k_image, sizeof(tk0.k_image)) > 0;
      } else {
        const txin_to_key &tk0 = boost::get<txin_to_key>(tx.vin[i0]);
        const txin_to_key &tk1 = boost::get<txin_to_key>(tx.vin[i1]);
        return memcmp(&tk0.k_image, &tk1.k_image, sizeof(tk0.k_image)) > 0;
      }
    });
    tools::apply_permutation(ins_order, [&] (size_t i0, size_t i1) {
      std::swap(tx.vin[i0], tx.vin[i1]);
      std::swap(in_contexts[i0], in_contexts[i1]);
      std::swap(sources[i0], sources[i1]);
    });

    // figure out if we need to make additional tx pubkeys
    size_t num_stdaddresses = 0;
    size_t num_subaddresses = 0;
    account_public_address single_dest_subaddress;
    classify_addresses(destinations, change_addr, num_stdaddresses, num_subaddresses, single_dest_subaddress);

    // if this is a single-destination transfer to a subaddress, we set the tx pubkey to R=s*D
    crypto::public_key txkey_pub;
    if (num_stdaddresses == 0 && num_subaddresses == 1)
    {
      txkey_pub = rct::rct2pk(hwdev.scalarmultKey(rct::pk2rct(single_dest_subaddress.m_spend_public_key), rct::sk2rct(tx_key)));
    }
    else
    {
      txkey_pub = rct::rct2pk(hwdev.scalarmultBase(rct::sk2rct(tx_key)));
    }
    remove_field_from_tx_extra(tx.extra, typeid(tx_extra_pub_key));
    add_tx_pub_key_to_extra(tx, txkey_pub);

    std::vector<crypto::public_key> additional_tx_public_keys;

    // we don't need to include additional tx keys if:
    //   - all the destinations are standard addresses
    //   - there's only one destination which is a subaddress
    bool need_additional_txkeys = num_subaddresses > 0 && (num_stdaddresses > 0 || num_subaddresses > 1);
    if (need_additional_txkeys)
      CHECK_AND_ASSERT_MES(destinations.size() == additional_tx_keys.size(), false, "Wrong amount of additional tx keys");

    uint64_t summary_outs_money = 0, summary_outs_money_usd = 0, summary_outs_money_xasset = 0;


    std::vector<std::pair<std::string, std::pair<uint64_t, bool>>> outamounts;
    rct::keyV destination_keys;
    uint64_t amount_out = 0;

    //fill outputs
    tx.amount_minted = tx.amount_burnt = 0;
    size_t output_index = 0;
    bool found_change = false;

    for(const tx_destination_entry& dst_entr: destinations)
    {
      CHECK_AND_ASSERT_MES(dst_entr.amount > 0 || tx.version > 1, false, "Destination with wrong amount: " << dst_entr.amount);
      crypto::public_key out_eph_public_key;

      tx_destination_entry dst_entr_clone = dst_entr;
      hwdev.generate_output_ephemeral_keys(
        tx.version,sender_account_keys,
        txkey_pub,
        tx_key,
        dst_entr_clone,
        change_addr,
        output_index,
        need_additional_txkeys,
        additional_tx_keys,
        additional_tx_public_keys,
        amount_keys,
        out_eph_public_key
      );

      tx_out out;
      out.amount = dst_entr_clone.amount;

      if (dst_entr_clone.asset_type == "XHV") {
        txout_to_key tk;
        tk.key = out_eph_public_key;
        out.target = tk;
        outamounts.push_back(std::pair<std::string, std::pair<uint64_t,bool>>("XHV", {dst_entr_clone.amount, dst_entr_clone.is_collateral}));
      } else if (dst_entr_clone.asset_type == "XUSD") {
        txout_offshore tk;
        tk.key = out_eph_public_key;
        out.target = tk;
        out.amount = dst_entr_clone.amount_usd;
        outamounts.push_back(std::pair<std::string, std::pair<uint64_t,bool>>("XUSD", {dst_entr_clone.amount_usd, dst_entr_clone.is_collateral}));
      } else {
        txout_xasset tk;
        tk.key = out_eph_public_key;
        tk.asset_type = dst_entr_clone.asset_type;
        out.target = tk;
        out.amount = dst_entr_clone.amount_xasset;
        outamounts.push_back(std::pair<std::string, std::pair<uint64_t,bool>>(dst_entr_clone.asset_type, {dst_entr_clone.amount_xasset, dst_entr_clone.is_collateral}));
      }

      // Check for per-output-unlocks
      if (hf_version >= HF_PER_OUTPUT_UNLOCK_VERSION && strSource != strDest) {

        // initialize collateral indices vector
        if (hf_version >= HF_VERSION_USE_COLLATERAL && tx.collateral_indices.size() != 2) {
          tx.collateral_indices.resize(2);
          tx.collateral_indices[0] = 0;
          tx.collateral_indices[1] = 0;
        }

        // set unlcok times and indiviual collateral indeces.
        if (dst_entr_clone.asset_type == strDest) {
          // Destination amount - needs a full unlock time
          if (hf_version >= HF_VERSION_USE_COLLATERAL && tx_type == transaction_type::ONSHORE && dst_entr_clone.is_collateral) {
            // lock collateral ouput full and change as 0
            if (dst_entr_clone.amount == onshore_col_amount) {
              tx.output_unlock_times.push_back(tx.unlock_time);
              tx.collateral_indices[0] = output_index;
            } else {
              tx.output_unlock_times.push_back(0);
              tx.collateral_indices[1] = output_index;
            }
          } else {
            tx.output_unlock_times.push_back(tx.unlock_time);
          }
        } else if (dst_entr_clone.asset_type == strSource) {
          if (hf_version >= HF_VERSION_USE_COLLATERAL && tx_type == transaction_type::OFFSHORE && dst_entr_clone.is_collateral) {
            // Collateral output - needs a full unlock time
            // offshores doesnt have collaterasl change since it is merged with actual change.
            tx.output_unlock_times.push_back(tx.unlock_time);
            tx.collateral_indices[0] = output_index;
          } else {
            // Source amount - unlock time can be shorter ("0" means "minimum allowed" = 10 blocks unlock)
            tx.output_unlock_times.push_back(0);
          }
        } else {
          // Should never happen
          LOG_ERROR("Invalid asset type detected: source = " << strSource << ", dest = " << strDest << ", detected " << dst_entr_clone.asset_type);
          return false;
        }
      } else {
        if (tx.unlock_time - current_height > CRYPTONOTE_DEFAULT_TX_SPENDABLE_AGE)
          tx.output_unlock_times.push_back(tx.unlock_time);
        else
          tx.output_unlock_times.push_back(0);
      }
      
      // push to outputs
      tx.vout.push_back(out);
      output_index++;

      // calculate total money, exclude the onshore collateral
      if (tx_type != transaction_type::ONSHORE || !dst_entr_clone.is_collateral) {
        summary_outs_money += dst_entr_clone.amount;
        summary_outs_money_usd += dst_entr_clone.amount_usd;
        summary_outs_money_xasset += dst_entr_clone.amount_xasset;
      }
      if (strSource != strDest) {
        if (dst_entr_clone.asset_type == strDest && !dst_entr_clone.is_collateral) {
          tx.amount_minted += out.amount;
          if (tx_type == transaction_type::OFFSHORE) {
            tx.amount_burnt += dst_entr_clone.amount;
          } else if (tx_type == transaction_type::ONSHORE || tx_type == transaction_type::XUSD_TO_XASSET) {
            tx.amount_burnt += dst_entr_clone.amount_usd;
          } else if (tx_type == transaction_type::XASSET_TO_XUSD) {
            tx.amount_burnt += dst_entr_clone.amount_xasset;
          }
        }
      }

      destination_keys.push_back(rct::pk2rct(out_eph_public_key));
    }
    CHECK_AND_ASSERT_MES(additional_tx_public_keys.size() == additional_tx_keys.size(), false, "Internal error creating additional public keys");

    remove_field_from_tx_extra(tx.extra, typeid(tx_extra_additional_pub_keys));

    LOG_PRINT_L2("tx pubkey: " << txkey_pub);
    if (need_additional_txkeys)
    {
      LOG_PRINT_L2("additional tx pubkeys: ");
      for (size_t i = 0; i < additional_tx_public_keys.size(); ++i)
        LOG_PRINT_L2(additional_tx_public_keys[i]);
      add_additional_tx_pub_keys_to_extra(tx.extra, additional_tx_public_keys);
    }

    if (!sort_tx_extra(tx.extra, tx.extra)) {
      LOG_ERROR("Failed to sort_tx_extra");
      return false;
    }

    // Add 80% of the conversion fee to the amount burnt
    if (hf_version >= HF_VERSION_XASSET_FEES_V2 && (tx_type == transaction_type::XUSD_TO_XASSET || tx_type == transaction_type::XASSET_TO_XUSD)) {
      if (hf_version < HF_VERSION_USE_COLLATERAL) {
        tx.amount_burnt += (offshore_fee * 4) / 5;
      }
    }
    
    //check money
    LOG_ERROR("SIM=" << summary_inputs_money);
    LOG_ERROR("SIMu=" << summary_inputs_money_usd);
    LOG_ERROR("SIMX=" << summary_inputs_money_xasset);
    LOG_ERROR("SOM=" << summary_outs_money);
    LOG_ERROR("SOMu=" << summary_outs_money_usd);
    LOG_ERROR("SOMX=" << summary_outs_money_xasset);
    CHECK_AND_ASSERT_MES(summary_inputs_money < HAVEN_MAX_TX_VALUE, false, "XHV inputs are too much");
    CHECK_AND_ASSERT_MES(summary_inputs_money_usd < HAVEN_MAX_TX_VALUE, false, "xUSD inputs are too much");
    CHECK_AND_ASSERT_MES(summary_inputs_money_xasset < HAVEN_MAX_TX_VALUE, false, "xAsset inputs are too much");
    CHECK_AND_ASSERT_MES(summary_outs_money < HAVEN_MAX_TX_VALUE, false, "XHV outputs are too much");
    CHECK_AND_ASSERT_MES(summary_outs_money_usd < HAVEN_MAX_TX_VALUE, false, "xUSD outputs are too much");
    CHECK_AND_ASSERT_MES(summary_outs_money_xasset < HAVEN_MAX_TX_VALUE, false, "xAsset outputs are too much");
    
    // check for watch only wallet
    bool zero_secret_key = true;
    for (size_t i = 0; i < sizeof(sender_account_keys.m_spend_secret_key); ++i)
      zero_secret_key &= (sender_account_keys.m_spend_secret_key.data[i] == 0);
    if (zero_secret_key)
    {
      MDEBUG("Null secret key, skipping signatures");
    }

    rct::ctkeyV inSk;
    inSk.reserve(sources.size());
    // mixRing indexing is done the other way round for simple
    rct::ctkeyM mixRing(sources.size());
    std::vector<uint64_t> inamounts;
    std::vector<size_t> inamounts_col_indices;
    std::vector<unsigned int> index;
    std::vector<rct::multisig_kLRki> kLRki;
    for (size_t i = 0; i < sources.size(); ++i)
    {
      rct::ctkey ctkey;
      rct::ctkeyV ctkeyV; // LA

      if (sources[i].asset_type == "XHV" && tx_type == transaction_type::ONSHORE && hf_version >= HF_VERSION_USE_COLLATERAL) {
        inamounts_col_indices.push_back(i);
      }
      
      inamounts.push_back(sources[i].amount);
      index.push_back(sources[i].real_output);
      // inSk: (secret key, mask)
      ctkey.dest = rct::sk2rct(in_contexts[i].in_ephemeral.sec);
      ctkey.mask = sources[i].mask;
      ctkeyV.push_back(ctkey);
      inSk.push_back(ctkey);
      memwipe(&ctkey, sizeof(rct::ctkey));
      // inPk: (public key, commitment)
      // will be done when filling in mixRing
      if (msout)
      {
        kLRki.push_back(sources[i].multisig_kLRki);
      }
    }

    // mixRing indexing is done the other way round for simple
    for (size_t i = 0; i < sources.size(); ++i)
    {
      mixRing[i].resize(sources[i].outputs.size());
      for (size_t n = 0; n < sources[i].outputs.size(); ++n)
      {
        mixRing[i][n] = sources[i].outputs[n].second;
      }
    }

    if (summary_inputs_money > summary_outs_money) {
      fee = summary_inputs_money - summary_outs_money - offshore_fee;
    } else if (summary_inputs_money_usd > summary_outs_money_usd) {
      fee = summary_inputs_money_usd - summary_outs_money_usd - offshore_fee;
    } else if (summary_inputs_money_xasset > summary_outs_money_xasset) {
      fee = summary_inputs_money_xasset - summary_outs_money_xasset - offshore_fee;
    }

    // zero out all amounts to mask rct outputs, real amounts are now encrypted
    for (size_t i = 0; i < tx.vin.size(); ++i)
    {
      if (sources[i].rct) {
        if (tx.vin[i].type() == typeid(txin_offshore)) {
          boost::get<txin_offshore>(tx.vin[i]).amount = 0;
        }
        else if (tx.vin[i].type() == typeid(txin_onshore)) {
          boost::get<txin_onshore>(tx.vin[i]).amount = 0;
        }
        else if (tx.vin[i].type() == typeid(txin_xasset)) {
          boost::get<txin_xasset>(tx.vin[i]).amount = 0;
        }
        else {
          boost::get<txin_to_key>(tx.vin[i]).amount = 0;
        }
      }
    }

    // zero out destination amounts
    for (size_t i = 0; i < tx.vout.size(); ++i) {
      tx.vout[i].amount = 0;
    }

    if ((strSource != strDest) && (!tx.amount_burnt || !tx.amount_minted)) {
      LOG_ERROR("Invalid offshore TX - amount too small (<1 ATOMIC_UNIT)");
      return false;
    }
    
    // HERE BE DRAGONS!!!
    // NEAC: Convert the fees for conversions to XHV
    if (hf_version >= HF_VERSION_BULLETPROOF_PLUS) {

      // New Haven TXs only from BP+ - convert fees to XHV
      switch(tx_type) {
      case transaction_type::ONSHORE:
        //fee = cryptonote::get_xhv_amount(fee, pr, transaction_type::ONSHORE, hf_version);
        offshore_fee = cryptonote::get_xhv_amount(offshore_fee, pr, transaction_type::ONSHORE, hf_version);
        break;
      case transaction_type::XUSD_TO_XASSET:
        //fee = cryptonote::get_xhv_amount(fee, pr, transaction_type::ONSHORE, hf_version);
        offshore_fee = cryptonote::get_xhv_amount(offshore_fee, pr, transaction_type::ONSHORE, hf_version);
        break;
      case transaction_type::XASSET_TO_XUSD:
        //fee = cryptonote::get_xusd_amount(fee, source_asset, pr, transaction_type::XASSET_TO_XUSD, hf_version);
        //fee = cryptonote::get_xhv_amount(fee, pr, transaction_type::ONSHORE, hf_version);
        offshore_fee = cryptonote::get_xusd_amount(offshore_fee, strSource, pr, transaction_type::XASSET_TO_XUSD, hf_version);
        offshore_fee = cryptonote::get_xhv_amount(offshore_fee, pr, transaction_type::ONSHORE, hf_version);
        break;
      default:
        break;
      }
    }
    // LAND AHOY!!!
      
    crypto::hash tx_prefix_hash;
    get_transaction_prefix_hash(tx, tx_prefix_hash, hwdev);
    rct::ctkeyV outSk;
    tx.rct_signatures = rct::genRctSimple(
      rct::hash2rct(tx_prefix_hash),
      inSk,
      destination_keys,
      inamounts,
      inamounts_col_indices,
      onshore_col_amount,
      strSource,
      outamounts,
      fee,
      offshore_fee,
      mixRing,
      amount_keys,
      msout ? &kLRki : NULL,
      msout,
      index,
      outSk,
      rct_config,
      hwdev,
      pr,
      tx.version
    );
    for (size_t i=0; i<inSk.size(); i++) {
      memwipe(&inSk[i], sizeof(rct::ctkeyV));
    }

    CHECK_AND_ASSERT_MES(tx.vout.size() == outSk.size(), false, "outSk size does not match vout");
    MCINFO("construct_tx", "transaction_created: " << get_transaction_hash(tx) << ENDL << obj_to_json_str(tx) << ENDL);
    tx.invalidate_hashes();

    return true;
  }
  //---------------------------------------------------------------
  bool construct_tx_and_get_tx_key(
    const account_keys& sender_account_keys,
    const std::unordered_map<crypto::public_key, subaddress_index>& subaddresses,
    std::vector<tx_source_entry>& sources,
    std::vector<tx_destination_entry>& destinations,
    const boost::optional<cryptonote::account_public_address>& change_addr,
    const std::vector<uint8_t> &extra,
    transaction& tx,
    transaction_type tx_type,
    const std::string strSource,
    const std::string strDest,
    uint64_t unlock_time,
    crypto::secret_key &tx_key,
    std::vector<crypto::secret_key> &additional_tx_keys,
    const uint64_t current_height,
    const offshore::pricing_record& pr,
    uint32_t hf_version,
    const uint64_t onshore_col_amount,
    bool rct,
    const rct::RCTConfig &rct_config,
    rct::multisig_out *msout
  ){

    hw::device &hwdev = sender_account_keys.get_device();
    hwdev.open_tx(tx_key);
    try {
      // figure out if we need to make additional tx pubkeys
      size_t num_stdaddresses = 0;
      size_t num_subaddresses = 0;
      account_public_address single_dest_subaddress;
      classify_addresses(destinations, change_addr, num_stdaddresses, num_subaddresses, single_dest_subaddress);
      bool need_additional_txkeys = num_subaddresses > 0 && (num_stdaddresses > 0 || num_subaddresses > 1);
      if (need_additional_txkeys)
      {
        additional_tx_keys.clear();
        for (const auto &d: destinations)
          additional_tx_keys.push_back(keypair::generate(sender_account_keys.get_device()).sec);
      }

      bool r = construct_tx_with_tx_key(
        sender_account_keys,
        subaddresses,
        sources,
        destinations,
        change_addr,
        extra, 
        tx,
        tx_type,
        strSource,
        strDest,
        unlock_time,
        tx_key,
        additional_tx_keys,
        current_height,
        pr,
        hf_version,
        onshore_col_amount,
        rct,
        rct_config,
        msout,
        true
      );
      hwdev.close_tx();
      return r;
    } catch(...) {
      hwdev.close_tx();
      throw;
    }
  }
  //---------------------------------------------------------------
  bool generate_genesis_block(
      block& bl
    , std::string const & genesis_tx
    , uint32_t nonce
    , cryptonote::network_type nettype
    )
  {
    //genesis block
    bl = {};
    account_public_address ac = boost::value_initialized<account_public_address>();
    std::vector<size_t> sz;
    std::map<std::string, uint64_t> fee_map, offshore_fee_map, xasset_fee_map;
    construct_miner_tx(0, 0, 0, 0, fee_map, offshore_fee_map, xasset_fee_map, ac, bl.miner_tx, blobdata(), 999, 1, nettype); // zero fee in genesis
    blobdata txb = tx_to_blob(bl.miner_tx);
    std::string hex_tx_represent = string_tools::buff_to_hex_nodelimer(txb);

    std::string genesis_coinbase_tx_hex = config::GENESIS_TX;

    blobdata tx_bl;
    bool r = string_tools::parse_hexstr_to_binbuff(genesis_coinbase_tx_hex, tx_bl);
    CHECK_AND_ASSERT_MES(r, false, "failed to parse coinbase tx from hard coded blob");
    r = parse_and_validate_tx_from_blob(tx_bl, bl.miner_tx);
    CHECK_AND_ASSERT_MES(r, false, "failed to parse coinbase tx from hard coded blob");
    bl.major_version = CURRENT_BLOCK_MAJOR_VERSION;
    bl.minor_version = CURRENT_BLOCK_MINOR_VERSION;
    bl.timestamp = 0;
    bl.nonce = nonce;
    miner::find_nonce_for_given_block([](const cryptonote::block &b, uint64_t height, unsigned int threads, crypto::hash &hash){
      return cryptonote::get_block_longhash(NULL, b, hash, height, threads);
    }, bl, 1, 0);
    bl.invalidate_hashes();
    return true;
  }
  //---------------------------------------------------------------
  void get_altblock_longhash(const block& b, crypto::hash& res, const uint64_t main_height, const uint64_t height, const uint64_t seed_height, const crypto::hash& seed_hash)
  {
    blobdata bd = get_block_hashing_blob(b);
    rx_slow_hash(main_height, seed_height, seed_hash.data, bd.data(), bd.size(), res.data, 0, 1);
  }

  bool get_block_longhash(const Blockchain *pbc, const block& b, crypto::hash& res, const uint64_t height, const int miners)
  {
    block b_local = b; //workaround to avoid const errors with do_serialize
    blobdata bd = get_block_hashing_blob(b);
    cn_pow_hash_v3 ctx;
    if(b_local.major_version >= CRYPTONOTE_V3_POW_BLOCK_VERSION)
    {
      ctx.hash(bd.data(), bd.size(), res.data);
    }
    else if(b_local.major_version == CRYPTONOTE_V2_POW_BLOCK_VERSION)
    {
      cn_pow_hash_v2 ctx_v2 = cn_pow_hash_v2::make_borrowed_v2(ctx);
      ctx_v2.hash(bd.data(), bd.size(), res.data);
    }
    else
    {
      cn_pow_hash_v1 ctx_v1 = cn_pow_hash_v1::make_borrowed_v1(ctx);
      ctx_v1.hash(bd.data(), bd.size(), res.data);
    }
    return true;
  }

  crypto::hash get_block_longhash(const Blockchain *pbc, const block& b, const uint64_t height, const int miners)
  {
    crypto::hash p = crypto::null_hash;
    get_block_longhash(pbc, b, p, height, miners);
    return p;
  }

  void get_block_longhash_reorg(const uint64_t split_height)
  {
    rx_reorg(split_height);
  }
}
