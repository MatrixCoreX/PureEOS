# Keep the legacy ABI wording for the default build; do not describe the
# experimental profile's vote restrictions as active on existing deployments.
if(PUREEOS_SINGLE_CHOICE_VOTING)
  set(REWARD_POLICY_RULES "PureEOS cancels the former 80% savings issuance. Rewards activate at the first positive direct producer vote, including one core-token smallest unit of owned CPU/NET stake or settled REX vote stake; there is no 15% or fixed-token-supply threshold. Zero-weight selections do not activate rewards. An upgraded, previously unactivated chain with existing positive votes activates under the new code without another vote or retroactive funding. An existing activation clock is never reset by upgrading, restarting or withdrawing votes. New rewards start at ln(1.05)/5 per legacy 364-day accounting year (approximately 1%), halving every 1,461 days from the first reward activation. Accrual crossing a boundary is split between eras and uses the currently issued token supply, not unissued or unmapped tokens. All new funding goes to block pay, distributed in exact integer proportion to unpaid produced blocks; votes do not enter the payout calculation. Zero unpaid blocks cannot unlock rewards. Vote pay, its minimum-payment threshold, and its share accumulation are disabled. Historical balances are not confiscated: any pre-upgrade vote pool remains unchanged and is not paid out by this action. The issuance cap and existing treasury fallback still apply. Initial registration also starts the existing 24-hour claim cooldown.")
  set(REWARD_ADMIN_RULES "In this PureEOS profile this compatibility action always rejects. The fixed halving policy cannot be overridden through legacy inflation/pay-factor/schedule actions; changes require reviewed system code.")
  set(REWARD_REVISION_RULES "In this PureEOS profile vote-pay revision changes are disabled. Rewards depend only on produced blocks; a legacy revision cannot enable vote pay.")
  set(REX_FROM_VOTE_RULE "Before buying REX, {{from}} must directly select exactly one active registered producer with usable block signing authority. Proxy voting is disabled.")
  set(REX_OWNER_VOTE_RULE "Before converting stake to REX, {{owner}}, not the resource receiver, must directly select exactly one active registered producer with usable block signing authority. Proxy voting is disabled.")
  set(REX_HOLDING_VOTE_RULE "While any REX shares remain, including savings and unfilled sell orders, the owner may switch directly to another eligible producer but cannot clear the selection. After all shares are sold and pending vote-stake changes are settled, the owner may clear the vote without withdrawing the personal REX fund. An existing producer becoming inactive does not prevent normal REX redemption.")
  set(REGPROXY_SUMMARY "Proxy voting is disabled; this action always rejects")
  set(REGPROXY_RULES "This compatibility action rejects both registration and unregistration. It cannot create, forward or aggregate proxy votes.")
  set(VOTEPRODUCER_SUMMARY "{{nowrap voter}} selects at most one producer directly, or clears an eligible vote")
  set(VOTEPRODUCER_RULES [=[
The voter account's own authorization is required, including for b1. The proxy must be empty and the producers list must contain zero or one entry. A new selection must be an active registered producer with usable block signing authority; it need not already have votes or an active committee seat.

A zero-balance voter can create a selection at its own RAM expense before buying REX; this creates no vote weight. Any REX holding, including immature, savings and queued-to-sell shares, prevents clearing the selection. Switching directly to another eligible producer is atomic. Fully sold REX and pending filled-order stake changes must be settled before clearing.

Votes use versioned integer tables in core-token smallest units, without a time multiplier. Each voter allocates owned CPU/NET stake plus settled REX vote stake to at most one producer. Repeating a selection cannot allocate the principal twice; changing a selection subtracts the old allocation and adds the new allocation atomically. Filled REX sell orders immediately update their owner's allocation.

The first positive allocation activates rewards without a fixed-supply or 15% threshold. A zero-weight choice does not activate them. Activation is one-time and vote removal never resets the halving clock. Existing positive allocations on a previously unactivated integer-v2 chain activate under the upgraded contract without requiring another vote; no pre-upgrade backpay is created.

This profile uses integer-v2 voting tables and does not migrate legacy floating-point voting tables. Votes determine candidate ranking, not rewards. Rewards cancel savings issuance, halve every 1,461 days from first reward activation, and allocate all new funding in proportion to unpaid produced blocks. Vote-pay shares are not created or updated. Zero unpaid blocks cannot unlock reward claims, and separate rank-only bpay distributions are disabled. Pre-upgrade vote-pool balances are preserved but no longer distributed by claimrewards. The unpaid-block check is not by itself proof of finality.
]=])
else()
  set(REWARD_POLICY_RULES "Legacy configurable inflation and producer-pay factors apply.")
  set(REWARD_ADMIN_RULES "")
  set(REWARD_REVISION_RULES "")
  set(REX_FROM_VOTE_RULE "In order for the action to succeed, {{from}} must have voted for a proxy or at least 21 block producers.")
  set(REX_OWNER_VOTE_RULE "In order for the action to succeed, {{owner}} must have voted for a proxy or at least 21 block producers.")
  set(REX_HOLDING_VOTE_RULE "")
  set(REGPROXY_SUMMARY "Register/unregister {{nowrap proxy}} as a proxy account")
  set(REGPROXY_RULES [=[
{{#if isproxy}}
{{proxy}} registers as a proxy that can vote on behalf of accounts that appoint it as their proxy.
{{else}}
{{proxy}} unregisters as a proxy that can vote on behalf of accounts that appoint it as their proxy.
{{/if}}
]=])
  set(VOTEPRODUCER_SUMMARY "{{nowrap voter}} votes for {{#if proxy}}the proxy {{nowrap proxy}}{{else}}up to 30 block producer candidates{{/if}}")
  set(VOTEPRODUCER_RULES [=[
{{#if proxy}}
{{voter}} votes for the proxy {{proxy}}.
At the time of voting the full weight of voter’s staked (CPU + NET) tokens will be cast towards each of the producers voted by {{proxy}}.
{{else}}
{{voter}} votes for the following block producer candidates:

{{#each producers}}
  + {{this}}
{{/each}}

At the time of voting the full weight of voter’s staked (CPU + NET) tokens will be cast towards each of the above producers.
{{/if}}
]=])
endif()
