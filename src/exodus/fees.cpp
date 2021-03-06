/**
 * @file fees.cpp
 *
 * This file contains code for handling Exodus fees.
 */

#include "exodus/fees.h"

#include "exodus/exodus.h"
#include "exodus/log.h"
#include "exodus/rules.h"
#include "exodus/sp.h"
#include "exodus/sto.h"

#include "leveldb/db.h"

#include "main.h"

#include <limits.h>
#include <stdint.h>

#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>

using namespace exodus;

std::map<uint32_t, int64_t> distributionThresholds;

// Returns the distribution threshold for a property
int64_t CExodusFeeCache::GetDistributionThreshold(const uint32_t &propertyId)
{
    return distributionThresholds[propertyId];
}

// Sets the distribution thresholds to total tokens for a property / EXODUS_FEE_THRESHOLD
void CExodusFeeCache::UpdateDistributionThresholds(uint32_t propertyId)
{
    int64_t distributionThreshold = getTotalTokens(propertyId) / EXODUS_FEE_THRESHOLD;
    if (distributionThreshold <= 0) {
        // protect against zero valued thresholds for low token count properties
        distributionThreshold = 1;
    }
    distributionThresholds[propertyId] = distributionThreshold;
}

// Gets the current amount of the fee cache for a property
int64_t CExodusFeeCache::GetCachedAmount(const uint32_t &propertyId)
{
    assert(pdb);
    // Get the fee history, set is sorted by block so last entry is most recent
    std::set<feeCacheItem> sCacheHistoryItems = GetCacheHistory(propertyId);
    if (!sCacheHistoryItems.empty()) {
        std::set<feeCacheItem>::iterator endIt = sCacheHistoryItems.end();
        --endIt;
        feeCacheItem mostRecentItem = *endIt;
        return mostRecentItem.second;
    } else {
        return 0; // property has never generated a fee
    }
}

// Zeros a property in the fee cache
void CExodusFeeCache::ClearCache(const uint32_t &propertyId, int block)
{
    if (exodus_debug_fees) PrintToLog("ClearCache starting (block %d, property ID %d)...\n", block, propertyId);
    const std::string key = strprintf("%010d", propertyId);
    std::set<feeCacheItem> sCacheHistoryItems = GetCacheHistory(propertyId);
    if (exodus_debug_fees) PrintToLog("   Iterating cache history (%d items)...\n",sCacheHistoryItems.size());
    std::string newValue;
    if (!sCacheHistoryItems.empty()) {
        for (std::set<feeCacheItem>::iterator it = sCacheHistoryItems.begin(); it != sCacheHistoryItems.end(); it++) {
            feeCacheItem tempItem = *it;
            if (tempItem.first == block) continue;
            if (!newValue.empty()) newValue += ",";
            newValue += strprintf("%d:%d", tempItem.first, tempItem.second);
            if (exodus_debug_fees) PrintToLog("      Readding entry: block %d amount %d\n", tempItem.first, tempItem.second);
        }
        if (!newValue.empty()) newValue += ",";
    }
    if (exodus_debug_fees) PrintToLog("   Adding zero valued entry: block %d\n", block);
    newValue += strprintf("%d:%d", block, 0);
    leveldb::Status status = pdb->Put(writeoptions, key, newValue);
    assert(status.ok());
    ++nWritten;

    PruneCache(propertyId, block);

    if (exodus_debug_fees) PrintToLog("Cleared cache for property %d block %d [%s]\n", propertyId, block, status.ToString());
}

// Adds a fee to the cache (eg on a completed trade)
void CExodusFeeCache::AddFee(const uint32_t &propertyId, int block, const int64_t &amount)
{
    if (exodus_debug_fees) PrintToLog("Starting AddFee for prop %d (block %d amount %d)...\n", propertyId, block, amount);

    // Get current cached fee
    int64_t currentCachedAmount = GetCachedAmount(propertyId);
    if (exodus_debug_fees) PrintToLog("   Current cached amount %d\n", currentCachedAmount);

    // Add new fee and rewrite record
    if ((currentCachedAmount > 0) && (amount > std::numeric_limits<int64_t>::max() - currentCachedAmount)) {
        // overflow - there is no way the fee cache should exceed the maximum possible number of tokens, not safe to continue
        const std::string& msg = strprintf("Shutting down due to fee cache overflow (block %d property %d current %d amount %d)\n", block, propertyId, currentCachedAmount, amount);
        PrintToLog(msg);
        if (!GetBoolArg("-overrideforcedshutdown", false)) {
            boost::filesystem::path persistPath = GetDataDir() / "MP_persist";
            if (boost::filesystem::exists(persistPath)) boost::filesystem::remove_all(persistPath); // prevent the node being restarted without a reparse after forced shutdown
            AbortNode(msg, msg);
        }
    }
    int64_t newCachedAmount = currentCachedAmount + amount;

    if (exodus_debug_fees) PrintToLog("   New cached amount %d\n", newCachedAmount);
    const std::string key = strprintf("%010d", propertyId);
    std::set<feeCacheItem> sCacheHistoryItems = GetCacheHistory(propertyId);
    if (exodus_debug_fees) PrintToLog("   Iterating cache history (%d items)...\n",sCacheHistoryItems.size());
    std::string newValue;
    if (!sCacheHistoryItems.empty()) {
        for (std::set<feeCacheItem>::iterator it = sCacheHistoryItems.begin(); it != sCacheHistoryItems.end(); it++) {
            feeCacheItem tempItem = *it;
            if (tempItem.first == block) continue; // this is an older entry for the same block, discard it
            if (!newValue.empty()) newValue += ",";
            newValue += strprintf("%d:%d", tempItem.first, tempItem.second);
            if (exodus_debug_fees) PrintToLog("      Readding entry: block %d amount %d\n", tempItem.first, tempItem.second);
        }
        if (!newValue.empty()) newValue += ",";
    }
    if (exodus_debug_fees) PrintToLog("   Adding requested entry: block %d new amount %d\n", block, newCachedAmount);
    newValue += strprintf("%d:%d", block, newCachedAmount);
    leveldb::Status status = pdb->Put(writeoptions, key, newValue);
    assert(status.ok());
    ++nWritten;
    if (exodus_debug_fees) PrintToLog("AddFee completed for property %d (new=%s [%s])\n", propertyId, newValue, status.ToString());

    // Call for pruning (we only prune when we update a record)
    PruneCache(propertyId, block);

    // Call for cache evaluation (we only need to do this each time a fee cache is increased)
    EvalCache(propertyId, block);

    return;
}

// Rolls back the cache to an earlier state (eg in event of a reorg) - block is *inclusive* (ie entries=block will get deleted)
void CExodusFeeCache::RollBackCache(int block)
{
    assert(pdb);
    for (uint8_t ecosystem = 1; ecosystem <= 2; ecosystem++) {
        uint32_t startPropertyId = (ecosystem == 1) ? 1 : TEST_ECO_PROPERTY_1;
        for (uint32_t propertyId = startPropertyId; propertyId < exodus::_my_sps->peekNextSPID(ecosystem); propertyId++) {
            const std::string key = strprintf("%010d", propertyId);
            std::set<feeCacheItem> sCacheHistoryItems = GetCacheHistory(propertyId);
            if (!sCacheHistoryItems.empty()) {
                std::set<feeCacheItem>::iterator mostRecentIt = sCacheHistoryItems.end();
                std::string newValue;
                --mostRecentIt;
                feeCacheItem mostRecentItem = *mostRecentIt;
                if (mostRecentItem.first < block) continue; // all entries are unaffected by this rollback, nothing to do
                for (std::set<feeCacheItem>::iterator it = sCacheHistoryItems.begin(); it != sCacheHistoryItems.end(); it++) {
                    feeCacheItem tempItem = *it;
                    if (tempItem.first >= block) continue; // discard this entry
                    if (!newValue.empty()) newValue += ",";
                    newValue += strprintf("%d:%d", tempItem.first, tempItem.second);
                }
                leveldb::Status status = pdb->Put(writeoptions, key, newValue);
                assert(status.ok());
                PrintToLog("Rolling back fee cache for property %d, new=%s [%s])\n", propertyId, newValue, status.ToString());
            }
        }
    }
}

// Evaluates fee caches for the property against threshold and executes distribution if threshold met
void CExodusFeeCache::EvalCache(const uint32_t &propertyId, int block)
{
    if (GetCachedAmount(propertyId) >= distributionThresholds[propertyId]) {
        DistributeCache(propertyId, block);
    }
}

// Performs distribution of fees
void CExodusFeeCache::DistributeCache(const uint32_t &propertyId, int block)
{
    LOCK(cs_tally);

    int64_t cachedAmount = GetCachedAmount(propertyId);

    if (cachedAmount == 0) {
        PrintToLog("Aborting fee distribution for property %d, the fee cache is empty!\n", propertyId);
    }

    OwnerAddrType receiversSet;
    if (isTestEcosystemProperty(propertyId)) {
        receiversSet = STO_GetReceivers("FEEDISTRIBUTION", EXODUS_PROPERTY_TEXODUS, cachedAmount);
    } else {
        receiversSet = STO_GetReceivers("FEEDISTRIBUTION", EXODUS_PROPERTY_EXODUS, cachedAmount);
    }

    uint64_t numberOfReceivers = receiversSet.size(); // there will always be addresses holding EXODUS, so no need to check size>0
    PrintToLog("Starting fee distribution for property %d to %d recipients...\n", propertyId, numberOfReceivers);

    int64_t sent_so_far = 0;
    std::set<feeHistoryItem> historyItems;
    for (OwnerAddrType::reverse_iterator it = receiversSet.rbegin(); it != receiversSet.rend(); ++it) {
        const std::string& address = it->second;
        int64_t will_really_receive = it->first;
        sent_so_far += will_really_receive;
        if (exodus_debug_fees) PrintToLog("  %s receives %d (running total %d of %d)\n", address, will_really_receive, sent_so_far, cachedAmount);
        assert(update_tally_map(address, propertyId, will_really_receive, BALANCE));
        feeHistoryItem recipient(address, will_really_receive);
        historyItems.insert(recipient);
    }

    PrintToLog("Fee distribution completed, distributed %d out of %d\n", sent_so_far, cachedAmount);

    // store the fee distribution
    p_feehistory->RecordFeeDistribution(propertyId, block, sent_so_far, historyItems);

    // final check to ensure the entire fee cache was distributed, then empty the cache
    assert(sent_so_far == cachedAmount);
    ClearCache(propertyId, block);
}

// Prunes entries over MAX_STATE_HISTORY blocks old from the entry for a property
void CExodusFeeCache::PruneCache(const uint32_t &propertyId, int block)
{
    if (exodus_debug_fees) PrintToLog("Starting PruneCache for prop %d block %d...\n", propertyId, block);
    assert(pdb);

    int pruneBlock = block - MAX_STATE_HISTORY;
    if (exodus_debug_fees) PrintToLog("Removing entries prior to block %d...\n", pruneBlock);
    const std::string key = strprintf("%010d", propertyId);
    std::set<feeCacheItem> sCacheHistoryItems = GetCacheHistory(propertyId);
    if (exodus_debug_fees) PrintToLog("   Iterating cache history (%d items)...\n",sCacheHistoryItems.size());
    if (!sCacheHistoryItems.empty()) {
        std::set<feeCacheItem>::iterator startIt = sCacheHistoryItems.begin();
        feeCacheItem firstItem = *startIt;
        if (firstItem.first >= pruneBlock) {
            if (exodus_debug_fees) PrintToLog("Endingg PruneCache - no matured entries found.\n");
            return; // all entries are above supplied block value, nothing to do
        }
        std::string newValue;
        for (std::set<feeCacheItem>::iterator it = sCacheHistoryItems.begin(); it != sCacheHistoryItems.end(); it++) {
            feeCacheItem tempItem = *it;
            if (tempItem.first < pruneBlock) {
                if (exodus_debug_fees) {
                    PrintToLog("      Skipping matured entry: block %d amount %d\n", tempItem.first, tempItem.second);
                    continue; // discard this entry
                }
            }
            if (!newValue.empty()) newValue += ",";
            newValue += strprintf("%d:%d", tempItem.first, tempItem.second);
            if (exodus_debug_fees) PrintToLog("      Readding immature entry: block %d amount %d\n", tempItem.first, tempItem.second);
        }
        // make sure the pruned cache isn't completely empty, if it is, prune down to just the most recent entry
        if (newValue.empty()) {
            std::set<feeCacheItem>::iterator mostRecentIt = sCacheHistoryItems.end();
            --mostRecentIt;
            feeCacheItem mostRecentItem = *mostRecentIt;
            newValue = strprintf("%d:%d", mostRecentItem.first, mostRecentItem.second);
            if (exodus_debug_fees) PrintToLog("   All entries matured and pruned - readding most recent entry: block %d amount %d\n", mostRecentItem.first, mostRecentItem.second);
        }
        leveldb::Status status = pdb->Put(writeoptions, key, newValue);
        assert(status.ok());
        if (exodus_debug_fees) PrintToLog("PruneCache completed for property %d (new=%s [%s])\n", propertyId, newValue, status.ToString());
    } else {
        return; // nothing to do
    }
}

// Show Fee Cache DB statistics
void CExodusFeeCache::printStats()
{
    PrintToConsole("CExodusFeeCache stats: nWritten= %d , nRead= %d\n", nWritten, nRead);
}

// Show Fee Cache DB records
void CExodusFeeCache::printAll()
{
    int count = 0;
    leveldb::Iterator* it = NewIterator();
    for(it->SeekToFirst(); it->Valid(); it->Next()) {
        ++count;
        PrintToConsole("entry #%8d= %s:%s\n", count, it->key().ToString(), it->value().ToString());
    }
    delete it;
}

// Return a set containing fee cache history items
std::set<feeCacheItem> CExodusFeeCache::GetCacheHistory(const uint32_t &propertyId)
{
    assert(pdb);

    const std::string key = strprintf("%010d", propertyId);

    std::set<feeCacheItem> sCacheHistoryItems;
    std::string strValue;
    leveldb::Status status = pdb->Get(readoptions, key, &strValue);
    if (status.IsNotFound()) {
        return sCacheHistoryItems; // no cache, return empty set
    }
    assert(status.ok());
    std::vector<std::string> vCacheHistoryItems;
    boost::split(vCacheHistoryItems, strValue, boost::is_any_of(","), boost::token_compress_on);
    for (std::vector<std::string>::iterator it = vCacheHistoryItems.begin(); it != vCacheHistoryItems.end(); ++it) {
        std::vector<std::string> vCacheHistoryItem;
        boost::split(vCacheHistoryItem, *it, boost::is_any_of(":"), boost::token_compress_on);
        if (2 != vCacheHistoryItem.size()) {
            PrintToConsole("ERROR: vCacheHistoryItem has unexpected number of elements: %d (raw %s)!\n", vCacheHistoryItem.size(), *it);
            printAll();
            continue;
        }
        int64_t cacheItemBlock = boost::lexical_cast<int64_t>(vCacheHistoryItem[0]);
        int64_t cacheItemAmount = boost::lexical_cast<int64_t>(vCacheHistoryItem[1]);
        sCacheHistoryItems.insert(std::make_pair(cacheItemBlock, cacheItemAmount));
    }

    return sCacheHistoryItems;
}

// Show Fee History DB statistics
void CExodusFeeHistory::printStats()
{
    PrintToConsole("CExodusFeeHistory stats: nWritten= %d , nRead= %d\n", nWritten, nRead);
}

// Show Fee History DB records
void CExodusFeeHistory::printAll()
{
    int count = 0;
    leveldb::Iterator* it = NewIterator();
    for(it->SeekToFirst(); it->Valid(); it->Next()) {
        ++count;
        PrintToConsole("entry #%8d= %s-%s\n", count, it->key().ToString(), it->value().ToString());
        PrintToLog("entry #%8d= %s-%s\n", count, it->key().ToString(), it->value().ToString());
    }
    delete it;
}

// Count Fee History DB records
int CExodusFeeHistory::CountRecords()
{
    // No faster way to count than to iterate - "There is no way to implement Count more efficiently inside leveldb than outside."
    int count = 0;
    leveldb::Iterator* it = NewIterator();
    for(it->SeekToFirst(); it->Valid(); it->Next()) {
        ++count;
    }
    delete it;
    return count;
}

// Roll back history in event of reorg, block is inclusive
void CExodusFeeHistory::RollBackHistory(int block)
{
    assert(pdb);

    std::set<int> sDistributions;
    leveldb::Iterator* it = NewIterator();
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        std::string strValue = it->value().ToString();
        std::string strKey = it->key().ToString();
        std::vector<std::string> vFeeHistoryDetail;
        boost::split(vFeeHistoryDetail, strValue, boost::is_any_of(":"), boost::token_compress_on);
        if (4 != vFeeHistoryDetail.size()) {
            PrintToLog("ERROR: vFeeHistoryDetail has unexpected number of elements: %d !\n", vFeeHistoryDetail.size());
            continue; // bad data
        }
        int feeBlock = boost::lexical_cast<int>(vFeeHistoryDetail[0]);
        if (feeBlock >= block) {
            PrintToLog("%s() deleting from fee history DB: %s %s\n", __FUNCTION__, strKey, strValue);
            pdb->Delete(writeoptions, strKey);
        }
    }
    delete it;
}

// Retrieve fee distributions for a property
std::set<int> CExodusFeeHistory::GetDistributionsForProperty(const uint32_t &propertyId)
{
    assert(pdb);

    std::set<int> sDistributions;
    leveldb::Iterator* it = NewIterator();
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        std::string strValue = it->value().ToString();
        std::vector<std::string> vFeeHistoryDetail;
        boost::split(vFeeHistoryDetail, strValue, boost::is_any_of(":"), boost::token_compress_on);
        if (4 != vFeeHistoryDetail.size()) {
            PrintToConsole("ERROR: vFeeHistoryDetail has unexpected number of elements: %d !\n", vFeeHistoryDetail.size());
            printAll();
            continue; // bad data
        }
        uint32_t prop = boost::lexical_cast<uint32_t>(vFeeHistoryDetail[1]);
        if (prop == propertyId) {
            std::string key = it->key().ToString();
            int id = boost::lexical_cast<int>(key);
            sDistributions.insert(id);
        }
    }
    delete it;
    return sDistributions;
}

// Populate data about a fee distribution
bool CExodusFeeHistory::GetDistributionData(int id, uint32_t *propertyId, int *block, int64_t *total)
{
    assert(pdb);

    const std::string key = strprintf("%d", id);
    std::string strValue;
    leveldb::Status status = pdb->Get(readoptions, key, &strValue);
    if (status.IsNotFound()) {
        return false; // fee distribution not found
    }
    assert(status.ok());
    std::vector<std::string> vFeeHistoryDetail;
    boost::split(vFeeHistoryDetail, strValue, boost::is_any_of(":"), boost::token_compress_on);
    if (4 != vFeeHistoryDetail.size()) {
        PrintToConsole("ERROR: vFeeHistoryDetail has unexpected number of elements: %d !\n", vFeeHistoryDetail.size());
        printAll();
        return false; // bad data
    }
    *block = boost::lexical_cast<int>(vFeeHistoryDetail[0]);
    *propertyId = boost::lexical_cast<uint32_t>(vFeeHistoryDetail[1]);
    *total = boost::lexical_cast<int64_t>(vFeeHistoryDetail[2]);
    return true;
}

// Retrieve the recipients for a fee distribution
std::set<feeHistoryItem> CExodusFeeHistory::GetFeeDistribution(int id)
{
    assert(pdb);

    const std::string key = strprintf("%d", id);
    std::set<feeHistoryItem> sFeeHistoryItems;
    std::string strValue;
    leveldb::Status status = pdb->Get(readoptions, key, &strValue);
    if (status.IsNotFound()) {
        return sFeeHistoryItems; // fee distribution not found, return empty set
    }
    assert(status.ok());
    std::vector<std::string> vFeeHistoryDetail;
    boost::split(vFeeHistoryDetail, strValue, boost::is_any_of(":"), boost::token_compress_on);
    if (4 != vFeeHistoryDetail.size()) {
        PrintToConsole("ERROR: vFeeHistoryDetail has unexpected number of elements: %d !\n", vFeeHistoryDetail.size());
        printAll();
        return sFeeHistoryItems; // bad data, return empty set
    }
    std::vector<std::string> vFeeHistoryItems;
    boost::split(vFeeHistoryItems, vFeeHistoryDetail[3], boost::is_any_of(","), boost::token_compress_on);
    for (std::vector<std::string>::iterator it = vFeeHistoryItems.begin(); it != vFeeHistoryItems.end(); ++it) {
        std::vector<std::string> vFeeHistoryItem;
        boost::split(vFeeHistoryItem, *it, boost::is_any_of("="), boost::token_compress_on);
        if (2 != vFeeHistoryItem.size()) {
            PrintToConsole("ERROR: vFeeHistoryItem has unexpected number of elements: %d (raw %s)!\n", vFeeHistoryItem.size(), *it);
            printAll();
            continue;
        }
        int64_t feeHistoryItemAmount = boost::lexical_cast<int64_t>(vFeeHistoryItem[1]);
        sFeeHistoryItems.insert(std::make_pair(vFeeHistoryItem[0], feeHistoryItemAmount));
    }

    return sFeeHistoryItems;
}

// Record a fee distribution
void CExodusFeeHistory::RecordFeeDistribution(const uint32_t &propertyId, int block, int64_t total, std::set<feeHistoryItem> feeRecipients)
{
    assert(pdb);

    int count = CountRecords() + 1;
    std::string key = strprintf("%d", count);
    std::string feeRecipientsStr;

    if (!feeRecipients.empty()) {
        for (std::set<feeHistoryItem>::iterator it = feeRecipients.begin(); it != feeRecipients.end(); it++) {
            feeHistoryItem tempRecipient = *it;
            feeRecipientsStr += strprintf("%s=%d,", tempRecipient.first, tempRecipient.second);
        }
        if (feeRecipientsStr.size() > 0) {
            feeRecipientsStr.resize(feeRecipientsStr.size() - 1);
        }
    }

    std::string value = strprintf("%d:%d:%d:%s", block, propertyId, total, feeRecipientsStr);
    leveldb::Status status = pdb->Put(writeoptions, key, value);
    if (exodus_debug_fees) PrintToLog("Added fee distribution to feeCacheHistory - key=%s value=%s [%s]\n", key, value, status.ToString());
}

