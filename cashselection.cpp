// Copyright (c) 2018 The SalemCash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/cashselection.h>
#include <util.h>
#include <utilmoneystr.h>

// Descending order comparator
struct {
    bool operator()(const CInputCash& a, const CInputCash& b) const
    {
        return a.effective_value > b.effective_value;
    }
} descending;

/*
 * This is the Branch and Bound Coin/Cash Selection algorithm designed by Murch. Note that the SalemCash 
 * developers opted to use the term "Cash" instead of "coin" in the algorithm. It searches for an input
 * set that can pay for the spending target and does not exceed the spending target by more than the
 * cost of creating and spending a change output. The algorithm uses a depth-first search on a binary
 * tree. In the binary tree, each node corresponds to the inclusion or the omission of a UTXO. UTXOs
 * are sorted by their effective values and the trees is explored deterministically per the inclusion
 * branch first. At each node, the algorithm checks whether the selection is within the target range.
 * While the selection has not reached the target range, more UTXOs are included. When a selection's
 * value exceeds the target range, the complete subtree deriving from this selection can be omitted.
 * At that point, the last included UTXO is deselected and the corresponding omission branch explored
 * instead. The search ends after the complete tree has been searched or after a limited number of tries.
 *
 * The search continues to search for better solutions after one solution has been found. The best
 * solution is chosen by minimizing the waste metric. The waste metric is defined as the cost to
 * spend the current inputs at the given fee rate minus the long term expected cost to spend the
 * inputs, plus the amount the selection exceeds the spending target:
 *
 * waste = selectionTotal - target + inputs × (currentFeeRate - longTermFeeRate)
 *
 * The algorithm uses two additional optimizations. A lookahead keeps track of the total value of
 * the unexplored UTXOs. A subtree is not explored if the lookahead indicates that the target range
 * cannot be reached. Further, it is unnecessary to test equivalent combinations. This allows us
 * to skip testing the inclusion of UTXOs that match the effective value and waste of an omitted
 * predecessor.
 *
 * The Branch and Bound algorithm is described in detail in Murch's Master Thesis:
 * https://murch.one/wp-content/uploads/2016/11/erhardt2016coinselection.pdf
 *
 * @param const std::vector<CInputCash>& utxo_pool The set of UTXOs that we are choosing from.
 *        These UTXOs will be sorted in descending order by effective value and the CInputCash
 *        values are their effective values.
 * @param const CAmount& target_value This is the value that we want to select. It is the lower
 *        bound of the range.
 * @param const CAmount& cost_of_change This is the cost of creating and spending a change output.
 *        This plus target_value is the upper bound of the range.
 * @param std::set<CInputCash>& out_set -> This is an output parameter for the set of CInputCash
 *        that have been selected.
 * @param CAmount& value_ret -> This is an output parameter for the total value of the CInputCash
 *        that were selected.
 * @param CAmount not_input_fees -> The fees that need to be paid for the outputs and fixed size
 *        overhead (version, locktime, marker and flag)
 */

static const size_t TOTAL_TRIES = 100000;

bool SelectCashBnB(std::vector<CInputCash>& utxo_pool, const CAmount& target_value, const CAmount& cost_of_change, std::set<CInputCash>& out_set, CAmount& value_ret, CAmount not_input_fees)
{
    out_set.clear();
    CAmount curr_value = 0;

    std::vector<bool> curr_selection; // select the utxo at this index
    curr_selection.reserve(utxo_pool.size());
    CAmount actual_target = not_input_fees + target_value;

    // Calculate curr_available_value
    CAmount curr_available_value = 0;
    for (const CInputCash& utxo : utxo_pool) {
        // Assert that this utxo is not negative. It should never be negative, effective value calculation should have removed it
        assert(utxo.effective_value > 0);
        curr_available_value += utxo.effective_value;
    }
    if (curr_available_value < actual_target) {
        return false;
    }

    // Sort the utxo_pool
    std::sort(utxo_pool.begin(), utxo_pool.end(), descending);

    CAmount curr_waste = 0;
    std::vector<bool> best_selection;
    CAmount best_waste = MAX_MONEY;

    // Depth First search loop for choosing the UTXOs
    for (size_t i = 0; i < TOTAL_TRIES; ++i) {
        // Conditions for starting a backtrack
        bool backtrack = false;
        if (curr_value + curr_available_value < actual_target ||                // Cannot possibly reach target with the amount remaining in the curr_available_value.
            curr_value > actual_target + cost_of_change ||    // Selected value is out of range, go back and try other branch
            (curr_waste > best_waste && (utxo_pool.at(0).fee - utxo_pool.at(0).long_term_fee) > 0)) { // Don't select things which we know will be more wasteful if the waste is increasing
            backtrack = true;
        } else if (curr_value >= actual_target) {       // Selected value is within range
            curr_waste += (curr_value - actual_target); // This is the excess value which is added to the waste for the below comparison
            // Adding another UTXO after this check could bring the waste down if the long term fee is higher than the current fee.
            // However we are not going to explore that because this optimization for the waste is only done when we have hit our target
            // value. Adding any more UTXOs will be just burning the UTXO; it will go entirely to fees. Thus we aren't going to
            // explore any more UTXOs to avoid burning money like that.
            if (curr_waste <= best_waste) {
                best_selection = curr_selection;
                best_selection.resize(utxo_pool.size());
                best_waste = curr_waste;
            }
            curr_waste -= (curr_value - actual_target); // Remove the excess value as we will be selecting different cash value now
            backtrack = true;
        }

        // Backtracking, moving backwards
        if (backtrack) {
            // Walk backwards to find the last included UTXO that still needs to have its omission branch traversed.
            while (!curr_selection.empty() && !curr_selection.back()) {
                curr_selection.pop_back();
                curr_available_value += utxo_pool.at(curr_selection.size()).effective_value;
            };

            if (curr_selection.empty()) { // We have walked back to the first utxo and no branch is untraversed. All solutions searched
                break;
            }

            // Output was included on previous iterations, try excluding now.
            curr_selection.back() = false;
            CInputCash& utxo = utxo_pool.at(curr_selection.size() - 1);
            curr_value -= utxo.effective_value;
            curr_waste -= utxo.fee - utxo.long_term_fee;
        } else { // Moving forwards, continuing down this branch
            CInputCash& utxo = utxo_pool.at(curr_selection.size());

            // Remove this utxo from the curr_available_value utxo amount
            curr_available_value -= utxo.effective_value;

            // Avoid searching a branch if the previous UTXO has the same value and same waste and was excluded. Since the ratio of fee to
            // long term fee is the same, we only need to check if one of those values match in order to know that the waste is the same.
            if (!curr_selection.empty() && !curr_selection.back() &&
                utxo.effective_value == utxo_pool.at(curr_selection.size() - 1).effective_value &&
                utxo.fee == utxo_pool.at(curr_selection.size() - 1).fee) {
                curr_selection.push_back(false);
            } else {
                // Inclusion branch first (Largest First Exploration)
                curr_selection.push_back(true);
                curr_value += utxo.effective_value;
                curr_waste += utxo.fee - utxo.long_term_fee;
            }
        }
    }

    // Check for solution
    if (best_selection.empty()) {
        return false;
    }

    // Set output set
    value_ret = 0;
    for (size_t i = 0; i < best_selection.size(); ++i) {
        if (best_selection.at(i)) {
            out_set.insert(utxo_pool.at(i));
            value_ret += utxo_pool.at(i).txout.nValue;
        }
    }

    return true;
}

static void ApproximateBestSubset(const std::vector<CInputCash>& vValue, const CAmount& nTotalLower, const CAmount& nTargetValue,
                                  std::vector<char>& vfBest, CAmount& nBest, int iterations = 1000)
{
    std::vector<char> vfIncluded;

    vfBest.assign(vValue.size(), true);
    nBest = nTotalLower;

    FastRandomContext insecure_rand;

    for (int nRep = 0; nRep < iterations && nBest != nTargetValue; nRep++)
    {
        vfIncluded.assign(vValue.size(), false);
        CAmount nTotal = 0;
        bool fReachedTarget = false;
        for (int nPass = 0; nPass < 2 && !fReachedTarget; nPass++)
        {
            for (unsigned int i = 0; i < vValue.size(); i++)
            {
                //The solver here uses a randomized algorithm,
                //the randomness serves no real security purpose but is just
                //needed to prevent degenerate behavior and it is important
                //that the rng is fast. We do not use a constant random sequence,
                //because there may be some privacy improvement by making
                //the selection random.
                if (nPass == 0 ? insecure_rand.randbool() : !vfIncluded[i])
                {
                    nTotal += vValue[i].txout.nValue;
                    vfIncluded[i] = true;
                    if (nTotal >= nTargetValue)
                    {
                        fReachedTarget = true;
                        if (nTotal < nBest)
                        {
                            nBest = nTotal;
                            vfBest = vfIncluded;
                        }
                        nTotal -= vValue[i].txout.nValue;
                        vfIncluded[i] = false;
                    }
                }
            }
        }
    }
}

bool KnapsackSolver(const CAmount& nTargetValue, std::vector<CInputCash>& vCash, std::set<CInputCash>& setCashRet, CAmount& nValueRet)
{
    setCashRet.clear();
    nValueRet = 0;

    // List of values less than target
    boost::optional<CInputCash> cashLowestLarger;
    std::vector<CInputCash> vValue;
    CAmount nTotalLower = 0;

    random_shuffle(vCash.begin(), vCash.end(), GetRandInt);

    for (const CInputCash &cash : vCash)
    {
        if (cash.txout.nValue == nTargetValue)
        {
            setCashRet.insert(cash);
            nValueRet += cash.txout.nValue;
            return true;
        }
        else if (cash.txout.nValue < nTargetValue + MIN_CHANGE)
        {
            vValue.push_back(cash);
            nTotalLower += cash.txout.nValue;
        }
        else if (!cashLowestLarger || cash.txout.nValue < cashLowestLarger->txout.nValue)
        {
            cashLowestLarger = cash;
        }
    }

    if (nTotalLower == nTargetValue)
    {
        for (const auto& input : vValue)
        {
            setCashRet.insert(input);
            nValueRet += input.txout.nValue;
        }
        return true;
    }

    if (nTotalLower < nTargetValue)
    {
        if (!cashLowestLarger)
            return false;
        setCashRet.insert(cashLowestLarger.get());
        nValueRet += cashLowestLarger->txout.nValue;
        return true;
    }

    // Solve subset sum by stochastic approximation
    std::sort(vValue.begin(), vValue.end(), descending);
    std::vector<char> vfBest;
    CAmount nBest;

    ApproximateBestSubset(vValue, nTotalLower, nTargetValue, vfBest, nBest);
    if (nBest != nTargetValue && nTotalLower >= nTargetValue + MIN_CHANGE)
        ApproximateBestSubset(vValue, nTotalLower, nTargetValue + MIN_CHANGE, vfBest, nBest);

    // If we have a bigger cash value and (either the stochastic approximation didn't find a good solution,
    //                                   or the next bigger cash value is closer), return the bigger cash value
    if (cashLowestLarger &&
        ((nBest != nTargetValue && nBest < nTargetValue + MIN_CHANGE) || cashLowestLarger->txout.nValue <= nBest))
    {
        setCashRet.insert(cashLowestLarger.get());
        nValueRet += cashLowestLarger->txout.nValue;
    }
    else {
        for (unsigned int i = 0; i < vValue.size(); i++)
            if (vfBest[i])
            {
                setCashRet.insert(vValue[i]);
                nValueRet += vValue[i].txout.nValue;
            }

        if (LogAcceptCategory(BCLog::SELECTCASH)) {
            LogPrint(BCLog::SELECTCASH, "SelectCash() best subset: ");
            for (unsigned int i = 0; i < vValue.size(); i++) {
                if (vfBest[i]) {
                    LogPrint(BCLog::SELECTCASH, "%s ", FormatMoney(vValue[i].txout.nValue));
                }
            }
            LogPrint(BCLog::SELECTCASH, "total %s\n", FormatMoney(nBest));
        }
    }

    return true;
}
