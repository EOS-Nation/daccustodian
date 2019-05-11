

void auditor::nominatecand(name cand) {
    require_auth(cand);

    _currentState.number_active_candidates++;

    pendingstake_table_t pendingstake(_self, _self.value);
    auto pending = pendingstake.find(cand.value);

    auto reg_candidate = registered_candidates.find(cand.value);
    if (reg_candidate != registered_candidates.end()) {
        eosio_assert(!reg_candidate->is_active, "ERR::NOMINATECAND_ALREADY_REGISTERED::Candidate is already registered and active.");
        registered_candidates.modify(reg_candidate, cand, [&](candidate &c) {
            c.is_active = 1;

            if (pending != pendingstake.end()) {
                c.locked_tokens += pending->quantity;
                pendingstake.erase(pending);
            }
            eosio_assert(c.locked_tokens >= configs().lockupasset, "ERR::NOMINATECAND_INSUFFICIENT_FUNDS_TO_STAKE::Insufficient funds have been staked.");
        });
    } else {
        eosio_assert(pending != pendingstake.end() &&
                     pending->quantity >= configs().lockupasset,
                     "ERR::NOMINATECAND_STAKING_FUNDS_INCOMPLETE::A registering candidate must transfer sufficient tokens to the contract for staking.");

        registered_candidates.emplace(cand, [&](candidate &c) {
            c.candidate_name = cand;
            c.locked_tokens = pending->quantity;
            c.total_votes = 0;
            c.is_active = 1;
        });
        pendingstake.erase(pending);
    }
}

void auditor::withdrawcand(name cand) {
    require_auth(cand);
    removeCandidate(cand, false);
}

void auditor::firecand(name cand, bool lockupStake) {
    require_auth(configs().authaccount);
    removeCandidate(cand, lockupStake);
}

void auditor::unstake(name cand) {
    const auto &reg_candidate = registered_candidates.get(cand.value, "ERR::UNSTAKE_CAND_NOT_REGISTERED::Candidate is not already registered.");
    eosio_assert(!reg_candidate.is_active, "ERR::UNSTAKE_CANNOT_UNSTAKE_FROM_ACTIVE_CAND::Cannot unstake tokens for an active candidate. Call withdrawcand first.");

    eosio_assert(reg_candidate.custodian_end_time_stamp < time_point_sec(now()), "ERR::UNSTAKE_CANNOT_UNSTAKE_UNDER_TIME_LOCK::Cannot unstake tokens before they are unlocked from the time delay.");

    registered_candidates.modify(reg_candidate, cand, [&](candidate &c) {
        // Ensure the candidate's tokens are not locked up for a time delay period.
        // Send back the locked up tokens
        transaction deferredTrans{};

        deferredTrans.actions.emplace_back(
        action(permission_level{_self, "xfer"_n},
               name( TOKEN_CONTRACT ),
               "transfer"_n,
               make_tuple(_self, cand, c.locked_tokens,
                          string("Returning locked up stake. Thank you."))
        )
        );

        deferredTrans.delay_sec = TRANSFER_DELAY;
        deferredTrans.send(cand.value, _self);

        c.locked_tokens = asset(0, configs().lockupasset.symbol);
    });
}

void auditor::resigncust(name cust) {
    require_auth(cust);
    removeCustodian(cust);
}

void auditor::firecust(name cust) {
    require_auth(configs().authaccount);
    removeCustodian(cust);
}

// private methods for the above actions

void auditor::removeCustodian(name cust) {

    custodians_table custodians(_self, _self.value);
    auto elected = custodians.find(cust.value);
    eosio_assert(elected != custodians.end(), "ERR::REMOVECUSTODIAN_NOT_CURRENT_CUSTODIAN::The entered account name is not for a current custodian.");

    eosio::print("Remove custodian from the custodians table.");
    custodians.erase(elected);

    // Remove the candidate from being eligible for the next election period.
    removeCandidate(cust, true);

    // Allocate the next set of candidates to only fill the gap for the missing slot.
    allocateCustodians(true);

    // Update the auths to give control to the new set of custodians.
    setCustodianAuths();
}

void auditor::removeCandidate(name cand, bool lockupStake) {
    _currentState.number_active_candidates--;

    const auto &reg_candidate = registered_candidates.get(cand.value, "ERR::REMOVECANDIDATE_NOT_CURRENT_CANDIDATE::Candidate is not already registered.");

    eosio::print("Remove from nominated candidate by setting them to inactive.");
    // Set the is_active flag to false instead of deleting in order to retain votes if they return as BOS auditors.
    registered_candidates.modify(reg_candidate, cand, [&](candidate &c) {
        c.is_active = 0;
        if (lockupStake) {
            eosio::print("Lockup stake for release delay.");
            c.custodian_end_time_stamp = time_point_sec(now() + configs().lockup_release_time_delay);
        }
    });
}
